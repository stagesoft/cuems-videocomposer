#include "VideoFileInput.h"
#include "../../ffcompat.h"
#include "../utils/CLegacyBridge.h"
#include "../utils/Logger.h"
#include <cstring>
#include <cassert>
#include <algorithm>
#include <vector>

#ifdef HAVE_VAAPI_INTEROP
#include "../hwdec/VaapiInterop.h"
#include "../display/DisplayBackend.h"
#endif

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
#include <libavutil/version.h>
#include <libavutil/frame.h>
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
    , swsCtxFormat_(AV_PIX_FMT_NONE)
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
    , hwPreference_(HardwareDecodePreference::AUTO)
#ifdef HAVE_VAAPI_INTEROP
    , vaapiInterop_(nullptr)
    , displayBackend_(nullptr)
#endif
    , useAsyncDecode_(false)
{
    frameRateQ_ = {1, 1};
    frameInfo_ = {};
}

VideoFileInput::~VideoFileInput() {
    stopAsyncDecode();
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

    ready_ = true;
    currentFrame_ = -1;
    
    // Initialize async decode queue for hardware decoding
    // This provides mpv-style pre-buffering for smooth playback
    if (useHardwareDecoding_ && hwDeviceCtx_) {
        asyncDecodeQueue_ = std::make_unique<AsyncDecodeQueue>();
        if (asyncDecodeQueue_->open(currentFile_, hwDeviceCtx_)) {
            useAsyncDecode_ = true;
            LOG_INFO << "Async decode queue enabled for smooth hardware decoding";
        } else {
            LOG_WARNING << "Failed to initialize async decode queue, using synchronous decode";
            asyncDecodeQueue_.reset();
            useAsyncDecode_ = false;
        }
    }
    
    return true;
}

void VideoFileInput::close() {
    // Stop async decode queue first
    if (asyncDecodeQueue_) {
        asyncDecodeQueue_->close();
        asyncDecodeQueue_.reset();
    }
    useAsyncDecode_ = false;
    
    // Stop async decode thread first
    stopAsyncDecode();
    
    // Clear frame cache
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        frameCache_.clear();
    }
    
    cleanup();
    currentFile_.clear();
    ready_ = false;
    currentFrame_ = -1;
    frameInfo_ = {};
    
    // NOTE: Don't reset C globals here. In multi-layer scenarios,
    // resetting would break other layers that are still using the globals.
    // The globals will be reset when the application exits or when
    // the last layer is removed.
}

bool VideoFileInput::isReady() const {
    return ready_ && mediaReader_.isReady();
}

// Helper function to check if codec parameters are compatible with hardware decoding
static bool isCompatibleWithHardwareDecoder(AVCodecParameters* codecParams, HardwareDecoder::Type hwType) {
    if (!codecParams) {
        return false;
    }
    
    AVCodecID codecId = codecParams->codec_id;
    
    // Check for known incompatibilities based on bit depth and profile
    if (codecId == AV_CODEC_ID_H264) {
        // H.264 hardware decoders typically only support 8-bit (baseline, main, high profiles)
        // High 10 Profile (profile 110) is 10-bit and not supported by most hardware decoders
        // Check bits_per_raw_sample first (most reliable)
        // Note: 0 means unknown/unset and typically defaults to 8-bit, so only check if > 8
        if (codecParams->bits_per_raw_sample > 8) {
            LOG_INFO << "H.264 video has " << codecParams->bits_per_raw_sample 
                     << "-bit depth, not supported by hardware decoder, falling back to software";
            return false;
        }
        
        // Also check profile: High 10 Profile (110) is 10-bit
        // Baseline = 66, Main = 77, High = 100, High 10 = 110
        // Check if profile value is 110 (High 10 Profile)
        if (codecParams->profile == 110) {
            LOG_INFO << "H.264 High 10 Profile detected (10-bit), not supported by hardware decoder, falling back to software";
            return false;
        }
    } else if (codecId == AV_CODEC_ID_HEVC) {
        // HEVC hardware decoder support varies, but many don't support 12-bit
        // Check bits_per_raw_sample (0 means unknown/unset)
        if (codecParams->bits_per_raw_sample > 10) {
            LOG_INFO << "HEVC video has " << codecParams->bits_per_raw_sample 
                     << "-bit depth, may not be supported by hardware decoder, falling back to software";
            return false;
        }
    }
    
    // Additional compatibility check: try to query hardware decoder capabilities
    // Get the codec we would use for hardware decoding
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 0, 0)
    const AVCodec* codec = nullptr;
#else
    AVCodec* codec = nullptr;
#endif
    
    // Find the appropriate hardware codec
    switch (hwType) {
        case HardwareDecoder::Type::VAAPI:
        case HardwareDecoder::Type::VIDEOTOOLBOX:
            // These use standard decoder with hw_device_ctx
            codec = avcodec_find_decoder(codecId);
            break;
        case HardwareDecoder::Type::CUDA: {
            std::string codecName = avcodec_get_name(codecId);
            codec = avcodec_find_decoder_by_name((codecName + "_cuvid").c_str());
            break;
        }
        case HardwareDecoder::Type::QSV: {
            std::string codecName = avcodec_get_name(codecId);
            codec = avcodec_find_decoder_by_name((codecName + "_qsv").c_str());
            break;
        }
        case HardwareDecoder::Type::DXVA2: {
            std::string codecName = avcodec_get_name(codecId);
            codec = avcodec_find_decoder_by_name((codecName + "_dxva2").c_str());
            break;
        }
        default:
            return false;
    }
    
    if (!codec) {
        return false;
    }
    
    // Check hardware configurations to see if the decoder supports this format
    AVHWDeviceType hwDeviceType = HardwareDecoder::getFFmpegDeviceType(hwType);
    if (hwDeviceType == AV_HWDEVICE_TYPE_NONE) {
        return false;
    }
    
    // Check if there's a hardware config that supports this device type
    bool hasCompatibleConfig = false;
    for (int n = 0; ; n++) {
        const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, n);
        if (!cfg) {
            break;
        }
        
        if (cfg->device_type == hwDeviceType) {
            hasCompatibleConfig = true;
            break;
        }
    }
    
    if (!hasCompatibleConfig) {
        LOG_VERBOSE << "No compatible hardware configuration found for codec, will use software";
        return false;
    }
    
    return true;
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

    if (hwPreference_ == HardwareDecodePreference::SOFTWARE_ONLY) {
        LOG_VERBOSE << "Hardware decoding disabled via preference, using software decoder";
        return false;
    }

    LOG_INFO << "Attempting to open hardware decoder for codec: " << codecName;

    bool forceSpecificDecoder = false;
    HardwareDecoder::Type forcedType = HardwareDecoder::Type::NONE;
    switch (hwPreference_) {
        case HardwareDecodePreference::VAAPI:
            forceSpecificDecoder = true;
            forcedType = HardwareDecoder::Type::VAAPI;
            break;
        case HardwareDecodePreference::CUDA:
            forceSpecificDecoder = true;
            forcedType = HardwareDecoder::Type::CUDA;
            break;
        default:
            break;
    }

    if (forceSpecificDecoder) {
        hwDecoderType_ = forcedType;
    } else {
        hwDecoderType_ = HardwareDecoder::detectAvailable();
    }

    if (hwDecoderType_ == HardwareDecoder::Type::NONE) {
        if (forceSpecificDecoder) {
            LOG_WARNING << "Requested hardware decoder (" << HardwareDecoder::getName(forcedType)
                        << ") not available on this system, falling back to software decoding";
        }
        return false;
    }

    if (!HardwareDecoder::isAvailableForCodec(codecId, hwDecoderType_)) {
        if (forceSpecificDecoder) {
            LOG_WARNING << "Requested hardware decoder (" << HardwareDecoder::getName(forcedType)
                        << ") is not available for codec " << codecName << ", falling back to software";
        }
        return false;
    }

    // Check if the codec parameters are compatible with the hardware decoder
    // This detects incompatibilities like 10-bit H.264, unsupported profiles, etc.
    if (!isCompatibleWithHardwareDecoder(codecParams, hwDecoderType_)) {
        LOG_INFO << "Codec format not compatible with hardware decoder, falling back to software";
        return false;
    }

    // Find hardware decoder
    // NOTE: Different hardware decoders work differently:
    // - QSV/CUVID/DXVA2: Have dedicated wrapper decoders (h264_qsv, h264_cuvid)
    // - VAAPI/VideoToolbox: Use standard decoder with hw_device_ctx attached
    
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 0, 0)
    const AVCodec* hwCodec = nullptr;
#else
    AVCodec* hwCodec = nullptr;
#endif
    std::string hwCodecName;

    switch (hwDecoderType_) {
        case HardwareDecoder::Type::VAAPI:
        case HardwareDecoder::Type::VIDEOTOOLBOX:
            // VAAPI and VideoToolbox use the standard decoder with hw_device_ctx (hwaccel method)
            hwCodec = avcodec_find_decoder(codecId);
            hwCodecName = codecName + " (with " + HardwareDecoder::getName(hwDecoderType_) + " hwaccel)";
            break;
        case HardwareDecoder::Type::CUDA:
            // FFmpeg uses "cuvid" suffix for NVIDIA CUDA decoders
            hwCodecName = codecName + "_cuvid";
            hwCodec = avcodec_find_decoder_by_name(hwCodecName.c_str());
            break;
        case HardwareDecoder::Type::QSV:
            hwCodecName = codecName + "_qsv";
            hwCodec = avcodec_find_decoder_by_name(hwCodecName.c_str());
            break;
        case HardwareDecoder::Type::DXVA2:
            hwCodecName = codecName + "_dxva2";
            hwCodec = avcodec_find_decoder_by_name(hwCodecName.c_str());
            break;
        default:
            return false;
    }

    if (!hwCodec) {
        LOG_WARNING << "Hardware decoder " << hwCodecName << " not found, falling back to software";
        return false;
    }
    
    LOG_INFO << "Found hardware decoder: " << hwCodecName;

    // Get hardware device type first (needed to filter hw_configs)
    AVHWDeviceType hwDeviceType = HardwareDecoder::getFFmpegDeviceType(hwDecoderType_);
    if (hwDeviceType == AV_HWDEVICE_TYPE_NONE) {
        return false;
    }

    // Check hardware config methods to determine how to initialize the decoder
    // Following mpv's approach: check what methods the codec supports for OUR device type
    // - METHOD_HW_DEVICE_CTX: needs hw_device_ctx set on codec context (VAAPI, VideoToolbox)
    // - METHOD_HW_FRAMES_CTX: needs hw_frames_ctx
    // - METHOD_INTERNAL: wrapper decoder (like cuvid, qsv) that manages hardware internally
    bool needsHwDeviceCtx = false;
    bool needsHwFramesCtx = false;
    AVPixelFormat hwPixFmt = AV_PIX_FMT_NONE;
    
    for (int n = 0; ; n++) {
        const AVCodecHWConfig *cfg = avcodec_get_hw_config(hwCodec, n);
        if (!cfg)
            break;
        
        // Only consider configs for our target device type
        if (cfg->device_type != hwDeviceType)
            continue;
        
        if (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) {
            needsHwDeviceCtx = true;
            hwPixFmt = cfg->pix_fmt;
            LOG_VERBOSE << "HW config: device_type=" << cfg->device_type 
                       << " method=HW_DEVICE_CTX pix_fmt=" << av_get_pix_fmt_name(cfg->pix_fmt);
        }
        if (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX) {
            needsHwFramesCtx = true;
            if (hwPixFmt == AV_PIX_FMT_NONE) {
                hwPixFmt = cfg->pix_fmt;
            }
            LOG_VERBOSE << "HW config: device_type=" << cfg->device_type 
                       << " method=HW_FRAMES_CTX pix_fmt=" << av_get_pix_fmt_name(cfg->pix_fmt);
        }
        // AV_CODEC_HW_CONFIG_METHOD_INTERNAL means it's a wrapper decoder
        // (like cuvid, qsv) that manages hardware internally
        if (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_INTERNAL) {
            needsHwDeviceCtx = false;
            needsHwFramesCtx = false;
            LOG_VERBOSE << "HW config: device_type=" << cfg->device_type 
                       << " method=INTERNAL - wrapper decoder";
            break;
        }
    }
    
    // If no hardware config found, fall back to our detection logic
    if (hwPixFmt == AV_PIX_FMT_NONE) {
        hwPixFmt = HardwareDecoder::getHardwarePixelFormat(hwDecoderType_, codecId);
    }

    // Create hardware device context (needed for frame transfers even if not for codec)
    int ret = -1;
    
#ifdef HAVE_VAAPI_INTEROP
    // For VAAPI: use shared VADisplay from VaapiInterop for zero-copy support
    // This ensures the decoder and EGL interop use the same VAAPI display
    if (hwDeviceType == AV_HWDEVICE_TYPE_VAAPI && vaapiInterop_ && vaapiInterop_->getVADisplay()) {
        VADisplay sharedDisplay = vaapiInterop_->getVADisplay();
        
        hwDeviceCtx_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VAAPI);
        if (hwDeviceCtx_) {
            AVHWDeviceContext* hwctx = (AVHWDeviceContext*)hwDeviceCtx_->data;
            AVVAAPIDeviceContext* vactx = (AVVAAPIDeviceContext*)hwctx->hwctx;
            vactx->display = sharedDisplay;
            
            ret = av_hwdevice_ctx_init(hwDeviceCtx_);
            if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
                LOG_WARNING << "Failed to init shared VAAPI device: " << errbuf;
                av_buffer_unref(&hwDeviceCtx_);
                hwDeviceCtx_ = nullptr;
            } else {
                LOG_INFO << "Using shared VADisplay for VAAPI zero-copy";
            }
        }
    }
#endif
    
    // Fallback: create device context normally (creates its own display)
    if (!hwDeviceCtx_) {
        ret = av_hwdevice_ctx_create(&hwDeviceCtx_, hwDeviceType, nullptr, nullptr, 0);
        if (ret < 0) {
            hwDeviceCtx_ = nullptr;
            return false;
        }
    }

    // Allocate codec context for hardware decoder
    codecCtx_ = avcodec_alloc_context3(hwCodec);
    if (!codecCtx_) {
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
        return false;
    }
    codecCtxAllocated_ = true;  // Mark as allocated (must be freed)

    // Set codec type and ID before copying parameters (following mpv's approach)
    codecCtx_->codec_type = AVMEDIA_TYPE_VIDEO;
    codecCtx_->codec_id = hwCodec->id;

    // Set packet timebase from stream BEFORE copying parameters (required for cuvid and other hardware decoders)
    // This prevents "Invalid pkt_timebase" warnings and ensures correct timestamp handling
    AVStream* avStream = mediaReader_.getStream(videoStream_);
    if (avStream) {
        codecCtx_->pkt_timebase = avStream->time_base;
    }

    // Copy codec parameters from stream
    if (avcodec_parameters_to_context(codecCtx_, codecParams) < 0) {
        avcodec_free_context(&codecCtx_);
        codecCtx_ = nullptr;
        av_buffer_unref(&hwDeviceCtx_);
        hwDeviceCtx_ = nullptr;
        return false;
    }

    // Set hardware acceleration flags (following mpv's approach)
    codecCtx_->hwaccel_flags |= AV_HWACCEL_FLAG_IGNORE_LEVEL;
    codecCtx_->hwaccel_flags |= AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;
#ifdef AV_HWACCEL_FLAG_UNSAFE_OUTPUT
    // This flag primarily exists for nvdec which has a very limited output frame pool
    // We copy frames anyway, so we don't need this extra implicit copy
    codecCtx_->hwaccel_flags |= AV_HWACCEL_FLAG_UNSAFE_OUTPUT;
#endif

    // Set hardware device context and pixel format callback based on codec requirements
    // This applies uniformly to all decoder types (cuvid, vaapi, qsv, etc.)
    // Wrapper decoders (with METHOD_INTERNAL) manage hardware internally
    // and don't need hw_device_ctx set on the codec context
    if (needsHwDeviceCtx || needsHwFramesCtx) {
        // Set hardware device context
        if (needsHwDeviceCtx) {
            codecCtx_->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
            if (!codecCtx_->hw_device_ctx) {
                avcodec_free_context(&codecCtx_);
                codecCtx_ = nullptr;
                av_buffer_unref(&hwDeviceCtx_);
                hwDeviceCtx_ = nullptr;
                return false;
            }
        }
        
        // Set hardware pixel format callback if we have a valid pixel format
        if (hwPixFmt == AV_PIX_FMT_NONE) {
            avcodec_free_context(&codecCtx_);
            codecCtx_ = nullptr;
            av_buffer_unref(&hwDeviceCtx_);
            hwDeviceCtx_ = nullptr;
            return false;
        }

        // Set hardware pixel format callback
        // Store hwPixFmt value in opaque (cast value to pointer, not storing a pointer)
        codecCtx_->opaque = reinterpret_cast<void*>(static_cast<intptr_t>(hwPixFmt));
        codecCtx_->get_format = [](AVCodecContext* ctx, const AVPixelFormat* pix_fmts) -> AVPixelFormat {
            // Cast opaque back to the enum value (it's a value stored as pointer, not a pointer)
            AVPixelFormat hwPixFmt = static_cast<AVPixelFormat>(reinterpret_cast<intptr_t>(ctx->opaque));
            for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
                if (*p == hwPixFmt) {
                    return *p;
                }
            }
            return AV_PIX_FMT_NONE;
        };
    }
    // Wrapper decoders (with METHOD_INTERNAL, like cuvid) handle hardware and pixel format
    // selection internally, but we keep hwDeviceCtx_ for frame transfers
    
    // CRITICAL: Request extra surfaces for zero-copy rendering (like mpv does)
    // The default pool size is determined by the decoder (usually ~17 for H.264)
    // But with EGL image lifecycle delays (we keep textures/EGL images alive for 1 frame),
    // we need extra surfaces to prevent pool exhaustion
    // MPV uses hwdec_extra_frames=6, we use more to account for our architecture
    codecCtx_->extra_hw_frames = 20;  // Request 20 extra surfaces

    // Open hardware codec
    ret = avcodec_open2(codecCtx_, hwCodec, nullptr);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        
        // Provide more helpful error messages for specific codecs
        if (codecId == AV_CODEC_ID_AV1 && hwDecoderType_ == HardwareDecoder::Type::CUDA) {
            LOG_WARNING << "AV1 CUDA hardware decoding not supported by this GPU. "
                        << "AV1 CUDA requires Ada Lovelace architecture (RTX 40 series or newer). "
                        << "Falling back to software decoding.";
        } else {
            LOG_WARNING << "Failed to open hardware decoder " << hwCodecName 
                        << ": " << errbuf << " (error code: " << ret << "), falling back to software";
        }
        
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

// Forward declaration of FrameIndex structure (matches VideoFileInput::FrameIndex layout)
// Note: This is a duplicate struct definition for helper functions since VideoFileInput::FrameIndex is private
struct LocalFrameIndex {
    int64_t pkt_pts;
    int64_t pkt_pos;
    int64_t frame_pts;
    int64_t frame_pos;
    int64_t timestamp;
    int64_t seekpts;
    int64_t seekpos;
    uint8_t key;
};

// Helper function to add index entry (like xjadeo's add_idx)
static int addIndexEntry(LocalFrameIndex* frameIndex, int64_t fcnt, int64_t frames,
                         int64_t ts, int64_t pos, uint8_t key, AVRational fr_Q, AVRational tb) {
    if (fcnt >= frames) {
        // Overflow - will be handled by caller
        return -1;
    }
    
    frameIndex[fcnt].pkt_pts = ts;
    frameIndex[fcnt].pkt_pos = pos;
    frameIndex[fcnt].timestamp = av_rescale_q(fcnt, fr_Q, tb);
    frameIndex[fcnt].key = key;
    frameIndex[fcnt].frame_pts = -1;
    frameIndex[fcnt].frame_pos = -1;
    frameIndex[fcnt].seekpts = 0;
    frameIndex[fcnt].seekpos = 0;
    
    return 0;
}

// Helper function to find keyframe for a given timestamp (like xjadeo's keyframe_lookup_helper)
static int64_t keyframeLookupHelper(LocalFrameIndex* frameIndex, int64_t fcnt,
                                     int64_t last, int64_t ts) {
    if (last >= fcnt) {
        last = fcnt - 1;
    }
    
    for (int64_t i = last; i >= 0; --i) {
        if (!frameIndex[i].key) continue;
        if (frameIndex[i].pkt_pts == AV_NOPTS_VALUE) {
            continue;
        }
        
        // For hardware decoding, frame_pts may not be set (Pass 2 doesn't verify keyframes)
        // In that case, use timestamp instead
        // Note: frame_pts can be -1 (uninitialized) or AV_NOPTS_VALUE (0x8000000000000000)
        int64_t keyframePts = (frameIndex[i].frame_pts != AV_NOPTS_VALUE && frameIndex[i].frame_pts >= 0) 
                              ? frameIndex[i].frame_pts 
                              : frameIndex[i].timestamp;
        
        if (keyframePts <= ts) {
            return i;
        }
    }
    return -1;
}

bool VideoFileInput::isIntraFrameCodec() const {
    // Check if codec is intra-frame only (all frames are keyframes)
    // These codecs don't need indexing - direct seek mode works perfectly
    if (!codecCtx_) {
        return false;
    }
    
    AVCodecID codecId = codecCtx_->codec_id;
    
    switch (codecId) {
        // Professional intra-frame codecs
        case AV_CODEC_ID_PRORES:     // Apple ProRes (all variants)
        case AV_CODEC_ID_DNXHD:      // Avid DNxHD/DNxHR
        case AV_CODEC_ID_MJPEG:      // Motion JPEG
        case AV_CODEC_ID_MJPEGB:     // Motion JPEG-B
        case AV_CODEC_ID_RAWVIDEO:   // Uncompressed video
        case AV_CODEC_ID_V210:       // Uncompressed 10-bit 4:2:2
        case AV_CODEC_ID_V410:       // Uncompressed 10-bit 4:4:4
        case AV_CODEC_ID_R210:       // Uncompressed RGB 10-bit
        case AV_CODEC_ID_R10K:       // AJA Kona 10-bit RGB
        case AV_CODEC_ID_AVUI:       // Avid Meridien Uncompressed
        case AV_CODEC_ID_AYUV:       // Uncompressed packed 4:4:4
        case AV_CODEC_ID_TARGA_Y216: // Pinnacle TARGA CineWave YUV16
        case AV_CODEC_ID_JPEG2000:   // JPEG 2000 (intra-frame)
        #ifdef AV_CODEC_ID_CINEFORM
        case AV_CODEC_ID_CINEFORM:   // GoPro CineForm
        #endif
            return true;
        
        // HAP is handled by HAPVideoInput, but include here for completeness
        case AV_CODEC_ID_HAP:
        #ifdef AV_CODEC_ID_HAPQ
        case AV_CODEC_ID_HAPQ:
        #endif
        #ifdef AV_CODEC_ID_HAPALPHA
        case AV_CODEC_ID_HAPALPHA:
        #endif
            return true;
        
        default:
            return false;
    }
}

void VideoFileInput::setupDirectSeekMode() {
    // Setup direct seek mode for intra-frame codecs
    // All frames are keyframes, so we can calculate positions mathematically
    
    AVStream* avStream = mediaReader_.getStream(videoStream_);
    if (!avStream) {
        return;
    }
    
    AVRational timeBase = avStream->time_base;
    int64_t frames = frameInfo_.totalFrames;
    if (frames <= 0) {
        double duration = mediaReader_.getDuration();
        if (duration > 0 && frameInfo_.framerate > 0) {
            frames = static_cast<int64_t>(duration * frameInfo_.framerate);
        } else {
            frames = 10000; // Default estimate
        }
    }
    
    // Allocate index for direct seek
    frameIndex_ = static_cast<FrameIndex*>(calloc(frames, sizeof(FrameIndex)));
    if (!frameIndex_) {
        return;
    }
    
    // Get first PTS from stream
    int64_t firstPTS = 0;
    if (avStream->start_time != AV_NOPTS_VALUE) {
        firstPTS = avStream->start_time;
    }
    
    // Fill index with calculated positions (all keyframes)
    for (int64_t i = 0; i < frames; ++i) {
        frameIndex_[i].key = 1;  // All keyframes
        frameIndex_[i].pkt_pts = firstPTS + av_rescale_q(i, frameRateQ_, timeBase);
        frameIndex_[i].frame_pts = frameIndex_[i].pkt_pts;
        frameIndex_[i].pkt_pos = -1;  // Use timestamp seeking
        frameIndex_[i].frame_pos = -1;
        frameIndex_[i].timestamp = av_rescale_q(i, frameRateQ_, timeBase);
        frameIndex_[i].seekpts = frameIndex_[i].pkt_pts;
        frameIndex_[i].seekpos = -1;
    }
    
    frameCount_ = frames;
    scanComplete_ = true;
    byteSeek_ = false;  // Use timestamp seeking
}

bool VideoFileInput::indexFrames() {
    // Check if codec is intra-frame only (all keyframes)
    // These codecs don't need the expensive 3-pass indexing
    if (isIntraFrameCodec()) {
        const char* codecName = codecCtx_ ? avcodec_get_name(codecCtx_->codec_id) : "unknown";
        LOG_INFO << "Codec " << codecName << " is intra-frame only (all keyframes), skipping indexing";
        setupDirectSeekMode();
        return scanComplete_;
    }
    
    // xjadeo-style 3-pass indexing implementation

    frameCount_ = 0;
    frameIndex_ = nullptr;

    AVStream* avStream = mediaReader_.getStream(videoStream_);
    if (!avStream) {
        return false;
    }
    
    AVRational timeBase = avStream->time_base;
    int64_t frames = frameInfo_.totalFrames;
    if (frames <= 0) {
        // Estimate frames from duration if not available
        double duration = mediaReader_.getDuration();
        if (duration > 0 && frameInfo_.framerate > 0) {
            frames = static_cast<int64_t>(duration * frameInfo_.framerate);
        } else {
            frames = 100000; // Large default, will grow as needed
        }
    }
    
    // Allocate index array (cast to our local FrameIndex type for helper functions)
    frameIndex_ = static_cast<VideoFileInput::FrameIndex*>(calloc(frames, sizeof(VideoFileInput::FrameIndex)));
    if (!frameIndex_) {
        return false;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        free(frameIndex_);
        frameIndex_ = nullptr;
        return false;
    }
    
    int use_dts = 0;
    int max_keyframe_interval = 0;
    int keyframe_interval = 0;
    int64_t keyframe_byte_pos = 0;
    int64_t keyframe_byte_distance = 0;
    const int keyframe_interval_limit = 300; // xjadeo default
    
    LOG_INFO << "Indexing video (Pass 1: Scanning packets)...";
    
    /* Pass 1: read all packets
     * -> find keyframes
     * -> check if file is complete
     * -> discover max. keyframe distance
     * -> get PTS/DTS of every *packet*
     */
    while (mediaReader_.readPacket(packet) == 0) {
        if (packet->stream_index != videoStream_) {
            av_packet_unref(packet);
            continue;
        }

        int64_t ts = AV_NOPTS_VALUE;
        
        // Try PTS first, fallback to DTS
        if (!use_dts && packet->pts != AV_NOPTS_VALUE) {
            ts = packet->pts;
        }
        if (ts == AV_NOPTS_VALUE) {
            use_dts = 1;
        }
        if (use_dts && packet->dts != AV_NOPTS_VALUE) {
            ts = packet->dts;
        }

        if (ts == AV_NOPTS_VALUE) {
            LOG_WARNING << "Index error: no PTS, nor DTS at frame " << frameCount_;
            av_packet_unref(packet);
            break;
        }
        
        const uint8_t key = (packet->flags & AV_PKT_FLAG_KEY) ? 1 : 0;

        // Grow array if needed
        if (frameCount_ >= frames) {
            frames *= 2;
            frameIndex_ = static_cast<FrameIndex*>(realloc(frameIndex_, frames * sizeof(FrameIndex)));
            if (!frameIndex_) {
                av_packet_unref(packet);
                av_packet_free(&packet);
                return false;
            }
        }

        if (addIndexEntry(reinterpret_cast<LocalFrameIndex*>(frameIndex_), frameCount_, frames, ts, packet->pos, key, frameRateQ_, timeBase) < 0) {
            av_packet_unref(packet);
            break;
        }
        
        if (key) {
            int64_t byte_distance = packet->pos - keyframe_byte_pos;
            keyframe_byte_pos = packet->pos;
            if (keyframe_byte_distance < byte_distance) {
                keyframe_byte_distance = byte_distance;
            }
        }
        
        av_packet_unref(packet);
        
        if (++keyframe_interval > max_keyframe_interval) {
            max_keyframe_interval = keyframe_interval;
        }
        
        // Check for problematic files (too many frames between keyframes)
        if (max_keyframe_interval > keyframe_interval_limit &&
            (keyframe_byte_distance > 0 && keyframe_byte_distance > 5242880 /* 5 MB */)) {
            LOG_WARNING << "Keyframe interval too large, stopping indexing";
            break;
        }
        
        // Optimization: if first 500 frames are all keyframes, use direct seek mode
        if ((frameCount_ == 500 || frameCount_ == frames) && max_keyframe_interval == 1) {
            int64_t file_frame_offset = frameInfo_.fileFrameOffset;
            int64_t ppts_offset = frameIndex_[0].pkt_pts;
            // Check if file_frame_offset matches packet PTS (like xjadeo)
            if (file_frame_offset == av_rescale_q(ppts_offset, timeBase, frameRateQ_) || file_frame_offset == 0) {
                LOG_INFO << "First 500 frames are all keyframes. Using direct seek mode.";
                // Fill in all frames as keyframes (only if we know total frames)
                if (frameInfo_.totalFrames > 0) {
                    for (int64_t i = 0; i < frameInfo_.totalFrames && i < frames; ++i) {
                        frameIndex_[i].key = 1;
                        frameIndex_[i].pkt_pts = frameIndex_[i].frame_pts = 
                            ppts_offset + av_rescale_q(i, frameRateQ_, timeBase);
                        frameIndex_[i].frame_pos = -1;
                        frameIndex_[i].timestamp = av_rescale_q(file_frame_offset + i, frameRateQ_, timeBase);
                        frameIndex_[i].seekpts = frameIndex_[i].pkt_pts;
                        frameIndex_[i].seekpos = frameIndex_[i].pkt_pos;
                    }
                    frameCount_ = frameInfo_.totalFrames;
                } else {
                    // Use current frame count
                    frameCount_ = frameCount_; // Already set
                }
                av_packet_free(&packet);
                scanComplete_ = true;
                return true;
            }
        }
        
        if (key) {
            keyframe_interval = 0;
        }

        frameCount_++;
    }
    
    av_packet_free(&packet);
    
    if (frameCount_ == 0) {
        LOG_ERROR << "No frames indexed";
        free(frameIndex_);
        frameIndex_ = nullptr;
        return false;
    }
    
    // Resize array to actual frame count
    if (frameCount_ < frames) {
        frameIndex_ = static_cast<FrameIndex*>(realloc(frameIndex_, frameCount_ * sizeof(FrameIndex)));
        if (!frameIndex_) {
            free(frameIndex_);
            frameIndex_ = nullptr;
            return false;
        }
    }
    
    LOG_INFO << "Pass 1 complete: indexed " << frameCount_ << " packets, max keyframe interval: " << max_keyframe_interval;
    
    /* Pass 2: verify keyframes
     * seek to [all] keyframe, decode one frame after
     * the keyframe and check *frame* PTS
     */
    LOG_INFO << "Indexing video (Pass 2: Verifying keyframes)...";
    
    int64_t keyframecount = 0;
    
    // Need a frame for decoding
    if (!frame_) {
        frame_ = av_frame_alloc();
        if (!frame_) {
            LOG_ERROR << "Failed to allocate frame for indexing";
            free(frameIndex_);
            frameIndex_ = nullptr;
            return false;
        }
    }
    
    for (int64_t i = 0; i < frameCount_; ++i) {
        if (!frameIndex_[i].key) continue;
        
        // Seek to keyframe
        if (!mediaReader_.seek(frameIndex_[i].pkt_pts, videoStream_, AVSEEK_FLAG_BACKWARD)) {
            LOG_WARNING << "IDX2: Seek failed for keyframe " << i;
            continue;
        }
        
        // Flush codec buffers
        // codec->flush was removed in FFmpeg 4.0+, but avcodec_flush_buffers() is always available
        if (codecCtx_) {
            avcodec_flush_buffers(codecCtx_);
        }
        
        // Decode one frame
        bool got_pic = false;
        int64_t pts = AV_NOPTS_VALUE;
        int bailout = 100;
        
        while (!got_pic && --bailout > 0) {
            AVPacket* decodePacket = av_packet_alloc();
            if (!decodePacket) {
                break;
            }
            
            int err = mediaReader_.readPacket(decodePacket);
            if (err < 0) {
                if (err == AVERROR_EOF) {
                    LOG_WARNING << "IDX2: Read/Seek compensate for premature EOF at keyframe " << i;
                    frameIndex_[i].key = 0;
                }
                av_packet_free(&decodePacket);
                break;
            }
            
            if (decodePacket->stream_index == videoStream_) {
                // Send packet to decoder
                err = videoDecoder_.sendPacket(decodePacket);
                if (err < 0 && err != AVERROR(EAGAIN)) {
                    av_packet_free(&decodePacket);
                    break;
                }
                
                // Receive frame from decoder
                err = videoDecoder_.receiveFrame(frame_);
                if (err == 0) {
                    got_pic = true;
                    pts = parsePTSFromFrame(frame_);
                } else if (err != AVERROR(EAGAIN)) {
                    av_packet_free(&decodePacket);
                    break;
                }
            }
            
            av_packet_free(&decodePacket);
        }
        
        if (!got_pic || pts == AV_NOPTS_VALUE) {
            continue;
        }
        
        frameIndex_[i].frame_pts = pts;
        // Use compatibility wrapper from ffcompat.h
        frameIndex_[i].frame_pos = av_frame_get_pkt_pos(frame_);
        if (pts != AV_NOPTS_VALUE) {
            keyframecount++;
        }
    }
    
    LOG_INFO << "Pass 2 complete: verified " << keyframecount << " keyframes";
    
    /* Pass 3: Create Seek-Table
     * -> assign seek-[key]frame to every frame
     */
    LOG_INFO << "Indexing video (Pass 3: Creating seek table)...";
    
    for (int64_t i = 0; i < frameCount_; ++i) {
        int64_t searchLimit = std::min(frameCount_ - 1, i + 2 + max_keyframe_interval);
        int64_t kfi = keyframeLookupHelper(reinterpret_cast<LocalFrameIndex*>(frameIndex_), frameCount_, searchLimit, frameIndex_[i].timestamp);
        
        if (kfi < 0) {
            frameIndex_[i].seekpts = 0;
            frameIndex_[i].seekpos = 0;
        } else {
            frameIndex_[i].seekpts = frameIndex_[kfi].pkt_pts;
            frameIndex_[i].seekpos = frameIndex_[kfi].frame_pos;
        }
    }
    
    LOG_INFO << "Pass 3 complete: seek table created";
    
    // Update total frames if we discovered more frames than estimated
    if (frameCount_ > frameInfo_.totalFrames) {
        frameInfo_.totalFrames = frameCount_;
    }

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

void VideoFileInput::resetSeekState() {
    // Reset internal tracking to force next seek to actually perform the seek
    // even if seeking to the same frame number (used for MTC full frame SYSEX)
    lastDecodedPTS_ = -1;
    lastDecodedFrameNo_ = -1;
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

        // Flush codec buffers (codec->flush was removed in FFmpeg 4.0+, but avcodec_flush_buffers() is always available)
        if (codecCtx_) {
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

    // Flush codec buffers (codec->flush was removed in FFmpeg 4.0+, but avcodec_flush_buffers() is always available)
    if (codecCtx_) {
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

    // QUICK WIN #1: Early return for same frame (xjadeo: if (!force_update && dispFrame == timestamp) return;)
    // If same frame is requested and we have valid decoded data, just re-run color conversion
    // This skips the expensive decode loop but still handles different output buffers
    if (currentFrame_ == frameNumber && frame_ && frame_->data[0]) {
        // Same frame requested - frame_ still has decoded YUV data
        // Just re-run color conversion to output buffer (much faster than re-decoding)
        if (!swsCtx_) {
            // No color conversion context - can't reuse
        } else {
            // Ensure output buffer is allocated
            if (!buffer.isValid() || buffer.info().width != frameInfo_.width ||
                buffer.info().height != frameInfo_.height) {
                FrameInfo outputInfo;
                outputInfo.width = frameInfo_.width;
                outputInfo.height = frameInfo_.height;
                outputInfo.format = PixelFormat::BGRA32;
                buffer.allocate(outputInfo);
            }
            
            // Re-run color conversion (frame_ → buffer)
            uint8_t* dstData[1] = { buffer.data() };
            int dstLinesize[1] = { static_cast<int>(frameInfo_.width * 4) };
            sws_scale(swsCtx_, frame_->data, frame_->linesize, 0, 
                      codecCtx_->height, dstData, dstLinesize);
            return true;
        }
    }

    // Check frame cache first (async pre-buffered frames)
    if (!useHardwareDecoding_) {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        CachedFrame* cached = findCachedFrame(frameNumber);
        if (cached && cached->valid) {
            // Found in cache - copy to output buffer
            if (!buffer.isValid() || buffer.info().width != cached->buffer.info().width ||
                buffer.info().height != cached->buffer.info().height) {
                buffer.allocate(cached->buffer.info());
            }
            memcpy(buffer.data(), cached->buffer.data(), 
                   cached->buffer.info().width * cached->buffer.info().height * 4);
            currentFrame_ = frameNumber;
            
            // Remove from cache (frame consumed)
            frameCache_.erase(
                std::remove_if(frameCache_.begin(), frameCache_.end(),
                    [frameNumber](const CachedFrame& cf) { return cf.frameNumber == frameNumber; }),
                frameCache_.end());
            
            // Start async decode for next frames
            startAsyncDecode(frameNumber);
            
            return true;
        }
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
            
            // Clear cache on seek (frames are no longer valid)
            if (!useHardwareDecoding_) {
                std::lock_guard<std::mutex> lock(cacheMutex_);
                frameCache_.clear();
            }
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
        
        // Convert from source format to BGRA32 for OpenGL
        // Use SWS_BILINEAR for better real-time performance (mpv default for scaling)
        // SWS_BICUBIC is higher quality but significantly slower for 10-bit content
        swsCtx_ = sws_getContext(
            codecCtx_->width, codecCtx_->height, codecCtx_->pix_fmt,
            frameInfo_.width, frameInfo_.height, AV_PIX_FMT_BGRA,
            SWS_BILINEAR, nullptr, nullptr, nullptr
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
    
    // Start async decode for next frames (software decoding only)
    if (!useHardwareDecoding_) {
        startAsyncDecode(frameNumber);
    }
    
    return true;
}

int64_t VideoFileInput::parsePTSFromFrame(AVFrame* frame) {
    int64_t pts = AV_NOPTS_VALUE;
    
    // Match xjadeo's parse_pts_from_frame logic
    // Try best effort timestamp first (using compatibility wrapper from ffcompat.h)
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(51, 49, 100)
    if (pts == AV_NOPTS_VALUE) {
        pts = av_frame_get_best_effort_timestamp(frame);
    }
#endif
    
    // Fallback: pkt_pts was removed in FFmpeg 4.0 (libavutil 56.x+)
    // Use frame->pts instead (which should work in all versions)
    if (pts == AV_NOPTS_VALUE) {
        pts = frame->pts;
    }
    
    // Fallback to frame pts (may be bogus with many codecs)
    if (pts == AV_NOPTS_VALUE) {
        pts = frame->pts;
    }
    
    // Last resort: packet DTS
    if (pts == AV_NOPTS_VALUE) {
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

    // =========================================================================
    // ASYNC DECODE PATH (mpv-style)
    // Use async queue if available - provides pre-buffered frames for smooth playback
    // =========================================================================
    if (useAsyncDecode_ && asyncDecodeQueue_) {
        // Set target frame so decode thread knows where we are
        asyncDecodeQueue_->setTargetFrame(frameNumber);
        
        // Try to get frame from queue (wait up to 5ms if not ready)
        AVFrame* queuedFrame = asyncDecodeQueue_->getFrame(frameNumber, 5);
        
        if (queuedFrame) {
            // Got frame from queue - transfer to GPU texture
            // Note: vaSyncSurface happens here, but the actual decode already completed in background
            bool success = transferHardwareFrameToGPU(queuedFrame, textureBuffer);
            if (success) {
                return true;
            } else {
                LOG_WARNING << "Async decode: GPU transfer failed for frame " << frameNumber;
            }
        } else {
            // Frame not ready - this shouldn't happen often if queue is working
            static int missCount = 0;
            if (++missCount % 30 == 1) {  // Log every 30 misses
                LOG_WARNING << "Async decode: frame " << frameNumber << " not in queue (oldest=" 
                           << asyncDecodeQueue_->getOldestFrame() << ", newest=" 
                           << asyncDecodeQueue_->getNewestFrame() << ")";
            }
            // Fall through to synchronous path as backup
        }
    }
    
    // =========================================================================
    // SYNCHRONOUS DECODE PATH (fallback)
    // Used when async queue is not available or frame was missed
    // =========================================================================

    // Seek to frame if needed
    // Only seek if this frame is far from the last decoded position
    // For consecutive frames, just decode forward
    static int64_t lastDecodedFrame = -1;
    
    // QUICK WIN #2: Early return for same frame (xjadeo-style)
    // If same frame is requested, GPU texture already has correct data
    // This avoids re-decoding when the caller requests the same frame multiple times
    if (lastDecodedFrame == frameNumber && textureBuffer.isValid()) {
        // Same frame already decoded and texture is valid - nothing to do
        return true;
    }
    
    bool needSeek = (lastDecodedFrame < 0 ||  // Initial state - must seek
                     frameNumber < lastDecodedFrame ||  // Backward seek
                     frameNumber > lastDecodedFrame + 30);  // Large forward jump
    
    if (needSeek) {
        // SPECIAL CASE: Frames before first keyframe
        // If the first keyframe is at frame 30, frames 0-29 exist before it as P/B frames
        // The index seekpts for these frames points to the keyframe (wrong!)
        // Instead, seek to beginning of file and decode forward
        bool isBeforeFirstKeyframe = false;
        if (frameIndex_ && frameCount_ > 0) {
            // Find first keyframe
            int64_t firstKeyframePTS = -1;
            for (int64_t i = 0; i < frameCount_; i++) {
                if (frameIndex_[i].key) {
                    firstKeyframePTS = frameIndex_[i].pkt_pts;
                    break;
                }
            }
            
            // Check if requested frame is before first keyframe
            if (firstKeyframePTS > 0 && frameIndex_[frameNumber].pkt_pts < firstKeyframePTS) {
                isBeforeFirstKeyframe = true;
            }
        }
        
        if (isBeforeFirstKeyframe) {
            // Seek to beginning of file (timestamp 0)
            if (!mediaReader_.seek(0, videoStream_, AVSEEK_FLAG_BACKWARD)) {
                LOG_WARNING << "Failed to seek to beginning for frame " << frameNumber;
                return false;
            }
        } else {
            // Use indexed seek (if available) for frame-accurate positioning
            // The seek() function uses frameIndex_ to find the exact packet position for this frame
            if (!seek(frameNumber)) {
                LOG_WARNING << "Failed to seek to frame " << frameNumber;
                return false;
            }
        }
        
        // Flush hardware decoder buffers ONLY after a real seek
        // This ensures we start fresh after a backward seek or large jump
        if (codecCtx_) {
            avcodec_flush_buffers(codecCtx_);
        }
    }

    // Decode frame using hardware decoder
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return false;
    }

    // Hardware decoders (especially h264_cuvid) need multiple packets before producing output
    // due to B-frame reordering and internal buffering. mpv uses up to 32 packets during probing.
    // We use a packet count bailout, not an iteration bailout.
    const int MAX_PACKETS = 128;  // Maximum packets to send before giving up
    int packetsSent = 0;
    int errorCount = 0;
    const int MAX_ERRORS = 10;
    bool frameFinished = false;
    
    // CRITICAL: Calculate target PTS for the frame we want
    // We need to decode forward until we reach this PTS
    AVRational timeBase = formatCtx_->streams[videoStream_]->time_base;
    int64_t targetPTS = av_rescale_q(frameNumber, frameRateQ_, timeBase);

    while (packetsSent < MAX_PACKETS && errorCount < MAX_ERRORS && !frameFinished) {
        // Try to receive a frame first (in case decoder has buffered frames)
        int err = avcodec_receive_frame(codecCtx_, hwFrame_);
        if (err == 0) {
            int64_t framePTS = hwFrame_->best_effort_timestamp != AV_NOPTS_VALUE 
                               ? hwFrame_->best_effort_timestamp 
                               : hwFrame_->pts;
            
            // Check if this is the frame we want (within tolerance for floating point frame rates)
            int64_t ptsDiff = framePTS - targetPTS;
            int64_t ptsPerFrame = av_rescale_q(1, frameRateQ_, timeBase);
            
            if (ptsDiff >= 0 && ptsDiff < ptsPerFrame) {
                // This is the correct frame (or close enough)
                frameFinished = true;
                break;
            } else if (framePTS < targetPTS) {
                // This frame is BEFORE the target - discard it and continue decoding
                av_frame_unref(hwFrame_);
                // Continue to next frame
            } else {
                // framePTS > targetPTS + ptsPerFrame - we've gone too far!
                // If we're WAY too far (more than 5 frames), something is wrong with the seek
                // Just use this frame as best effort (better than showing nothing)
                int64_t framesAhead = ptsDiff / ptsPerFrame;
                if (framesAhead > 5) {
                    LOG_ERROR << "Decoded frame PTS " << framePTS << " is " << framesAhead 
                             << " frames ahead of target " << targetPTS << " - seek may have failed";
                }
                frameFinished = true;
                break;
            }
        } else if (err == AVERROR(EAGAIN)) {
            // Decoder needs more input - this is normal, not an error
            // Continue to send more packets
        } else if (err == AVERROR_EOF) {
            // Decoder flushed, no more frames
            break;
        } else {
            // Actual error
            ++errorCount;
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(err, errbuf, AV_ERROR_MAX_STRING_SIZE);
            LOG_VERBOSE << "Hardware decoder receive error: " << errbuf;
            continue;
        }

        // Need more input - read and send a packet
        av_packet_unref(packet);
        err = mediaReader_.readPacket(packet);
        if (err < 0) {
            if (err == AVERROR_EOF) {
                // End of file - try to flush decoder
                err = avcodec_send_packet(codecCtx_, nullptr); // nullptr flushes
                if (err < 0 && err != AVERROR_EOF && err != AVERROR(EAGAIN)) {
                    ++errorCount;
                }
                // After flush, try to receive remaining frames
                continue;
            } else {
                // Read error
                av_packet_free(&packet);
                return false;
            }
        }

        // Skip non-video packets
        if (packet->stream_index != videoStream_) {
            continue;
        }

        // Send packet to hardware decoder
        err = avcodec_send_packet(codecCtx_, packet);
        if (err == 0) {
            // Successfully sent packet
            ++packetsSent;
        } else if (err == AVERROR(EAGAIN)) {
            // Decoder input buffer full - this shouldn't happen if we're receiving frames properly
            // Try to receive a frame and retry
            continue;
        } else if (err == AVERROR_EOF) {
            // Decoder already flushed
            break;
        } else {
            // Send error
            ++errorCount;
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(err, errbuf, AV_ERROR_MAX_STRING_SIZE);
            LOG_VERBOSE << "Hardware decoder send error: " << errbuf;
        }
    }
    
    if (!frameFinished) {
        LOG_VERBOSE << "Hardware decode: no frame after " << packetsSent << " packets";
    }

    if (!frameFinished) {
        av_packet_free(&packet);
        return false;
    }

    av_packet_free(&packet);

    // Update last decoded position for next call (use ACTUAL decoded PTS, not requested frame)
    int64_t decodedFrameNum = av_rescale_q(
        hwFrame_->best_effort_timestamp != AV_NOPTS_VALUE ? hwFrame_->best_effort_timestamp : hwFrame_->pts,
        timeBase, frameRateQ_
    );
    lastDecodedFrame = decodedFrameNum;

    // Lazy initialization of VaapiInterop (needs GL context which may not be available at open time)
#ifdef HAVE_VAAPI_INTEROP
    if (!vaapiInterop_ && displayBackend_ && displayBackend_->hasVaapiSupport() && 
        hwDecoderType_ == HardwareDecoder::Type::VAAPI) {
        vaapiInterop_ = std::make_unique<VaapiInterop>();
        if (!vaapiInterop_->init(displayBackend_)) {
            LOG_WARNING << "Failed to initialize per-instance VaapiInterop, falling back to CPU copy";
            vaapiInterop_.reset();
        }
    }
#endif

    // Transfer hardware frame to GPU texture
    // MPV-style: VaapiInterop will ref the frame, and we'll unref our copy immediately after transfer
    bool success = transferHardwareFrameToGPU(hwFrame_, textureBuffer);
    
    // CRITICAL: Unref hwFrame_ immediately after transfer (MPV pattern)
    // VaapiInterop has its own reference (currentFrame_) which it will release after rendering
    // If we don't unref here, we hold 2 references to the surface (ours + VaapiInterop's)
    // This exhausts the VAAPI surface pool and causes the 30-frame freeze!
    av_frame_unref(hwFrame_);
    
    return success;
}

bool VideoFileInput::transferHardwareFrameToGPU(AVFrame* hwFrame, GPUTextureFrameBuffer& textureBuffer) {
    if (!hwFrame) {
        LOG_WARNING << "transferHardwareFrameToGPU: hwFrame is NULL";
        return false;
    }
    
    // For hardware frames (VAAPI, etc.), data[0] may be NULL
    // VAAPI stores VASurfaceID in data[3], not data[0]
    // Check format to determine if it's a valid hardware frame
    bool isHwFrame = (hwFrame->format == AV_PIX_FMT_VAAPI ||
                      hwFrame->format == AV_PIX_FMT_CUDA ||
                      hwFrame->format == AV_PIX_FMT_QSV ||
                      hwFrame->format == AV_PIX_FMT_VIDEOTOOLBOX ||
                      hwFrame->format == AV_PIX_FMT_DXVA2_VLD);
    
    if (!isHwFrame && !hwFrame->data[0]) {
        LOG_WARNING << "transferHardwareFrameToGPU: frame has no data";
        return false;
    }

#ifdef HAVE_VAAPI_INTEROP
    // VAAPI ZERO-COPY PATH: Use shared VADisplay for direct GPU-to-GPU transfer
    // Two-phase import:
    //   Phase 1: createEGLImages - can be done from any thread
    //   Phase 2: bindTexturesToImages - must be done from GL thread
    if (hwFrame->format == AV_PIX_FMT_VAAPI && vaapiInterop_ && vaapiInterop_->isAvailable()) {
        GLuint texY = 0, texUV = 0;
        int width = 0, height = 0;
        
        // Phase 1: Create EGL images (works on any thread)
        if (vaapiInterop_->createEGLImages(hwFrame, width, height)) {
            // Phase 2: Try to bind textures (needs GL context)
            // If we're on the GL thread, this will succeed
            // If not, the caller can call bindTexturesToImages later
            if (vaapiInterop_->bindTexturesToImages(texY, texUV)) {
                // Set up the texture buffer with the imported textures
                if (!textureBuffer.setExternalNV12Textures(texY, texUV, frameInfo_)) {
                    LOG_WARNING << "transferHardwareFrameToGPU: Failed to set external NV12 textures";
                    vaapiInterop_->releaseFrame();
                    // Fall through to CPU path
                } else {
                    return true;
                }
            } else {
                // Texture binding failed - release EGL images and fall back to CPU path
                vaapiInterop_->releaseFrame();
            }
        } else {
            LOG_VERBOSE << "transferHardwareFrameToGPU: VAAPI zero-copy failed, falling back to CPU path";
        }
    }
#endif

    // Check if this is actually a hardware frame (has hardware format)
    // Cuvid decoders can output frames in different formats - some may be CPU frames already
    bool isHardwareFrame = (hwFrame->format == AV_PIX_FMT_CUDA || 
                           hwFrame->format == AV_PIX_FMT_VAAPI ||
                           hwFrame->format == AV_PIX_FMT_QSV ||
                           hwFrame->format == AV_PIX_FMT_VIDEOTOOLBOX ||
                           hwFrame->format == AV_PIX_FMT_DXVA2_VLD ||
                           hwFrame->hw_frames_ctx != nullptr);

    // Allocate CPU frame
    if (!frame_) {
        frame_ = av_frame_alloc();
        if (!frame_) {
            return false;
        }
    }

    // Determine which frame to use for conversion
    AVFrame* sourceFrame = hwFrame;  // Use hwFrame directly if it's already on CPU
    
    if (isHardwareFrame) {
        // Hardware frames need to be transferred from GPU to CPU first
        
        // Allocate CPU frame if needed
        if (!frame_) {
            frame_ = av_frame_alloc();
            if (!frame_) {
                return false;
            }
        }
        
        // Download from hardware frame to CPU frame
        int ret = av_hwframe_transfer_data(frame_, hwFrame, 0);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
            LOG_WARNING << "Failed to transfer hardware frame to CPU: " << errbuf;
            return false;
        }
        sourceFrame = frame_;
    }
    // else: h264_cuvid outputs directly to CPU memory (e.g., NV12) - use hwFrame directly

    AVPixelFormat srcFormat = static_cast<AVPixelFormat>(sourceFrame->format);
    
    // OPTIMIZATION: If source is NV12, upload directly without sws_scale conversion
    // The shader will do YUV→RGB conversion on the GPU (much faster than CPU sws_scale)
    if (srcFormat == AV_PIX_FMT_NV12) {
        // Allocate NV12 multi-plane texture if needed
        if (!textureBuffer.isValid() || 
            textureBuffer.getPlaneType() != TexturePlaneType::YUV_NV12 ||
            textureBuffer.info().width != frameInfo_.width || 
            textureBuffer.info().height != frameInfo_.height) {
            if (!textureBuffer.allocateMultiPlane(frameInfo_, TexturePlaneType::YUV_NV12)) {
                LOG_WARNING << "transferHardwareFrameToGPU: Failed to allocate NV12 texture";
                return false;
            }
            LOG_INFO << "transferHardwareFrameToGPU: Allocated NV12 multi-plane texture "
                    << frameInfo_.width << "x" << frameInfo_.height;
        }
        
        // Upload Y and UV planes directly
        // NV12: data[0] = Y plane, data[1] = interleaved UV plane
        if (!textureBuffer.uploadMultiPlaneData(
                sourceFrame->data[0],  // Y plane
                sourceFrame->data[1],  // UV plane (interleaved)
                nullptr,               // No V plane for NV12
                sourceFrame->linesize[0],  // Y stride
                sourceFrame->linesize[1],  // UV stride
                0)) {                      // No V stride
            LOG_WARNING << "transferHardwareFrameToGPU: Failed to upload NV12 data";
            return false;
        }
        
        return true;
    }
    
    // OPTIMIZATION: If source is YUV420P, upload directly without sws_scale conversion
    if (srcFormat == AV_PIX_FMT_YUV420P) {
        // Allocate YUV420P multi-plane texture if needed
        if (!textureBuffer.isValid() || 
            textureBuffer.getPlaneType() != TexturePlaneType::YUV_420P ||
            textureBuffer.info().width != frameInfo_.width || 
            textureBuffer.info().height != frameInfo_.height) {
            if (!textureBuffer.allocateMultiPlane(frameInfo_, TexturePlaneType::YUV_420P)) {
                LOG_WARNING << "transferHardwareFrameToGPU: Failed to allocate YUV420P texture";
                return false;
            }
            LOG_INFO << "transferHardwareFrameToGPU: Allocated YUV420P multi-plane texture "
                    << frameInfo_.width << "x" << frameInfo_.height;
        }
        
        // Upload Y, U, V planes directly
        if (!textureBuffer.uploadMultiPlaneData(
                sourceFrame->data[0],  // Y plane
                sourceFrame->data[1],  // U plane
                sourceFrame->data[2],  // V plane
                sourceFrame->linesize[0],  // Y stride
                sourceFrame->linesize[1],  // U stride
                sourceFrame->linesize[2])) { // V stride
            LOG_WARNING << "transferHardwareFrameToGPU: Failed to upload YUV420P data";
            return false;
        }
        
        return true;
    }

    // FALLBACK: For other formats, use sws_scale to convert to RGBA
    // This is slower but ensures compatibility with all pixel formats
    
    // Allocate RGBA texture buffer if needed
    if (!textureBuffer.isValid() || 
        textureBuffer.getPlaneType() != TexturePlaneType::SINGLE ||
        textureBuffer.info().width != frameInfo_.width || 
        textureBuffer.info().height != frameInfo_.height) {
        GLenum textureFormat = GL_RGBA;
        if (!textureBuffer.allocate(frameInfo_, textureFormat, false)) {
            return false;
        }
    }

    // Initialize sws context if needed
    if (!swsCtx_ || swsCtxWidth_ != sourceFrame->width || swsCtxHeight_ != sourceFrame->height ||
        swsCtxFormat_ != srcFormat) {
        if (swsCtx_) {
            sws_freeContext(swsCtx_);
            swsCtx_ = nullptr;
        }
        
        // Use SWS_BILINEAR for better real-time performance
        swsCtx_ = sws_getContext(
            sourceFrame->width, sourceFrame->height, srcFormat,
            frameInfo_.width, frameInfo_.height, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        if (!swsCtx_) {
            return false;
        }
        swsCtxWidth_ = sourceFrame->width;
        swsCtxHeight_ = sourceFrame->height;
        swsCtxFormat_ = srcFormat;
        LOG_WARNING << "transferHardwareFrameToGPU: Using sws_scale fallback for format " 
                   << av_get_pix_fmt_name(srcFormat) << " (slower path)";
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
              (const uint8_t* const*)sourceFrame->data, sourceFrame->linesize,
              0, sourceFrame->height,
              dstData, dstLinesize);
    
    if (result <= 0) {
        LOG_WARNING << "transferHardwareFrameToGPU: sws_scale failed, result=" << result;
        return false;
    }

    // Upload RGBA data to GPU texture
    if (!textureBuffer.uploadUncompressedData(rgbaBuffer.data(), rgbaSize,
                                             frameInfo_.width, frameInfo_.height,
                                             GL_RGBA)) {
        return false;
    }

    return true;
}

#ifdef HAVE_VAAPI_INTEROP
void VideoFileInput::setDisplayBackend(DisplayBackend* displayBackend) {
    displayBackend_ = displayBackend;
    // VaapiInterop will be created lazily when needed (requires GL context)
}

bool VideoFileInput::hasVaapiZeroCopy() const {
    return vaapiInterop_ != nullptr && vaapiInterop_->isAvailable() &&
           hwDecoderType_ == HardwareDecoder::Type::VAAPI;
}
#endif

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
    swsCtxFormat_ = AV_PIX_FMT_NONE;

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

#ifdef HAVE_VAAPI_INTEROP
    // Clean up per-instance VaapiInterop
    vaapiInterop_.reset();
#endif

    videoStream_ = -1;
    lastDecodedPTS_ = -1;
    lastDecodedFrameNo_ = -1;
    scanComplete_ = false;
    currentFrame_ = -1;
    useHardwareDecoding_ = false;
    hwDecoderType_ = HardwareDecoder::Type::NONE;
}

// ============================================================================
// Async Frame Pre-buffering (like mpv's decode-ahead)
// ============================================================================

void VideoFileInput::stopAsyncDecode() {
    if (decodeThread_) {
        decodeThreadStop_ = true;
        decodeCond_.notify_all();
        if (decodeThread_->joinable()) {
            decodeThread_->join();
        }
        decodeThread_.reset();
        decodeThreadRunning_ = false;
        decodeThreadStop_ = false;
    }
}

void VideoFileInput::startAsyncDecode(int64_t startFrame) {
    // DISABLED: Async decode has race conditions with shared FFmpeg objects
    // The decode thread and main thread share frame_, codecCtx_, swsCtx_, etc.
    // without proper synchronization, causing corrupted frames.
    // 
    // TODO: SMOOTHNESS FIX #2 - Implement proper async decode like mpv
    // The fix requires:
    //   1. Create separate AVCodecContext for decode thread (or use mutex)
    //   2. Separate AVFrame and SwsContext per thread
    //   3. Producer-consumer queue: decode thread produces, render thread consumes
    //   4. Pre-buffer 2-4 frames ahead of current playback position
    // 
    // mpv's approach in video/decode/vd_lavc.c:
    //   - Uses mp_dispatch for thread-safe decode requests
    //   - Maintains internal frame queue in filters/f_decoder_wrapper.c
    //   - Decodes ahead based on display timing requirements
    (void)startFrame;
    return;
    
#if 0  // Original implementation - disabled due to race conditions
    // Only use async decode for software decoding (hardware decode is already fast)
    if (useHardwareDecoding_) {
        return;
    }
    
    // Start decode thread if not running
    if (!decodeThread_) {
        decodeThreadStop_ = false;
        decodeThreadRunning_ = true;
        decodeTargetFrame_ = startFrame + 1;  // Start decoding next frame
        decodeThread_ = std::make_unique<std::thread>(&VideoFileInput::decodeThreadFunc, this);
    } else {
        // Update target and wake thread
        decodeTargetFrame_ = startFrame + 1;
        decodeCond_.notify_one();
    }
#endif
}

VideoFileInput::CachedFrame* VideoFileInput::findCachedFrame(int64_t frameNumber) {
    for (auto& cf : frameCache_) {
        if (cf.frameNumber == frameNumber && cf.valid) {
            return &cf;
        }
    }
    return nullptr;
}

void VideoFileInput::decodeThreadFunc() {
    LOG_INFO << "Async decode thread started";
    
    while (!decodeThreadStop_) {
        int64_t targetFrame = decodeTargetFrame_.load();
        
        // Check if we should decode this frame
        bool shouldDecode = false;
        {
            std::lock_guard<std::mutex> lock(cacheMutex_);
            // Only decode if frame not already cached and cache not full
            if (frameCache_.size() < FRAME_CACHE_SIZE && !findCachedFrame(targetFrame)) {
                shouldDecode = true;
            }
        }
        
        if (shouldDecode && targetFrame >= 0 && targetFrame < frameCount_) {
            // Decode the frame
            CachedFrame cf;
            cf.frameNumber = targetFrame;
            cf.valid = false;
            
            if (decodeFrameInternal(targetFrame, cf.buffer)) {
                cf.valid = true;
                
                // Add to cache
                std::lock_guard<std::mutex> lock(cacheMutex_);
                // Remove old frames if cache is full
                while (frameCache_.size() >= FRAME_CACHE_SIZE) {
                    frameCache_.pop_front();
                }
                frameCache_.push_back(std::move(cf));
                
                // Move to next frame
                decodeTargetFrame_ = targetFrame + 1;
            } else {
                // Decode failed, wait a bit and try again
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } else {
            // Nothing to decode, wait for signal
            std::unique_lock<std::mutex> lock(cacheMutex_);
            decodeCond_.wait_for(lock, std::chrono::milliseconds(10), [this] {
                return decodeThreadStop_.load();
            });
        }
    }
    
    LOG_INFO << "Async decode thread stopped";
}

bool VideoFileInput::decodeFrameInternal(int64_t frameNumber, FrameBuffer& buffer) {
    // This is a copy of the decoding logic from readFrame, but without cache check
    // Used by the async decode thread
    
    if (!isReady()) {
        return false;
    }

    // Check if we need to seek
    bool needSeek = false;
    if (currentFrame_ < 0 || currentFrame_ != frameNumber) {
        if (currentFrame_ >= 0 && frameNumber == currentFrame_ + 1) {
            needSeek = false;  // Sequential
        } else {
            needSeek = true;
        }
    }
    
    if (needSeek) {
        if (!seek(frameNumber)) {
            return false;
        }
    }

    // Decode frame
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return false;
    }

    // Get target timestamp
    int64_t targetTimestamp = -1;
    if (frameIndex_ && frameNumber >= 0 && frameNumber < frameCount_) {
        if (frameIndex_[frameNumber].frame_pts >= 0) {
            targetTimestamp = frameIndex_[frameNumber].frame_pts;
        } else {
            targetTimestamp = frameIndex_[frameNumber].timestamp;
        }
    }

    int64_t oneFrame = 1;
    if (frameIndex_ && frameNumber > 0 && frameNumber < frameCount_) {
        if (frameIndex_[frameNumber-1].timestamp >= 0 && 
            frameIndex_[frameNumber].timestamp >= 0) {
            oneFrame = frameIndex_[frameNumber].timestamp - frameIndex_[frameNumber-1].timestamp;
            if (oneFrame <= 0) oneFrame = 1;
        }
    }
    const int64_t prefuzz = oneFrame > 10 ? 1 : 0;
    
    int bailout = 64;
    bool frameFinished = false;

    while (bailout > 0 && !decodeThreadStop_) {
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

        err = videoDecoder_.sendPacket(packet);
        if (err < 0 && err != AVERROR(EAGAIN)) {
            --bailout;
            continue;
        }

        err = videoDecoder_.receiveFrame(frame_);
        bool gotFrame = (err == 0);
        if (err == AVERROR(EAGAIN)) {
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

        int64_t pts = parsePTSFromFrame(frame_);
        if (pts == AV_NOPTS_VALUE) {
            --bailout;
            continue;
        }

        lastDecodedPTS_ = pts;

        if (targetTimestamp >= 0) {
            if (pts + prefuzz >= targetTimestamp) {
                if (pts - targetTimestamp < oneFrame) {
                    frameFinished = true;
                    break;
                }
            }
        } else {
            frameFinished = true;
            break;
        }

        --bailout;
    }

    av_packet_free(&packet);

    if (!frameFinished) {
        return false;
    }

    // Allocate buffer if needed
    if (!buffer.isValid() || buffer.info().width != frameInfo_.width || 
        buffer.info().height != frameInfo_.height) {
        if (!buffer.allocate(frameInfo_)) {
            return false;
        }
    }

    // Convert frame format
    if (!swsCtx_ || swsCtxWidth_ != frameInfo_.width || swsCtxHeight_ != frameInfo_.height) {
        if (swsCtx_) {
            sws_freeContext(swsCtx_);
            swsCtx_ = nullptr;
        }
        
        swsCtx_ = sws_getContext(
            codecCtx_->width, codecCtx_->height, codecCtx_->pix_fmt,
            frameInfo_.width, frameInfo_.height, AV_PIX_FMT_BGRA,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        if (!swsCtx_) {
            return false;
        }
        swsCtxWidth_ = frameInfo_.width;
        swsCtxHeight_ = frameInfo_.height;
    }

    int bgraStride = frameInfo_.width * 4;
    uint8_t* dstData[4] = {buffer.data(), nullptr, nullptr, nullptr};
    int dstLinesize[4] = {bgraStride, 0, 0, 0};
    
    int result = sws_scale(swsCtx_,
              (const uint8_t* const*)frame_->data, frame_->linesize,
              0, codecCtx_->height,
              dstData, dstLinesize);
    
    if (result <= 0) {
        return false;
    }

    currentFrame_ = frameNumber;
    return true;
}

} // namespace videocomposer

