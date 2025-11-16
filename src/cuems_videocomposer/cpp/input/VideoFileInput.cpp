#include "VideoFileInput.h"
#include "../../ffcompat.h"
#include "../utils/CLegacyBridge.h"
#include "../utils/Logger.h"
#include <cstring>
#include <cassert>
#include <algorithm>
#include <vector>

// OpenGL includes
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
}

namespace videocomposer {

VideoFileInput::VideoFileInput()
    : formatCtx_(nullptr)
    , codecCtx_(nullptr)
    , frame_(nullptr)
    , frameFMT_(nullptr)
    , swsCtx_(nullptr)
    , swsCtxWidth_(0)
    , swsCtxHeight_(0)
    , videoStream_(-1)
    , frameIndex_(nullptr)
    , frameCount_(0)
    , lastDecodedPTS_(-1)
    , lastDecodedFrameNo_(-1)
    , scanComplete_(false)
    , byteSeek_(false)
    , ignoreStartOffset_(false)
    , noIndex_(false)
    , currentFrame_(-1)
    , ready_(false)
    , hwDeviceCtx_(nullptr)
    , hwFrame_(nullptr)
    , hwDecoderType_(HardwareDecoder::Type::NONE)
    , useHardwareDecoding_(false)
    , codecCtxAllocated_(false)
{
    frameRateQ_ = {1, 1};
    frameInfo_ = {};
}

VideoFileInput::~VideoFileInput() {
    close();
}

bool VideoFileInput::initializeFFmpeg() {
    // FFmpeg should already be initialized globally
    // This is just a placeholder for any per-instance initialization
    return true;
}

bool VideoFileInput::open(const std::string& source) {
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

    // Open codec (try hardware first, fallback to software)
    if (!openHardwareCodec() && !openCodec()) {
        mediaReader_.close();
        formatCtx_ = nullptr;
        return false;
    }

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
        AVCodecParameters* codecParams = mediaReader_.getCodecParameters(videoStream_);
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
    // Use BGRA32 format for OpenGL rendering (matches original xjadeo)
    frameInfo_.format = PixelFormat::BGRA32;

    // Calculate total frames (needed before indexing decision)
    if (framerate > 0 && duration > 0) {
        frameInfo_.totalFrames = (int64_t)(duration * framerate);
    } else {
        // Will be set after indexing if indexing is enabled
        frameInfo_.totalFrames = 0;
    }

    // Index frames (unless --noindex flag is set)
    if (!noIndex_) {
    if (!indexFrames()) {
        cleanup();
        return false;
    }

        // If indexing completed, use indexed frame count if available
        if (frameCount_ > 0 && frameInfo_.totalFrames == 0) {
            frameInfo_.totalFrames = frameCount_;
        }
    } else {
        // Without indexing, we rely on duration-based frame calculation
        // and timestamp-based seeking
        scanComplete_ = true; // Mark as "complete" so seeking can work
        frameCount_ = frameInfo_.totalFrames; // Use calculated frame count
    }

    // Update C globals for compatibility (used by display backends and SMPTEWrapper)
    movie_width = width;
    movie_height = height;
    movie_aspect = frameInfo_.aspect;
    ::framerate = framerate;
    frames = frameInfo_.totalFrames;

    ready_ = true;
    currentFrame_ = -1;
    return true;
}

void VideoFileInput::close() {
    cleanup();
    currentFile_.clear();
    ready_ = false;
    currentFrame_ = -1;
    frameInfo_ = {};
    
    // Reset C globals
    movie_width = 640;
    movie_height = 360;
    movie_aspect = 640.0f / 360.0f;
    ::framerate = 1.0;
    frames = 1;
}

bool VideoFileInput::isReady() const {
    return ready_ && mediaReader_.isReady();
}

bool VideoFileInput::openHardwareCodec() {
    // Try to open hardware decoder if available
    // First, we need to detect the codec type
    AVCodecParameters* codecParams = mediaReader_.getCodecParameters(videoStream_);
    if (!codecParams) {
        return false;
    }
    
    AVCodecID codecId = codecParams->codec_id;
    std::string codecName = avcodec_get_name(codecId);
    
    // Hardware decoding only for H.264, HEVC, AV1
    if (codecId != AV_CODEC_ID_H264 && codecId != AV_CODEC_ID_HEVC && codecId != AV_CODEC_ID_AV1) {
        LOG_VERBOSE << "Codec " << codecName << " does not support hardware decoding, will use software";
        return false;
    }

    LOG_INFO << "Attempting to open hardware decoder for codec: " << codecName;

    // Detect available hardware decoder
    hwDecoderType_ = HardwareDecoder::detectAvailable();
    if (hwDecoderType_ == HardwareDecoder::Type::NONE) {
        LOG_INFO << "No hardware decoder available for " << codecName << ", falling back to software decoding";
        return false;
    }

    // Check if hardware decoder is available for this codec
    if (!HardwareDecoder::isAvailableForCodec(codecId)) {
        LOG_INFO << "Hardware decoder not available for " << codecName << ", falling back to software decoding";
        return false;
    }

    // Get hardware device type
    AVHWDeviceType hwDeviceType = HardwareDecoder::getFFmpegDeviceType(hwDecoderType_);
    if (hwDeviceType == AV_HWDEVICE_TYPE_NONE) {
        return false;
    }

    // Create hardware device context
    int ret = av_hwdevice_ctx_create(&hwDeviceCtx_, hwDeviceType, nullptr, nullptr, 0);
    if (ret < 0) {
        hwDeviceCtx_ = nullptr;
        return false;
    }

    // Find hardware decoder
    std::string hwCodecName;
    switch (hwDecoderType_) {
        case HardwareDecoder::Type::VAAPI:
            hwCodecName = codecName + "_vaapi";
            break;
        case HardwareDecoder::Type::CUDA:
            hwCodecName = codecName + "_cuda";
            break;
        case HardwareDecoder::Type::VIDEOTOOLBOX:
            hwCodecName = codecName; // VideoToolbox uses same name
            break;
        case HardwareDecoder::Type::DXVA2:
            hwCodecName = codecName + "_dxva2";
            break;
        default:
            return false;
    }

    AVCodec* hwCodec = avcodec_find_decoder_by_name(hwCodecName.c_str());
    if (!hwCodec) {
        LOG_WARNING << "Hardware decoder " << hwCodecName << " not found, falling back to software";
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
        return false;
    }
    
    LOG_INFO << "Found hardware decoder: " << hwCodecName;

    // Allocate codec context for hardware decoder
    codecCtx_ = avcodec_alloc_context3(hwCodec);
    if (!codecCtx_) {
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
        return false;
    }
    codecCtxAllocated_ = true;  // Mark as allocated (must be freed)

    // Copy codec parameters from stream
    if (avcodec_parameters_to_context(codecCtx_, codecParams) < 0) {
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
        return false;
    }

    // Set hardware device context
    codecCtx_->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
    if (!codecCtx_->hw_device_ctx) {
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
        return false;
    }

    // Get hardware pixel format
    AVPixelFormat hwPixFmt = HardwareDecoder::getHardwarePixelFormat(hwDecoderType_, codecId);
    if (hwPixFmt == AV_PIX_FMT_NONE) {
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
        return false;
    }

    // Set hardware pixel format callback
    codecCtx_->get_format = [](AVCodecContext* ctx, const AVPixelFormat* pix_fmts) -> AVPixelFormat {
        const AVPixelFormat* p;
        AVPixelFormat hwPixFmt = *reinterpret_cast<AVPixelFormat*>(ctx->opaque);
        for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
            if (*p == hwPixFmt) {
                return *p;
            }
        }
        return AV_PIX_FMT_NONE;
    };
    codecCtx_->opaque = reinterpret_cast<void*>(static_cast<intptr_t>(hwPixFmt));

    // Open hardware codec
    if (avcodec_open2(codecCtx_, hwCodec, nullptr) < 0) {
        LOG_WARNING << "Failed to open hardware decoder " << hwCodecName << ", falling back to software";
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
        return false;
    }
    
    LOG_INFO << "Successfully opened hardware decoder: " << hwCodecName 
             << " (" << HardwareDecoder::getName(hwDecoderType_) << ")";

    // Allocate frames
    frame_ = av_frame_alloc();
    if (!frame_) {
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
        return false;
    }

    hwFrame_ = av_frame_alloc();
    if (!hwFrame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
        return false;
    }

    frameFMT_ = av_frame_alloc();
    if (!frameFMT_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
        av_frame_free(&hwFrame_);
        hwFrame_ = nullptr;
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
        return false;
    }

    useHardwareDecoding_ = true;
    LOG_INFO << "Using HARDWARE decoding for " << codecName;
    return true;
}

bool VideoFileInput::openCodec() {
    // Get codec parameters using MediaFileReader
    AVCodecParameters* codecParams = mediaReader_.getCodecParameters(videoStream_);
    if (!codecParams) {
        LOG_ERROR << "Failed to get codec parameters for video stream";
        return false;
    }

    std::string codecName = avcodec_get_name(codecParams->codec_id);
    LOG_INFO << "Opening software decoder for codec: " << codecName;

    // Open codec using VideoDecoder
    if (!videoDecoder_.openCodec(codecParams)) {
        LOG_ERROR << "Failed to open software decoder for codec: " << codecName;
        return false;
    }

    // Get codec context from VideoDecoder
    codecCtx_ = videoDecoder_.getCodecContext();
    codecCtxAllocated_ = false;  // Managed by VideoDecoder
    
    LOG_INFO << "Successfully opened software decoder: " << codecName;

    // Allocate frames
    frame_ = av_frame_alloc();
    if (!frame_) {
        videoDecoder_.close();
        codecCtx_ = nullptr;
        return false;
    }

    frameFMT_ = av_frame_alloc();
    if (!frameFMT_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
        videoDecoder_.close();
        codecCtx_ = nullptr;
        return false;
    }

    useHardwareDecoding_ = false;
    LOG_INFO << "Using SOFTWARE decoding for " << codecName;
    return true;
}

bool VideoFileInput::indexFrames() {
    // Simplified indexing - in full implementation, this would index all frames
    // For now, we'll do a basic implementation

    frameCount_ = 0;
    frameIndex_ = nullptr;

    // Allocate initial index array (will grow as needed)
    const size_t initialSize = 10000;
    frameIndex_ = static_cast<FrameIndex*>(calloc(initialSize, sizeof(FrameIndex)));
    if (!frameIndex_) {
        return false;
    }

    size_t indexSize = initialSize;
    AVStream* avStream = mediaReader_.getStream(videoStream_);
    if (!avStream) {
        return false;
    }
    AVRational timeBase = avStream->time_base;

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return false;
    }
    while (mediaReader_.readPacket(packet) == 0) {
        if (packet->stream_index != videoStream_) {
            av_packet_unref(packet);
            continue;
        }

        int64_t ts = AV_NOPTS_VALUE;
        if (packet->pts != AV_NOPTS_VALUE) {
            ts = packet->pts;
        } else if (packet->dts != AV_NOPTS_VALUE) {
            ts = packet->dts;
        }

        if (ts == AV_NOPTS_VALUE) {
            av_packet_unref(packet);
            continue;
        }

        // Grow array if needed
        if (frameCount_ >= static_cast<int64_t>(indexSize)) {
            indexSize *= 2;
            frameIndex_ = static_cast<FrameIndex*>(realloc(frameIndex_, indexSize * sizeof(FrameIndex)));
            if (!frameIndex_) {
                av_packet_unref(packet);
                av_packet_free(&packet);
                return false;
            }
        }

        FrameIndex& idx = frameIndex_[frameCount_];
        idx.pkt_pts = packet->pts;
        idx.pkt_pos = packet->pos;
        idx.frame_pts = ts;
        idx.frame_pos = packet->pos;
        // Calculate timestamp in stream's timebase (like xjadeo)
        // timestamp = av_rescale_q(frameCount_, frameRateQ_, timeBase)
        idx.timestamp = av_rescale_q(frameCount_, frameRateQ_, timeBase);
        idx.seekpts = ts;
        idx.seekpos = packet->pos;
        idx.key = (packet->flags & AV_PKT_FLAG_KEY) ? 1 : 0;

        frameCount_++;
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    scanComplete_ = true;
    return true;
}

bool VideoFileInput::seek(int64_t frameNumber) {
    if (!isReady() || !scanComplete_) {
        return false;
    }

    int64_t targetFrame = frameNumber;
    if (ignoreStartOffset_) {
        targetFrame += frameInfo_.fileFrameOffset;
    }

    if (targetFrame < 0) {
        return false;
    }

    // If no indexing, use timestamp-based seeking
    if (noIndex_ || !frameIndex_) {
        // Check bounds using totalFrames instead of frameCount_
        if (frameInfo_.totalFrames > 0 && targetFrame >= frameInfo_.totalFrames) {
            return false;
        }
        return seekByTimestamp(targetFrame);
    }

    // With indexing, check bounds and use indexed seeking
    if (targetFrame >= frameCount_) {
        return false;
    }

    return seekToFrame(targetFrame);
}

bool VideoFileInput::seekToFrame(int64_t frameNumber) {
    if (frameNumber < 0 || frameNumber >= frameCount_ || !frameIndex_) {
        return false;
    }

    const FrameIndex& idx = frameIndex_[frameNumber];
    int64_t timestamp = idx.timestamp;

    if (timestamp < 0) {
        return false;
    }

    // Check if we need to seek
    bool needSeek = false;
    if (lastDecodedPTS_ < 0 || lastDecodedFrameNo_ < 0) {
        needSeek = true;
    } else if (lastDecodedPTS_ > timestamp) {
        needSeek = true;
    } else if ((frameNumber - lastDecodedFrameNo_) != 1) {
        if (idx.seekpts != frameIndex_[lastDecodedFrameNo_].seekpts) {
            needSeek = true;
        }
    }

    lastDecodedPTS_ = -1;
    lastDecodedFrameNo_ = -1;

    if (needSeek) {
        bool seekResult;
        if (byteSeek_ && idx.seekpos > 0) {
            seekResult = mediaReader_.seek(idx.seekpos, videoStream_, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_BYTE);
        } else {
            seekResult = mediaReader_.seek(idx.seekpts, videoStream_, AVSEEK_FLAG_BACKWARD);
        }

        if (codecCtx_ && codecCtx_->codec && codecCtx_->codec->flush) {
            avcodec_flush_buffers(codecCtx_);
        }

        if (!seekResult) {
            return false;
        }
    }

    currentFrame_ = frameNumber;
    return true;
}

bool VideoFileInput::seekByTimestamp(int64_t frameNumber) {
    // Seek using timestamp calculation (for files without indexing)
    if (frameInfo_.framerate <= 0.0 || !mediaReader_.isReady() || videoStream_ < 0) {
        return false;
    }

    // Calculate target timestamp from frame number
    double targetTime = (double)frameNumber / frameInfo_.framerate;

    // Seek to the target timestamp using MediaFileReader
    bool seekResult = mediaReader_.seekToTime(targetTime, videoStream_, AVSEEK_FLAG_BACKWARD);
    if (!seekResult) {
        return false;
    }

    // Flush codec buffers
    if (codecCtx_ && codecCtx_->codec && codecCtx_->codec->flush) {
        avcodec_flush_buffers(codecCtx_);
    }

    currentFrame_ = frameNumber;
    lastDecodedPTS_ = -1;
    lastDecodedFrameNo_ = -1;
    
    return true;
}

bool VideoFileInput::readFrame(int64_t frameNumber, FrameBuffer& buffer) {
    if (!isReady()) {
        return false;
    }

    // Check if we need to seek (optimize for sequential frame access)
    bool needSeek = false;
    if (currentFrame_ < 0 || currentFrame_ != frameNumber) {
        // Check if this is a sequential frame (next frame after current)
        if (currentFrame_ >= 0 && frameNumber == currentFrame_ + 1) {
            // Sequential frame - don't seek, just continue reading
            needSeek = false;
        } else {
            // Non-sequential frame - need to seek
            needSeek = true;
        }
    }
    
    // Seek only if needed
    if (needSeek) {
    if (!seek(frameNumber)) {
            LOG_WARNING << "Failed to seek to frame " << frameNumber;
        return false;
        }
    }

    // Decode frame
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return false;
    }

    // Get target timestamp for the frame we want (like xjadeo)
    // Use frame_pts (actual PTS from packet) instead of calculated timestamp
    // This ensures we match against the actual PTS values in the stream
    int64_t targetTimestamp = -1;
    if (frameIndex_ && frameNumber >= 0 && frameNumber < frameCount_) {
        // Use frame_pts if available, fallback to timestamp
        if (frameIndex_[frameNumber].frame_pts >= 0) {
            targetTimestamp = frameIndex_[frameNumber].frame_pts;
        } else {
            targetTimestamp = frameIndex_[frameNumber].timestamp;
        }
    } else if (!noIndex_ && frameInfo_.totalFrames > 0) {
        // Fallback: calculate timestamp from frame number
        AVStream* avStream = mediaReader_.getStream(videoStream_);
        if (!avStream) {
            return false;
        }
        AVRational timeBase = avStream->time_base;
        double fps = av_q2d(av_guess_frame_rate(formatCtx_, avStream, nullptr));
        if (fps > 0) {
            // Calculate timestamp in stream's timebase (like xjadeo)
            AVRational frameRateQ = { static_cast<int>(fps * 1000), 1000 }; // Approximate
            targetTimestamp = av_rescale_q(frameNumber, frameRateQ, timeBase);
        }
    }

    // Decode frames until we get the target frame (like xjadeo's seek_frame)
    // Use PTS matching with fuzzy matching to handle keyframe-based codecs
    int64_t oneFrame = 1;
    if (frameIndex_ && frameNumber > 0 && frameNumber < frameCount_) {
        // Calculate one_frame equivalent (timestamp difference between consecutive frames)
        if (frameIndex_[frameNumber-1].timestamp >= 0 && 
            frameIndex_[frameNumber].timestamp >= 0) {
            oneFrame = frameIndex_[frameNumber].timestamp - frameIndex_[frameNumber-1].timestamp;
            if (oneFrame <= 0) oneFrame = 1;
        }
    }
    const int64_t prefuzz = oneFrame > 10 ? 1 : 0;
    
    // Increase bailout for formats that need more decoding iterations (like xjadeo uses 2 * seek_threshold)
    // Some formats (especially keyframe-based codecs) may need to decode many frames to reach target
    int bailout = 2 * 8; // 16 (xjadeo default), but increase for problematic formats
    if (targetTimestamp >= 0 && frameIndex_) {
        // For indexed files, we can be more generous with bailout
        // Keyframe-based codecs may need to decode many frames from a keyframe to reach target
        bailout = 64; // Increased from 32 to handle keyframe-based codecs better
    }
    bool frameFinished = false;

    while (bailout > 0) {
        av_packet_unref(packet);
        int err = mediaReader_.readPacket(packet);
        if (err < 0) {
            if (err == AVERROR_EOF) {
                --bailout;
                continue;
            } else {
                av_packet_free(&packet);
                return false;
            }
        }

        if (packet->stream_index != videoStream_) {
            continue;
        }

    // Decode video frame using VideoDecoder
    // Send packet to decoder
    err = videoDecoder_.sendPacket(packet);
    if (err < 0 && err != AVERROR(EAGAIN)) {
        --bailout;
        continue;
    }

    // Receive frame from decoder
    err = videoDecoder_.receiveFrame(frame_);
    bool gotFrame = (err == 0);
    if (err == AVERROR(EAGAIN)) {
        // Need more packets
        --bailout;
        continue;
    } else if (err < 0) {
        --bailout;
        continue;
    }

    if (!gotFrame) {
        --bailout;
        continue;
    }

        // Check if we got the target frame (like xjadeo's PTS matching)
        int64_t pts = parsePTSFromFrame(frame_);
        if (pts == AV_NOPTS_VALUE) {
            --bailout;
            continue;
        }

        // Update last decoded PTS for error reporting
        lastDecodedPTS_ = pts;

        // Match xjadeo's fuzzy PTS matching logic
        if (targetTimestamp >= 0) {
            if (pts + prefuzz >= targetTimestamp) {
                // Fuzzy match: check if PTS is close enough to target (like xjadeo line 674)
                if (pts - targetTimestamp < oneFrame) {
                    // Found target frame!
            frameFinished = true;
                    break;
                }
                // PTS is beyond target but not close enough - continue decoding
                // This can happen with keyframe-based codecs where we need to decode more frames
                // xjadeo continues in this case and may eventually return an error if bailout expires
            }
            // PTS is before target - continue decoding
        } else {
            // No target timestamp - just use first decoded frame
            frameFinished = true;
            break;
        }

            --bailout;
    }

    if (!frameFinished) {
        // Log detailed error information for debugging format-specific issues
        if (targetTimestamp >= 0) {
            LOG_WARNING << "Failed to decode target frame " << frameNumber 
                       << " (target PTS: " << targetTimestamp 
                       << ", last decoded PTS: " << (lastDecodedPTS_ >= 0 ? std::to_string(lastDecodedPTS_) : "none")
                       << ", bailout expired)";
        } else {
            LOG_WARNING << "Failed to decode frame " << frameNumber 
                       << " (no target timestamp, bailout expired)";
        }
        return false;
    }

    // Allocate buffer if needed
    if (!buffer.isValid() || buffer.info().width != frameInfo_.width || 
        buffer.info().height != frameInfo_.height) {
        if (!buffer.allocate(frameInfo_)) {
            return false;
        }
    }

    // Convert frame format using sws_scale (YUV to RGB)
    // Initialize sws context if needed
    if (!swsCtx_ || swsCtxWidth_ != frameInfo_.width || swsCtxHeight_ != frameInfo_.height) {
        if (swsCtx_) {
            sws_freeContext(swsCtx_);
            swsCtx_ = nullptr;
        }
        
        // Convert from source format to BGRA32 for OpenGL (matches original xjadeo)
        swsCtx_ = sws_getContext(
            codecCtx_->width, codecCtx_->height, codecCtx_->pix_fmt,
            frameInfo_.width, frameInfo_.height, AV_PIX_FMT_BGRA,
            SWS_BICUBIC, nullptr, nullptr, nullptr
        );
        if (!swsCtx_) {
            return false;
        }
        swsCtxWidth_ = frameInfo_.width;
        swsCtxHeight_ = frameInfo_.height;
    }

    // Calculate BGRA buffer stride (BGRA32 = 4 bytes per pixel)
    int bgraStride = frameInfo_.width * 4;
    
    // Prepare destination frame (BGRA32, packed format - single plane)
    uint8_t* dstData[4] = {buffer.data(), nullptr, nullptr, nullptr};
    int dstLinesize[4] = {bgraStride, 0, 0, 0};
    
    // Scale and convert YUV to RGB
    int result = sws_scale(swsCtx_,
              (const uint8_t* const*)frame_->data, frame_->linesize,
              0, codecCtx_->height,
              dstData, dstLinesize);
    
    if (result <= 0) {
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

int64_t VideoFileInput::parsePTSFromFrame(AVFrame* frame) {
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

FrameInfo VideoFileInput::getFrameInfo() const {
    return frameInfo_;
}

int64_t VideoFileInput::getCurrentFrame() const {
    return currentFrame_;
}

InputSource::CodecType VideoFileInput::detectCodec() const {
    if (!codecCtx_) {
        return CodecType::SOFTWARE;
    }

    // Check for HAP codec
    // Note: FFmpeg may use AV_CODEC_ID_HAP for all HAP variants
    // The specific variant (HAP, HAP_Q, HAP_ALPHA) is determined by HAPVideoInput
    if (codecCtx_->codec_id == AV_CODEC_ID_HAP) {
        // Check for variant-specific codec IDs if available
        #ifdef AV_CODEC_ID_HAPALPHA
        if (codecCtx_->codec_id == AV_CODEC_ID_HAPALPHA) {
            return CodecType::HAP_ALPHA;
        }
        #endif
        #ifdef AV_CODEC_ID_HAPQ
        if (codecCtx_->codec_id == AV_CODEC_ID_HAPQ) {
            return CodecType::HAP_Q;
        }
        #endif
        // Default to standard HAP - HAPVideoInput will detect the actual variant
        return CodecType::HAP;
    }

    // Check for hardware-accelerated codecs
    if (codecCtx_->codec_id == AV_CODEC_ID_H264) {
        return CodecType::H264;
    }
    if (codecCtx_->codec_id == AV_CODEC_ID_HEVC) {
        return CodecType::HEVC;
    }
    if (codecCtx_->codec_id == AV_CODEC_ID_AV1) {
        return CodecType::AV1;
    }

    // Default to software codec
    return CodecType::SOFTWARE;
}

bool VideoFileInput::supportsDirectGPUTexture() const {
    // Only HAP codecs support direct GPU texture decoding
    return detectCodec() == CodecType::HAP ||
           detectCodec() == CodecType::HAP_Q ||
           detectCodec() == CodecType::HAP_ALPHA;
}

InputSource::DecodeBackend VideoFileInput::getOptimalBackend() const {
    CodecType codec = detectCodec();
    
    // HAP codecs use direct GPU texture (zero-copy)
    if (codec == CodecType::HAP || codec == CodecType::HAP_Q || codec == CodecType::HAP_ALPHA) {
        return DecodeBackend::HAP_DIRECT;
    }
    
    // Hardware-accelerated codecs (H.264, HEVC, AV1) can use GPU hardware decoder
    if (codec == CodecType::H264 || codec == CodecType::HEVC || codec == CodecType::AV1) {
        // Check if hardware decoder is available for this codec
        if (!codecCtx_) {
            // Codec context not yet opened, check if hardware decoder exists
            AVCodecID codecId = AV_CODEC_ID_NONE;
            if (codec == CodecType::H264) {
                codecId = AV_CODEC_ID_H264;
            } else if (codec == CodecType::HEVC) {
                codecId = AV_CODEC_ID_HEVC;
            } else if (codec == CodecType::AV1) {
                codecId = AV_CODEC_ID_AV1;
            }
            
            if (codecId != AV_CODEC_ID_NONE && HardwareDecoder::isAvailableForCodec(codecId)) {
                return DecodeBackend::GPU_HARDWARE;
            }
        } else {
            // Codec context is open, check if we're using hardware decoding
            if (useHardwareDecoding_) {
                return DecodeBackend::GPU_HARDWARE;
            }
        }
        
        // Hardware decoder not available, use software
        return DecodeBackend::CPU_SOFTWARE;
    }
    
    // Software codecs use CPU
    return DecodeBackend::CPU_SOFTWARE;
}

bool VideoFileInput::readFrameToTexture(int64_t frameNumber, GPUTextureFrameBuffer& textureBuffer) {
    if (!isReady() || !useHardwareDecoding_) {
        // Hardware decoding not available or not enabled
        return false;
    }

    // Seek to frame if needed
    if (!seek(frameNumber)) {
        return false;
    }

    // Decode frame using hardware decoder
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
                --bailout;
                continue;
            } else {
                av_packet_free(&packet);
                return false;
            }
        }

        if (packet->stream_index != videoStream_) {
            continue;
        }

        // Send packet to hardware decoder
        err = avcodec_send_packet(codecCtx_, packet);

        if (err < 0) {
            --bailout;
            continue;
        }

        // Receive frame from hardware decoder
        err = avcodec_receive_frame(codecCtx_, hwFrame_);
        if (err == 0) {
            frameFinished = true;
        } else if (err == AVERROR(EAGAIN)) {
            // Need more input
            --bailout;
            continue;
        } else {
            --bailout;
            continue;
        }
    }

    if (!frameFinished) {
        av_packet_free(&packet);
        return false;
    }

    av_packet_free(&packet);

    // Transfer hardware frame to GPU texture
    return transferHardwareFrameToGPU(hwFrame_, textureBuffer);
}

bool VideoFileInput::transferHardwareFrameToGPU(AVFrame* hwFrame, GPUTextureFrameBuffer& textureBuffer) {
    if (!hwFrame || !hwFrame->data[0]) {
        return false;
    }

    // Hardware frames need to be transferred to GPU texture
    // The exact method depends on the hardware decoder type
    
    // For now, we'll need to download from hardware frame to CPU,
    // then upload to GPU texture. In the future, we can optimize this
    // to use zero-copy methods (e.g., CUDA interop, VAAPI surface export)
    
    // Allocate CPU frame for download
    if (!frame_) {
        frame_ = av_frame_alloc();
        if (!frame_) {
            return false;
        }
    }

    // Download from hardware frame to CPU frame
    int ret = av_hwframe_transfer_data(frame_, hwFrame, 0);
    if (ret < 0) {
        return false;
    }

    // Convert CPU frame to GPU texture
    // For now, we'll use the existing CPU path and upload to GPU
    // TODO: Implement zero-copy hardware frame to GPU texture transfer
    
    // Allocate texture buffer if needed
    if (!textureBuffer.isValid() || 
        textureBuffer.info().width != frameInfo_.width || 
        textureBuffer.info().height != frameInfo_.height) {
        GLenum textureFormat = GL_RGBA; // Uncompressed texture
        if (!textureBuffer.allocate(frameInfo_, textureFormat, false)) {
            return false;
        }
    }

    // Convert frame format to RGBA and upload to GPU
    // Initialize sws context if needed
    if (!swsCtx_ || swsCtxWidth_ != frameInfo_.width || swsCtxHeight_ != frameInfo_.height) {
        if (swsCtx_) {
            sws_freeContext(swsCtx_);
            swsCtx_ = nullptr;
        }
        
        swsCtx_ = sws_getContext(
            frame_->width, frame_->height, static_cast<AVPixelFormat>(frame_->format),
            frameInfo_.width, frameInfo_.height, AV_PIX_FMT_RGBA,
            SWS_BICUBIC, nullptr, nullptr, nullptr
        );
        if (!swsCtx_) {
            return false;
        }
        swsCtxWidth_ = frameInfo_.width;
        swsCtxHeight_ = frameInfo_.height;
    }

    // Allocate temporary CPU buffer for RGBA data
    int rgbaStride = frameInfo_.width * 4;
    size_t rgbaSize = rgbaStride * frameInfo_.height;
    std::vector<uint8_t> rgbaBuffer(rgbaSize);

    // Prepare destination frame (RGBA)
    uint8_t* dstData[4] = {rgbaBuffer.data(), nullptr, nullptr, nullptr};
    int dstLinesize[4] = {rgbaStride, 0, 0, 0};
    
    // Scale and convert to RGBA
    int result = sws_scale(swsCtx_,
              (const uint8_t* const*)frame_->data, frame_->linesize,
              0, frame_->height,
              dstData, dstLinesize);
    
    if (result <= 0) {
        return false;
    }

    // Upload RGBA data to GPU texture
    // Note: uploadUncompressedData expects stride in bytes, which we already have
    if (!textureBuffer.uploadUncompressedData(rgbaBuffer.data(), rgbaSize,
                                             frameInfo_.width, frameInfo_.height,
                                             GL_RGBA)) {
        return false;
    }

    return true;
}

void VideoFileInput::cleanup() {
    // Free frame index
    if (frameIndex_) {
        free(frameIndex_);
        frameIndex_ = nullptr;
    }
    frameCount_ = 0;

    // Free software scaler
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }
    swsCtxWidth_ = 0;
    swsCtxHeight_ = 0;

    // Free hardware frames (must use av_frame_free for frames allocated with av_frame_alloc)
    if (hwFrame_) {
        av_frame_free(&hwFrame_);
        hwFrame_ = nullptr;
    }

    // Free frames (must use av_frame_free for frames allocated with av_frame_alloc)
    if (frameFMT_) {
        av_frame_free(&frameFMT_);
        frameFMT_ = nullptr;
    }
    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }

    // Free hardware device context
    if (hwDeviceCtx_) {
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
    }

    // Close video decoder (for software decoding)
    if (!useHardwareDecoding_) {
        videoDecoder_.close();
        codecCtx_ = nullptr;
    } else {
        // For hardware decoding, we need to manually close
        if (codecCtx_) {
            avcodec_close(codecCtx_);
            if (codecCtxAllocated_) {
                avcodec_free_context(&codecCtx_);
            }
            codecCtx_ = nullptr;
        }
    }

    // Close media reader (closes format context)
    mediaReader_.close();
    formatCtx_ = nullptr;

    videoStream_ = -1;
    lastDecodedPTS_ = -1;
    lastDecodedFrameNo_ = -1;
    scanComplete_ = false;
    currentFrame_ = -1;
    useHardwareDecoding_ = false;
    hwDecoderType_ = HardwareDecoder::Type::NONE;
}

} // namespace videocomposer

