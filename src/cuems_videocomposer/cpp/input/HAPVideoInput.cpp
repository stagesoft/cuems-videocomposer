#include "HAPVideoInput.h"
#include "../../ffcompat.h"
#include "../utils/CLegacyBridge.h"
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
    , textureFormat_(0)
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

    // If we have frame indexing, use it for faster seeking
    if (scanComplete_ && frameIndex_ && frameNumber >= 0 && frameNumber < frameCount_) {
        const FrameIndex& idx = frameIndex_[frameNumber];
        
        // Check if we need to seek
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

        lastDecodedPTS_ = -1;
        lastDecodedFrameNo_ = -1;

        if (needSeek) {
            int seekResult;
            if (idx.seekpos > 0) {
                // Try byte-based seeking first (more accurate)
                seekResult = av_seek_frame(formatCtx_, videoStream_, idx.seekpos, 
                                          AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_BYTE);
            } else {
                // Fallback to timestamp-based seeking
                seekResult = av_seek_frame(formatCtx_, videoStream_, idx.seekpts, 
                                          AVSEEK_FLAG_BACKWARD);
            }

            if (codecCtx_->codec->flush) {
                avcodec_flush_buffers(codecCtx_);
            }

            if (seekResult < 0) {
                return false;
            }
        }

        currentFrame_ = frameNumber;
        return true;
    }

    // Fallback to timestamp-based seeking (when indexing not available)
    AVStream* avStream = formatCtx_->streams[videoStream_];
    double framerate = frameInfo_.framerate;
    
    if (framerate <= 0) {
        return false;
    }

    // Calculate timestamp
    int64_t timestamp = static_cast<int64_t>((frameNumber / framerate) * AV_TIME_BASE);
    
    // Seek to timestamp
    int ret = av_seek_frame(formatCtx_, videoStream_, timestamp, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        return false;
    }

    // Flush codec buffers
    if (codecCtx_->codec->flush) {
        avcodec_flush_buffers(codecCtx_);
    }

    lastDecodedPTS_ = -1;
    lastDecodedFrameNo_ = -1;
    currentFrame_ = frameNumber;
    return true;
}

bool HAPVideoInput::readFrame(int64_t frameNumber, FrameBuffer& buffer) {
    // HAP frames should be read to GPU texture, not CPU FrameBuffer
    // This method is kept for interface compatibility but should not be used for HAP
    // Use readFrameToTexture() instead
    return false;
}

bool HAPVideoInput::readFrameToTexture(int64_t frameNumber, GPUTextureFrameBuffer& textureBuffer) {
    if (!isReady()) {
        return false;
    }

    // Seek to frame if needed
    if (!seek(frameNumber)) {
        return false;
    }

    // Decode frame
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
                --bailout;
                av_free_packet(&packet);
                continue;
            } else {
                av_free_packet(&packet);
                return false;
            }
        }

        if (packet.stream_index != videoStream_) {
            av_free_packet(&packet);
            continue;
        }

        // Decode video frame
        int gotFrame = 0;
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(52, 21, 0)
        err = avcodec_decode_video(codecCtx_, frame_, &gotFrame, packet.data, packet.size);
#else
        err = avcodec_decode_video2(codecCtx_, frame_, &gotFrame, &packet);
#endif
        av_free_packet(&packet);

        if (err < 0) {
            --bailout;
            continue;
        }

        if (gotFrame) {
            frameFinished = true;
        } else {
            --bailout;
        }
    }

    if (!frameFinished) {
        return false;
    }

    // HAP frames decode to DXT1/DXT5 compressed texture data
    // The compressed data is in frame_->data[0]
    // Refine variant detection from the actual decoded frame (if not already determined)
    if (hapVariant_ == HAPVariant::HAP) {
        // If we defaulted to HAP, try to refine from the frame
        refineHAPVariantFromFrame(frame_);
    }
    
    // Determine texture format based on HAP variant
    GLenum glFormat;
    if (hapVariant_ == HAPVariant::HAP_ALPHA || hapVariant_ == HAPVariant::HAP_Q) {
        glFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        textureFormat_ = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    } else {
        glFormat = GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
        textureFormat_ = GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
    }

    // Allocate texture buffer if needed
    if (!textureBuffer.isValid() || 
        textureBuffer.info().width != frameInfo_.width || 
        textureBuffer.info().height != frameInfo_.height) {
        if (!textureBuffer.allocate(frameInfo_, glFormat, true)) {
            return false;
        }
    }

    // Calculate compressed texture size
    // DXT1/DXT5 are block-based: 4x4 pixel blocks
    // DXT1: 8 bytes per 4x4 block = 0.5 bytes per pixel
    // DXT5: 16 bytes per 4x4 block = 1 byte per pixel
    // Width and height must be rounded up to multiples of 4 for block alignment
    int blockWidth = (frameInfo_.width + 3) / 4;   // Round up to multiple of 4
    int blockHeight = (frameInfo_.height + 3) / 4; // Round up to multiple of 4
    size_t compressedSize;
    if (glFormat == GL_COMPRESSED_RGBA_S3TC_DXT5_EXT) {
        compressedSize = blockWidth * blockHeight * 16; // 16 bytes per DXT5 block
    } else {
        compressedSize = blockWidth * blockHeight * 8;  // 8 bytes per DXT1 block
    }

    // Try to use actual size from frame if available (more accurate)
    // frame->linesize[0] may contain the actual compressed chunk size
    size_t actualSize = compressedSize;
    if (frame_->linesize[0] > 0 && frame_->linesize[0] <= compressedSize * 2) {
        // Use actual size if it's reasonable (within 2x of expected)
        // This handles cases where the actual size might be slightly different
        actualSize = frame_->linesize[0];
    }

    // Validate that we have enough data
    if (!frame_->data[0]) {
        return false;
    }

    // Upload compressed texture data directly to GPU
    // HAP frames are already in compressed format, so we can upload directly
    if (!textureBuffer.uploadCompressedData(frame_->data[0], actualSize, 
                                            frameInfo_.width, frameInfo_.height, glFormat)) {
        return false;
    }

    int64_t pts = parsePTSFromFrame(frame_);
    if (pts != AV_NOPTS_VALUE) {
        lastDecodedPTS_ = pts;
        lastDecodedFrameNo_ = frameNumber;
    }

    currentFrame_ = frameNumber;
    return true;
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
    // For HAP, the compressed data is in frame->data[0]
    // The size can be determined from frame->linesize[0] or by examining the data
    size_t actualSize = 0;
    if (frame->linesize[0] > 0) {
        // linesize[0] might contain the size of the compressed chunk
        actualSize = frame->linesize[0];
    } else if (frame->data[0]) {
        // Fallback: estimate from data (less reliable)
        // For HAP, we can't easily determine size without parsing the chunk structure
        // So we'll use a heuristic based on the expected sizes
        return; // Can't reliably determine without more information
    }
    
    // Compare actual size to expected sizes
    // Allow some tolerance for compression variations
    if (actualSize > 0) {
        size_t dxt1Diff = (actualSize > expectedDXT1Size) ? 
                          (actualSize - expectedDXT1Size) : 
                          (expectedDXT1Size - actualSize);
        size_t dxt5Diff = (actualSize > expectedDXT5Size) ? 
                          (actualSize - expectedDXT5Size) : 
                          (expectedDXT5Size - actualSize);
        
        // If size is closer to DXT5, it's likely HAP_Q or HAP_ALPHA
        // We can't distinguish between HAP_Q and HAP_ALPHA from size alone
        // Default to HAP_Q (higher quality) if it looks like DXT5
        if (dxt5Diff < dxt1Diff && actualSize >= expectedDXT5Size * 0.9) {
            // Looks like DXT5 - could be HAP_Q or HAP_ALPHA
            // Default to HAP_Q (we can't distinguish without alpha channel info)
            hapVariant_ = HAPVariant::HAP_Q;
        }
        // Otherwise, keep as HAP (DXT1)
    }
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

unsigned int HAPVideoInput::getTextureFormat() const {
    return textureFormat_;
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
    textureFormat_ = 0;
}

} // namespace videocomposer

