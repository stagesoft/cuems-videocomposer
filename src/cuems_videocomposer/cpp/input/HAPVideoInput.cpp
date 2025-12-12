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
#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM
#define GL_COMPRESSED_RGBA_BPTC_UNORM 0x8E8C
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
    , frameCount_(0)
    , lastDecodedPTS_(-1)
    , lastDecodedFrameNo_(-1)
    , scanComplete_(false)
    , currentFrame_(-1)
    , hapVariant_(HAPVariant::HAP)
    , ready_(false)
#ifdef ENABLE_HAP_DIRECT
    , fallbackWarningShown_(false)
#endif
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

    // Open video file using MediaFileReader
    if (!mediaReader_.open(source)) {
        return false;
    }

    // Get format context for compatibility
    formatCtx_ = mediaReader_.getFormatContext();
    if (!formatCtx_) {
        return false;
    }

    // Find video stream using MediaFileReader
    videoStream_ = mediaReader_.findStream(AVMEDIA_TYPE_VIDEO);
    if (videoStream_ < 0) {
        mediaReader_.close();
        formatCtx_ = nullptr;
        return false;
    }

    // Verify this is a HAP codec
    // Note: FFmpeg may use AV_CODEC_ID_HAP for all HAP variants
    // The variant (HAP, HAP_Q, HAP_ALPHA) is determined later by examining the codec
    AVCodecParameters* codecParams = mediaReader_.getCodecParameters(videoStream_);
    if (!codecParams) {
        mediaReader_.close();
        formatCtx_ = nullptr;
        return false;
    }
    AVCodecID codecId = codecParams->codec_id;
    
    // Check for HAP codec (all variants use AV_CODEC_ID_HAP in some FFmpeg versions)
    // We'll detect the specific variant after opening the codec
    if (codecId != AV_CODEC_ID_HAP) {
        // Some FFmpeg versions might have separate IDs - check if available
        #ifdef AV_CODEC_ID_HAPQ
        if (codecId != AV_CODEC_ID_HAPQ && codecId != AV_CODEC_ID_HAPALPHA) {
            mediaReader_.close();
            formatCtx_ = nullptr;
            return false; // Not a HAP file
        }
        #else
        mediaReader_.close();
        formatCtx_ = nullptr;
        return false; // Not a HAP file
        #endif
    }

    // Open codec
    if (!openCodec()) {
        mediaReader_.close();
        formatCtx_ = nullptr;
        return false;
    }

    // Detect HAP variant
    hapVariant_ = detectHAPVariant();

    // Get video properties
    AVStream* avStream = mediaReader_.getStream(videoStream_);
    if (!avStream) {
        mediaReader_.close();
        formatCtx_ = nullptr;
        return false;
    }
    
    // Frame rate
    double framerate = 0.0;
    if (avStream->r_frame_rate.den > 0 && avStream->r_frame_rate.num > 0) {
        framerate = av_q2d(avStream->r_frame_rate);
        frameRateQ_.den = avStream->r_frame_rate.num;
        frameRateQ_.num = avStream->r_frame_rate.den;
    }

    // Dimensions - get from codec context
    int width = 0;
    int height = 0;
    if (codecCtx_) {
        width = codecCtx_->width;
        height = codecCtx_->height;
    } else {
        // Fallback to codec parameters if codec not opened yet
        if (codecParams) {
            width = codecParams->width;
            height = codecParams->height;
        }
    }
    
    // Duration
    double duration = mediaReader_.getDuration();

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

    // HAP is an intra-frame codec where every frame is a keyframe
    // No indexing needed - we can seek directly to any frame using timestamp calculation
    // This makes file opening instant instead of scanning the entire file
    frameCount_ = frameInfo_.totalFrames;
    scanComplete_ = true;
    LOG_INFO << "HAP: Ready for playback (" << frameCount_ << " frames, no indexing needed - all keyframes)";

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
    return ready_ && mediaReader_.isReady();
}

bool HAPVideoInput::openCodec() {
    // Get codec parameters using MediaFileReader
    AVCodecParameters* codecParams = mediaReader_.getCodecParameters(videoStream_);
    if (!codecParams) {
        return false;
    }

    // Open codec using VideoDecoder
    if (!videoDecoder_.openCodec(codecParams)) {
        return false;
    }

    // Get codec context from VideoDecoder
    codecCtx_ = videoDecoder_.getCodecContext();
    if (!codecCtx_) {
        return false;
    }

    // Allocate frame
    frame_ = av_frame_alloc();
    if (!frame_) {
        videoDecoder_.close();
        codecCtx_ = nullptr;
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

// HAP is an intra-frame codec - indexFrames() is not needed
// Every frame is a keyframe, so we can seek directly using timestamp calculation
// This is handled in seekToFrame() using calculatePtsForFrame()

bool HAPVideoInput::seek(int64_t frameNumber) {
    if (!isReady()) {
        return false;
    }

    currentFrame_ = frameNumber;
    return seekToFrame(frameNumber);
}

void HAPVideoInput::resetSeekState() {
    // Reset internal tracking to force next seek to actually perform the seek
    // even if seeking to the same frame number (used for MTC full frame SYSEX)
    lastDecodedPTS_ = -1;
    lastDecodedFrameNo_ = -1;
}

bool HAPVideoInput::seekToFrame(int64_t frameNumber) {
    if (!formatCtx_ || videoStream_ < 0) {
        return false;
    }

    // HAP is an intra-frame codec - every frame is a keyframe
    // We can seek directly to any frame using timestamp calculation
    // No index needed - this is instant
    
    double framerate = frameInfo_.framerate;
    if (framerate <= 0) {
        return false;
    }

    // Check if we need to seek (skip for sequential frames - just advance)
    bool needSeek = false;
    if (lastDecodedFrameNo_ < 0) {
        needSeek = true;  // First seek
    } else if (frameNumber < lastDecodedFrameNo_) {
        needSeek = true;  // Backward seek
    } else if (frameNumber != lastDecodedFrameNo_ + 1) {
        needSeek = true;  // Non-sequential forward seek
    }
    // For sequential frames (frameNumber == lastDecodedFrameNo_ + 1), no seek needed

    if (needSeek) {
        // Calculate timestamp from frame number
        double targetTime = static_cast<double>(frameNumber) / framerate;
        
        bool ret = mediaReader_.seekToTime(targetTime, videoStream_, AVSEEK_FLAG_BACKWARD);
        if (!ret) {
            return false;
        }

        // Flush codec buffers
        if (codecCtx_) {
            avcodec_flush_buffers(codecCtx_);
        }
        
        lastDecodedPTS_ = -1;
        lastDecodedFrameNo_ = -1;
    }

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

    // QUICK WIN: Early return for same frame (xjadeo-style)
    // If same frame is requested and we have valid decoded data, just copy from frame_
    if (currentFrame_ == frameNumber && frame_ && frame_->data[0]) {
        // Same frame requested - just copy existing data
        if (!buffer.isValid() || buffer.info().width != frameInfo_.width ||
            buffer.info().height != frameInfo_.height) {
            FrameInfo outputInfo = frameInfo_;
            outputInfo.format = PixelFormat::RGBA32;
            buffer.allocate(outputInfo);
        }
        
        // Copy from decoded frame (HAP decodes to RGBA, linesize may have padding)
        int srcLinesize = frame_->linesize[0];
        int dstLinesize = frameInfo_.width * 4;
        if (srcLinesize == dstLinesize) {
            memcpy(buffer.data(), frame_->data[0], dstLinesize * frameInfo_.height);
        } else {
            for (int y = 0; y < frameInfo_.height; y++) {
                memcpy(buffer.data() + y * dstLinesize,
                       frame_->data[0] + y * srcLinesize,
                       dstLinesize);
            }
        }
        return true;
    }

    // Seek to target frame
    if (currentFrame_ < 0 || currentFrame_ != frameNumber) {
        if (!seek(frameNumber)) {
            return false;
        }
    }

    // Decode frame using VideoDecoder API
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return false;
    }

    int bailout = 20;
    bool frameFinished = false;

    while (bailout > 0 && !frameFinished) {
        av_packet_unref(packet);
        int err = mediaReader_.readPacket(packet);
        if (err < 0) {
            if (err == AVERROR_EOF) {
                // Flush decoder
                err = videoDecoder_.sendPacket(nullptr); // nullptr flushes
                if (err < 0 && err != AVERROR_EOF) {
                    --bailout;
                    continue;
                }
            } else {
                av_packet_free(&packet);
                return false;
            }
        }

        if (packet->stream_index != videoStream_) {
            continue;
        }

        // Send packet to decoder using VideoDecoder
        err = videoDecoder_.sendPacket(packet);
        if (err < 0) {
            if (err != AVERROR(EAGAIN)) {
                --bailout;
                continue;
            }
        }

        // Receive frame from decoder using VideoDecoder
        err = videoDecoder_.receiveFrame(frame_);
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

    av_packet_free(&packet);

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

    int64_t pts = parsePTSFromFrame(frame_);
    if (pts != AV_NOPTS_VALUE) {
        lastDecodedPTS_ = pts;
        lastDecodedFrameNo_ = frameNumber;
    }

    currentFrame_ = frameNumber;
    return true;
}

bool HAPVideoInput::readFrameToTexture(int64_t frameNumber, GPUTextureFrameBuffer& textureBuffer) {
    if (!isReady()) {
        return false;
    }

    // QUICK WIN: Early return for same frame (xjadeo-style)
    // If same frame is requested and texture is valid, nothing to do
    if (currentFrame_ == frameNumber && textureBuffer.isValid()) {
        return true;
    }

#ifdef ENABLE_HAP_DIRECT
    // Try direct HAP decode first (optimal path)
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return false;
    }

    if (readRawPacket(frameNumber, packet)) {
        bool success = decodeHapDirectToTexture(packet, textureBuffer);
        av_packet_free(&packet);
        
        if (success) {
            currentFrame_ = frameNumber;
            return true;
        }
        
        // Log fallback warning (once per file)
        if (!fallbackWarningShown_) {
            LOG_WARNING << "HAP direct decode failed for frame " << frameNumber 
                       << ", falling back to FFmpeg RGBA path (reduced performance)";
            LOG_WARNING << "Error: " << hapDecoder_.getLastError();
            fallbackWarningShown_ = true;
        }
    } else {
        av_packet_free(&packet);
    }
#endif

    // Fallback: FFmpeg decode to RGBA, upload as uncompressed
    return decodeWithFFmpegFallback(frameNumber, textureBuffer);
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
    
    // Calculate expected sizes for both variants (commented out - not currently used)
    // int blockWidth = (frameInfo_.width + 3) / 4;
    // int blockHeight = (frameInfo_.height + 3) / 4;
    // size_t expectedDXT1Size = blockWidth * blockHeight * 8;   // DXT1
    // size_t expectedDXT5Size = blockWidth * blockHeight * 16;  // DXT5
    
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
    }
    // pkt_pts was removed in FFmpeg 4.0 (libavutil 56.x+)
    // Use best_effort_timestamp or pts instead (compatible with all versions)
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 0, 0)
    // FFmpeg 4.0+: use best_effort_timestamp field directly
    if (pts == AV_NOPTS_VALUE && frame->best_effort_timestamp != AV_NOPTS_VALUE) {
        pts = frame->best_effort_timestamp;
    }
#endif
    // Note: pkt_pts removed, using pts above should be sufficient
    if (pts == AV_NOPTS_VALUE && frame->pkt_dts != AV_NOPTS_VALUE) {
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


#ifdef ENABLE_HAP_DIRECT
bool HAPVideoInput::readRawPacket(int64_t frameNumber, AVPacket* packet) {
    // Seek to frame if needed
    if (currentFrame_ < 0 || currentFrame_ != frameNumber) {
        if (!seek(frameNumber)) {
            LOG_WARNING << "Failed to seek to frame " << frameNumber;
            return false;
        }
    }

    // Read packets until we find the video packet for this frame
    int bailout = 20;
    while (bailout > 0) {
        av_packet_unref(packet);
        int err = mediaReader_.readPacket(packet);
        if (err < 0) {
            if (err == AVERROR_EOF) {
                --bailout;
                continue;
            } else {
                return false;
            }
        }

        if (packet->stream_index != videoStream_) {
            continue;
        }

        // Found video packet
        return true;
    }

    return false;
}

bool HAPVideoInput::decodeHapDirectToTexture(AVPacket* packet, GPUTextureFrameBuffer& textureBuffer) {
    if (!packet || packet->size == 0) {
        return false;
    }

    // Get HAP variant from packet
    HapVariant variant = hapDecoder_.getVariant(packet->data, packet->size);
    if (variant == HapVariant::NONE) {
        LOG_WARNING << "Unknown HAP variant in packet";
        return false;
    }

    // Decode HAP packet to DXT textures
    std::vector<HapDecodedTexture> textures;
    if (!hapDecoder_.decode(packet->data, packet->size, 
                            frameInfo_.width, frameInfo_.height, textures)) {
        return false;
    }

    if (textures.empty()) {
        LOG_WARNING << "No textures decoded from HAP packet";
        return false;
    }

    // Upload textures based on variant
    if (variant == HapVariant::HAP_Q_ALPHA) {
        // Dual texture: YCoCg color + alpha
        if (textures.size() != 2) {
            LOG_WARNING << "HAP Q Alpha should have 2 textures, got " << textures.size();
            return false;
        }

        // Allocate dual texture if needed
        if (!textureBuffer.isValid() || textureBuffer.getPlaneType() != TexturePlaneType::HAP_Q_ALPHA) {
            if (!textureBuffer.allocateHapQAlpha(frameInfo_)) {
                LOG_WARNING << "Failed to allocate HAP Q Alpha texture";
                return false;
            }
        }

        // Upload both textures
        if (!textureBuffer.uploadHapQAlphaData(
                textures[0].data.data(), textures[0].size,
                textures[1].data.data(), textures[1].size,
                frameInfo_.width, frameInfo_.height)) {
            return false;
        }

        textureBuffer.setHapVariant(HapVariant::HAP_Q_ALPHA);
    } else {
        // Single texture: HAP, HAP Q, or HAP Alpha
        if (textures.size() != 1) {
            LOG_WARNING << "Single-texture HAP should have 1 texture, got " << textures.size();
            return false;
        }

        // Determine OpenGL format from HAP format
        GLenum glFormat;
        HapVariant gpuVariant;
        switch (textures[0].format) {
            case HapTextureFormat_RGB_DXT1:
                glFormat = GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
                gpuVariant = HapVariant::HAP;
                break;
            case HapTextureFormat_RGBA_DXT5:
                glFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
                gpuVariant = HapVariant::HAP_ALPHA;
                break;
            case HapTextureFormat_YCoCg_DXT5:
                glFormat = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
                gpuVariant = HapVariant::HAP_Q;
                break;
            case HapTextureFormat_RGBA_BPTC_UNORM:
                glFormat = GL_COMPRESSED_RGBA_BPTC_UNORM;
                gpuVariant = HapVariant::HAP_R;  // HAP R (BPTC/BC7 - best quality + alpha) - UNTESTED
                break;
            default:
                LOG_WARNING << "Unsupported HAP texture format: 0x" << std::hex << textures[0].format;
                return false;
        }

        // Allocate texture if needed
        if (!textureBuffer.isValid() || textureBuffer.getTextureFormat() != glFormat) {
            if (!textureBuffer.allocate(frameInfo_, glFormat, true)) {
                LOG_WARNING << "Failed to allocate HAP texture";
                return false;
            }
        }

        // Upload compressed DXT data
        if (!textureBuffer.uploadCompressedData(
                textures[0].data.data(), textures[0].size,
                frameInfo_.width, frameInfo_.height, glFormat)) {
            return false;
        }

        textureBuffer.setHapVariant(gpuVariant);
    }

    return true;
}
#endif

bool HAPVideoInput::decodeWithFFmpegFallback(int64_t frameNumber, GPUTextureFrameBuffer& textureBuffer) {
    // Use standard readFrame to decode to CPU buffer
    FrameBuffer cpuBuffer;
    if (!readFrame(frameNumber, cpuBuffer)) {
        return false;
    }

    // Allocate GPU texture if needed
    if (!textureBuffer.isValid() || textureBuffer.getTextureFormat() != GL_RGBA) {
        if (!textureBuffer.allocate(frameInfo_, GL_RGBA, false)) {
            LOG_WARNING << "Failed to allocate fallback texture";
            return false;
        }
    }

    // Upload CPU buffer to GPU as uncompressed RGBA
    if (!textureBuffer.uploadUncompressedData(
            cpuBuffer.data(), cpuBuffer.size(),
            frameInfo_.width, frameInfo_.height, GL_RGBA)) {
        return false;
    }

    textureBuffer.setHapVariant(HapVariant::NONE);  // Not a compressed HAP texture
    return true;
}

void HAPVideoInput::cleanup() {
    frameCount_ = 0;

    // Free frame (must use av_frame_free for frames allocated with av_frame_alloc)
    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }

    // Close video decoder
    videoDecoder_.close();
        codecCtx_ = nullptr;

    // Close media reader (closes format context)
    mediaReader_.close();
        formatCtx_ = nullptr;

    videoStream_ = -1;
    lastDecodedPTS_ = -1;
    lastDecodedFrameNo_ = -1;
    scanComplete_ = false;
    currentFrame_ = -1;
    
#ifdef ENABLE_HAP_DIRECT
    fallbackWarningShown_ = false;
#endif
}

} // namespace videocomposer

