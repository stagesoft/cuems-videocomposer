#include "HAPVideoInput.h"
#include "../../ffcompat.h"
#include "../utils/CLegacyBridge.h"
#include "../utils/Logger.h"
#include <cstring>
#include <cassert>
#include <algorithm>

// OpenGL includes
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

// HAP uses S3TC compressed textures
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
}

namespace videocomposer {

HAPVideoInput::HAPVideoInput()
    : formatCtx_(nullptr)
    , codecCtx_(nullptr)
    , frame_(nullptr)
    , videoStream_(-1)
    , frameIndex_(nullptr)
    , frameCount_(0)
    , lastDecodedPTS_(-1)
    , lastDecodedFrameNo_(-1)
    , scanComplete_(false)
    , currentFrame_(-1)
    , hapVariant_(HAPVariant::HAP)
    , ready_(false)
{
    frameRateQ_ = {1, 1};
    frameInfo_ = {};
}

HAPVideoInput::~HAPVideoInput() {
    close();
}

bool HAPVideoInput::initializeFFmpeg() {
    // FFmpeg should already be initialized globally
    return true;
}

bool HAPVideoInput::open(const std::string& source) {
    if (source.empty()) {
        return false;
    }

    // Close any existing file
    close();

    currentFile_ = source;
    ready_ = false;

    // Initialize FFmpeg
    if (!initializeFFmpeg()) {
        return false;
    }

    // Open video file
    if (avformat_open_input(&formatCtx_, source.c_str(), nullptr, nullptr) != 0) {
        formatCtx_ = nullptr;
        return false;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(formatCtx_, nullptr) < 0) {
        avformat_close_input(&formatCtx_);
        formatCtx_ = nullptr;
        return false;
    }

    // Find video stream
    if (!findVideoStream()) {
        avformat_close_input(&formatCtx_);
        formatCtx_ = nullptr;
        return false;
    }

    // Verify this is a HAP codec
    // Note: FFmpeg may use AV_CODEC_ID_HAP for all HAP variants
    // The variant (HAP, HAP_Q, HAP_ALPHA) is determined later by examining the codec
    AVStream* avStream = formatCtx_->streams[videoStream_];
    AVCodecID codecId = avStream->codec->codec_id;
    
    // Check for HAP codec (all variants use AV_CODEC_ID_HAP in some FFmpeg versions)
    // We'll detect the specific variant after opening the codec
    if (codecId != AV_CODEC_ID_HAP) {
        // Some FFmpeg versions might have separate IDs - check if available
        #ifdef AV_CODEC_ID_HAPQ
        if (codecId != AV_CODEC_ID_HAPQ && codecId != AV_CODEC_ID_HAPALPHA) {
            avformat_close_input(&formatCtx_);
            formatCtx_ = nullptr;
            return false; // Not a HAP file
        }
        #else
        avformat_close_input(&formatCtx_);
        formatCtx_ = nullptr;
        return false; // Not a HAP file
        #endif
    }

    // Open codec
    if (!openCodec()) {
        avformat_close_input(&formatCtx_);
        formatCtx_ = nullptr;
        return false;
    }

    // Detect HAP variant
    hapVariant_ = detectHAPVariant();

    // Get video properties
    // Frame rate
    double framerate = 0.0;
    if (avStream->r_frame_rate.den > 0 && avStream->r_frame_rate.num > 0) {
        framerate = av_q2d(avStream->r_frame_rate);
        frameRateQ_.den = avStream->r_frame_rate.num;
        frameRateQ_.num = avStream->r_frame_rate.den;
    }

    // Dimensions
    int width = codecCtx_->width;
    int height = codecCtx_->height;
    
    // Duration
    double duration = 0.0;
    if (formatCtx_->duration != AV_NOPTS_VALUE) {
        duration = (double)formatCtx_->duration / AV_TIME_BASE;
    }

    // Store frame info
    frameInfo_.width = width;
    frameInfo_.height = height;
    frameInfo_.aspect = (float)width / (float)height;
    frameInfo_.framerate = framerate;
    frameInfo_.duration = duration;
    // HAP uses compressed texture format, not traditional pixel format
    frameInfo_.format = PixelFormat::BGRA32; // Placeholder

    // Calculate total frames
    if (framerate > 0 && duration > 0) {
        frameInfo_.totalFrames = static_cast<int64_t>(framerate * duration);
    }

    // Index frames (optional, for faster seeking)
    if (!indexFrames()) {
        // Indexing failed, but we can still proceed
        // However, seeking will fall back to timestamp-based method
        LOG_WARNING << "HAP: Frame indexing failed, will use timestamp-based seeking";
    } else {
        LOG_INFO << "HAP: Frame indexing completed, indexed " << frameCount_ << " frames";
    }

    ready_ = true;
    return true;
}

void HAPVideoInput::close() {
    cleanup();
    ready_ = false;
    currentFile_.clear();
    currentFrame_ = -1;
}

bool HAPVideoInput::isReady() const {
    return ready_;
}

bool HAPVideoInput::findVideoStream() {
    videoStream_ = -1;
    for (unsigned int i = 0; i < formatCtx_->nb_streams; i++) {
        if (formatCtx_->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream_ = i;
            break;
        }
    }
    return videoStream_ >= 0;
}

bool HAPVideoInput::openCodec() {
    AVStream* avStream = formatCtx_->streams[videoStream_];
    codecCtx_ = avStream->codec;

    AVCodec* codec = avcodec_find_decoder(codecCtx_->codec_id);
    if (!codec) {
        return false;
    }

    if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
        return false;
    }

    // Allocate frame
    frame_ = av_frame_alloc();
    if (!frame_) {
        return false;
    }

    return true;
}

HAPVideoInput::HAPVariant HAPVideoInput::detectHAPVariant() {
    if (!codecCtx_) {
        return HAPVariant::HAP;
    }
    
    // Detect HAP variant from codec ID
    // Note: Some FFmpeg versions use AV_CODEC_ID_HAP for all variants
    // We'll need to detect by codec name or by examining the first decoded frame
    AVCodecID codecId = codecCtx_->codec_id;
    
    #ifdef AV_CODEC_ID_HAPALPHA
    if (codecId == AV_CODEC_ID_HAPALPHA) {
        return HAPVariant::HAP_ALPHA;
    }
    #endif
    
    #ifdef AV_CODEC_ID_HAPQ
    if (codecId == AV_CODEC_ID_HAPQ) {
        return HAPVariant::HAP_Q;
    }
    #endif
    
    // If codec ID is HAP, try to detect variant from codec name
    if (codecId == AV_CODEC_ID_HAP && codecCtx_->codec) {
        const char* codecName = codecCtx_->codec->name;
        if (codecName) {
            if (strstr(codecName, "hapalpha") || strstr(codecName, "hap_alpha")) {
                return HAPVariant::HAP_ALPHA;
            } else if (strstr(codecName, "hapq") || strstr(codecName, "hap_q")) {
                return HAPVariant::HAP_Q;
            }
        }
    }
    
    // Default to standard HAP if unknown
    // The actual variant will be determined when decoding the first frame
    // (HAP_ALPHA and HAP_Q use DXT5, standard HAP uses DXT1)
    return HAPVariant::HAP;
}

bool HAPVideoInput::indexFrames() {
    // Full frame indexing for HAP - similar to VideoFileInput
    // This enables faster seeking by indexing all frames
    AVPacket packet;
    av_init_packet(&packet);
    packet.data = nullptr;
    packet.size = 0;

    frameCount_ = 0;
    frameIndex_ = nullptr;

    const size_t initialSize = 10000;
    frameIndex_ = static_cast<FrameIndex*>(calloc(initialSize, sizeof(FrameIndex)));
    if (!frameIndex_) {
        return false;
    }

    size_t indexSize = initialSize;
    AVRational timeBase = formatCtx_->streams[videoStream_]->time_base;

    // Rewind to beginning of file
    if (av_seek_frame(formatCtx_, -1, 0, AVSEEK_FLAG_BACKWARD) < 0) {
        free(frameIndex_);
        frameIndex_ = nullptr;
        return false;
    }

    // Flush codec buffers
    if (codecCtx_->codec->flush) {
        avcodec_flush_buffers(codecCtx_);
    }

    // Scan all frames and build index
    while (av_read_frame(formatCtx_, &packet) >= 0) {
        if (packet.stream_index != videoStream_) {
            av_free_packet(&packet);
            continue;
        }

        int64_t ts = AV_NOPTS_VALUE;
        if (packet.pts != AV_NOPTS_VALUE) {
            ts = packet.pts;
        } else if (packet.dts != AV_NOPTS_VALUE) {
            ts = packet.dts;
        }

        if (ts == AV_NOPTS_VALUE) {
            av_free_packet(&packet);
            continue;
        }

        // Grow array if needed
        if (frameCount_ >= static_cast<int64_t>(indexSize)) {
            indexSize *= 2;
            frameIndex_ = static_cast<FrameIndex*>(realloc(frameIndex_, indexSize * sizeof(FrameIndex)));
            if (!frameIndex_) {
                av_free_packet(&packet);
                return false;
            }
        }

        FrameIndex& idx = frameIndex_[frameCount_];
        idx.pkt_pts = packet.pts;
        idx.pkt_pos = packet.pos;
        idx.frame_pts = ts;
        idx.frame_pos = packet.pos;
        idx.timestamp = frameCount_;
        idx.seekpts = ts;
        idx.seekpos = packet.pos;
        idx.key = (packet.flags & AV_PKT_FLAG_KEY) ? 1 : 0;

        frameCount_++;
        av_free_packet(&packet);
    }

    scanComplete_ = true;
    return true;
}

bool HAPVideoInput::seek(int64_t frameNumber) {
    if (!isReady()) {
        return false;
    }

    currentFrame_ = frameNumber;
    return seekToFrame(frameNumber);
}

bool HAPVideoInput::seekToFrame(int64_t frameNumber) {
    if (!formatCtx_ || videoStream_ < 0) {
        return false;
    }

    // Simple approach: use indexed seek if available, otherwise timestamp-based
    if (scanComplete_ && frameIndex_ && frameNumber >= 0 && frameNumber < frameCount_) {
        const FrameIndex& idx = frameIndex_[frameNumber];
        
        // Check if we need to seek (skip for sequential frames)
        bool needSeek = false;
        if (lastDecodedPTS_ < 0 || lastDecodedFrameNo_ < 0) {
            needSeek = true;
        } else if (lastDecodedPTS_ > idx.seekpts) {
            needSeek = true;
        } else if ((frameNumber - lastDecodedFrameNo_) != 1) {
            if (lastDecodedFrameNo_ >= 0 && lastDecodedFrameNo_ < frameCount_) {
                if (idx.seekpts != frameIndex_[lastDecodedFrameNo_].seekpts) {
                    needSeek = true;
                }
            } else {
                needSeek = true;
            }
        }

        if (needSeek) {
            // Try timestamp-based seek (simplest and most reliable)
            if (idx.seekpts >= 0) {
                int ret = av_seek_frame(formatCtx_, videoStream_, idx.seekpts, AVSEEK_FLAG_BACKWARD);
                if (ret < 0) {
                    return false;
                }
            } else {
                return false;
            }

            if (codecCtx_->codec->flush) {
                avcodec_flush_buffers(codecCtx_);
            }
        }

        lastDecodedPTS_ = -1;
        lastDecodedFrameNo_ = -1;
        currentFrame_ = frameNumber;
        return true;
    }

    // Fallback: timestamp-based seeking
    AVStream* avStream = formatCtx_->streams[videoStream_];
    double framerate = frameInfo_.framerate;
    
    if (framerate <= 0) {
        return false;
    }

    // Calculate timestamp from frame number
    AVRational timeBase = avStream->time_base;
    int64_t timeBaseTimestamp = static_cast<int64_t>((frameNumber / framerate) * AV_TIME_BASE);
    AVRational timeBaseQ = {1, AV_TIME_BASE};
    int64_t timestamp = av_rescale_q(timeBaseTimestamp, timeBaseQ, timeBase);
    
    int ret = av_seek_frame(formatCtx_, videoStream_, timestamp, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        return false;
    }

    if (codecCtx_->codec->flush) {
        avcodec_flush_buffers(codecCtx_);
    }

    lastDecodedPTS_ = -1;
    lastDecodedFrameNo_ = -1;
    currentFrame_ = frameNumber;
    return true;
}

bool HAPVideoInput::readFrame(int64_t frameNumber, FrameBuffer& buffer) {
    // FFmpeg's HAP decoder outputs uncompressed RGBA data (pix_fmt=rgb0), NOT compressed DXT
    // This matches mpv's approach - treat HAP as regular RGBA frames
    // mpv references AVFrame buffers directly with planes[i] = src->data[i] and stride[i] = src->linesize[i]
    //
    // TODO: Review HAP implementation - Are we using CPU upload correctly?
    //       Can we benefit from using compressed textures from HAP directly to GPU?
    //       FFmpeg decodes HAP to uncompressed RGBA, but HAP files contain compressed DXT data.
    //       Could we extract DXT data directly from packets and upload as compressed textures?
    //       This would reduce CPU→GPU bandwidth and memory usage.
    if (!isReady()) {
        return false;
    }

    // Seek to target frame
    if (currentFrame_ < 0 || currentFrame_ != frameNumber) {
        if (!seek(frameNumber)) {
            return false;
        }
    }

    // Decode frame using newer FFmpeg API (matches mpv's approach)
    AVPacket packet;
    av_init_packet(&packet);
    packet.data = nullptr;
    packet.size = 0;

    int bailout = 20;
    bool frameFinished = false;

    while (bailout > 0 && !frameFinished) {
        int err = av_read_frame(formatCtx_, &packet);
        if (err < 0) {
            if (err == AVERROR_EOF) {
                // Flush decoder
                av_init_packet(&packet);
                packet.data = nullptr;
                packet.size = 0;
                err = avcodec_send_packet(codecCtx_, &packet);
                if (err < 0 && err != AVERROR_EOF) {
                    --bailout;
                    continue;
                }
            } else {
                av_free_packet(&packet);
                return false;
            }
        }

        if (packet.stream_index != videoStream_) {
            av_free_packet(&packet);
            continue;
        }

        // Send packet to decoder (newer API - matches mpv)
        err = avcodec_send_packet(codecCtx_, &packet);
        av_free_packet(&packet);

        if (err < 0) {
            if (err != AVERROR(EAGAIN)) {
                --bailout;
                continue;
            }
        }

        // Receive frame from decoder (newer API - matches mpv)
        err = avcodec_receive_frame(codecCtx_, frame_);
        if (err == 0) {
            frameFinished = true;
        } else if (err == AVERROR(EAGAIN)) {
            // Need more input - continue reading packets
            --bailout;
            continue;
        } else if (err == AVERROR_EOF) {
            // Decoder flushed
            --bailout;
            continue;
        } else {
            // Error
            --bailout;
            continue;
        }
    }

    if (!frameFinished) {
        return false;
    }

    if (!frame_->data[0]) {
        return false;
    }

    // FFmpeg decodes HAP to uncompressed RGBA (pix_fmt=rgb0)
    // linesize[0] = width * 4 bytes (RGBA)
    // Total size = linesize[0] * height
    // Matches mpv's mp_image_from_av_frame approach

    FrameInfo hapInfo = frameInfo_;
    hapInfo.format = PixelFormat::RGBA32; // FFmpeg outputs RGBA
    hapInfo.width = frameInfo_.width;
    hapInfo.height = frameInfo_.height;
    
    if (!buffer.allocate(hapInfo)) {
        return false;
    }

    // Copy frame data row by row (matches mpv's memcpy_pic approach)
    // mpv uses linesize as stride, we copy using linesize[0]
    int bytesPerLine = frameInfo_.width * 4; // RGBA = 4 bytes per pixel
    for (int y = 0; y < frameInfo_.height; y++) {
        memcpy(buffer.data() + y * bytesPerLine,
               frame_->data[0] + y * frame_->linesize[0],
               bytesPerLine);
    }
    
    LOG_VERBOSE << "HAP: Copied " << (bytesPerLine * frameInfo_.height) << " bytes RGBA data "
               << "(linesize[0]=" << frame_->linesize[0] << ", width=" << frameInfo_.width 
               << ", height=" << frameInfo_.height << ")";

    int64_t pts = parsePTSFromFrame(frame_);
    if (pts != AV_NOPTS_VALUE) {
        lastDecodedPTS_ = pts;
        lastDecodedFrameNo_ = frameNumber;
    }

    currentFrame_ = frameNumber;
    return true;
}

bool HAPVideoInput::readFrameToTexture(int64_t frameNumber, GPUTextureFrameBuffer& textureBuffer) {
    // DEPRECATED: HAP now uses readFrame() (CPU path) which treats HAP as uncompressed RGBA
    // FFmpeg's HAP decoder outputs uncompressed RGBA (pix_fmt=rgb0), NOT compressed DXT
    // This method is kept for interface compatibility but should not be used for HAP
    // Use readFrame() instead, which uploads to GPU during rendering
    (void)frameNumber;  // Unused
    (void)textureBuffer;  // Unused
    LOG_WARNING << "HAP: readFrameToTexture() is deprecated - HAP now uses uncompressed RGBA path via readFrame()";
    return false;
}

void HAPVideoInput::refineHAPVariantFromFrame(AVFrame* frame) {
    if (!frame || !codecCtx_ || hapVariant_ != HAPVariant::HAP) {
        // Only refine if we defaulted to HAP (unknown variant)
        return;
    }
    
    // HAP frames decode to compressed texture data
    // We can determine the variant by examining the compressed data size
    // HAP_ALPHA and HAP_Q use DXT5 (16 bytes per 4x4 block = 1 byte per pixel)
    // Standard HAP uses DXT1 (8 bytes per 4x4 block = 0.5 bytes per pixel)
    
    // Calculate expected sizes for both variants
    int blockWidth = (frameInfo_.width + 3) / 4;
    int blockHeight = (frameInfo_.height + 3) / 4;
    size_t expectedDXT1Size = blockWidth * blockHeight * 8;   // DXT1
    size_t expectedDXT5Size = blockWidth * blockHeight * 16;  // DXT5
    
    // Get actual compressed data size from frame
    // For HAP, FFmpeg decodes to compressed DXT data in frame->data[0]
    // The packet size is the actual compressed HAP data size before decoding
    // After decoding, we get the full DXT texture data
    // linesize[0] for compressed textures doesn't represent the data size reliably
    // So we'll use the packet size if available, otherwise can't determine
    
    // Note: We can't reliably determine variant from linesize[0] alone
    // because it might not represent the actual decoded data size
    // For now, keep as HAP (DXT1) - variant detection should happen during open()
    // This refinement is not reliable for compressed textures
    return; // Can't reliably determine variant from frame data alone
}

int64_t HAPVideoInput::parsePTSFromFrame(AVFrame* frame) {
    int64_t pts = AV_NOPTS_VALUE;
    if (frame->pts != AV_NOPTS_VALUE) {
        pts = frame->pts;
    } else if (frame->pkt_pts != AV_NOPTS_VALUE) {
        pts = frame->pkt_pts;
    } else if (frame->pkt_dts != AV_NOPTS_VALUE) {
        pts = frame->pkt_dts;
    }
    return pts;
}

FrameInfo HAPVideoInput::getFrameInfo() const {
    return frameInfo_;
}

int64_t HAPVideoInput::getCurrentFrame() const {
    return currentFrame_;
}

InputSource::CodecType HAPVideoInput::detectCodec() const {
    if (hapVariant_ == HAPVariant::HAP_Q) {
        return CodecType::HAP_Q;
    } else if (hapVariant_ == HAPVariant::HAP_ALPHA) {
        return CodecType::HAP_ALPHA;
    } else {
        return CodecType::HAP;
    }
}

bool HAPVideoInput::supportsDirectGPUTexture() const {
    return true; // HAP always supports direct GPU texture
}

InputSource::DecodeBackend HAPVideoInput::getOptimalBackend() const {
    return DecodeBackend::HAP_DIRECT;
}

InputSource::CodecType HAPVideoInput::getHAPVariant() const {
    return detectCodec();
}


void HAPVideoInput::cleanup() {
    // Free frame index
    if (frameIndex_) {
        free(frameIndex_);
        frameIndex_ = nullptr;
    }
    frameCount_ = 0;

    // Free frame (must use av_frame_free for frames allocated with av_frame_alloc)
    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }

    // Close codec
    if (codecCtx_) {
        avcodec_close(codecCtx_);
        codecCtx_ = nullptr;
    }

    // Close format context
    if (formatCtx_) {
        avformat_close_input(&formatCtx_);
        formatCtx_ = nullptr;
    }

    videoStream_ = -1;
    lastDecodedPTS_ = -1;
    lastDecodedFrameNo_ = -1;
    scanComplete_ = false;
    currentFrame_ = -1;
}

} // namespace videocomposer

