#include "HardwareDecoder.h"
#include "../utils/Logger.h"
#include <cstring>
#include <string>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

namespace videocomposer {

// Cache the detected hardware decoder type to avoid re-detection and duplicate logging
static HardwareDecoder::Type cached_hw_type = HardwareDecoder::Type::NONE;
static bool hw_type_detected = false;

HardwareDecoder::Type HardwareDecoder::detectAvailable() {
    // Return cached result if already detected
    if (hw_type_detected) {
        return cached_hw_type;
    }
    
    // Try to detect available hardware decoders
    // Check in order of preference: CUDA > QSV > VAAPI > VideoToolbox > DXVA2
    // QSV is checked before VAAPI for Intel GPUs (QSV is Intel-specific and often more efficient)
    
    AVBufferRef* hwDeviceCtx = nullptr;
    
    // Check CUDA (NVIDIA)
    #ifdef __linux__
    hwDeviceCtx = nullptr;
    int ret = av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
    if (ret >= 0 && hwDeviceCtx != nullptr) {
        LOG_INFO << "Hardware decoder detected: CUDA (NVIDIA)";
        av_buffer_unref(&hwDeviceCtx);
        cached_hw_type = Type::CUDA;
        hw_type_detected = true;
        return Type::CUDA;
    }
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        LOG_VERBOSE << "CUDA hardware decoder not available: " << errbuf << " (error code: " << ret << ")";
    }
    if (hwDeviceCtx) {
        av_buffer_unref(&hwDeviceCtx);
        hwDeviceCtx = nullptr;
    }
    #endif
    
    // Check QSV (Intel Quick Sync Video - Linux/Windows)
    #if defined(__linux__) || defined(_WIN32)
    hwDeviceCtx = nullptr;
    ret = av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_QSV, nullptr, nullptr, 0);
    if (ret >= 0 && hwDeviceCtx != nullptr) {
        LOG_INFO << "Hardware decoder detected: QSV (Intel Quick Sync Video)";
        av_buffer_unref(&hwDeviceCtx);
        cached_hw_type = Type::QSV;
        hw_type_detected = true;
        return Type::QSV;
    }
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        LOG_VERBOSE << "QSV hardware decoder not available: " << errbuf << " (error code: " << ret << ")";
    }
    if (hwDeviceCtx) {
        av_buffer_unref(&hwDeviceCtx);
        hwDeviceCtx = nullptr;
    }
    #endif
    
    // Check VAAPI (Linux, Intel/AMD)
    #ifdef __linux__
    hwDeviceCtx = nullptr;
    ret = av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
    if (ret >= 0 && hwDeviceCtx != nullptr) {
        LOG_INFO << "Hardware decoder detected: VAAPI (Intel/AMD)";
        av_buffer_unref(&hwDeviceCtx);
        cached_hw_type = Type::VAAPI;
        hw_type_detected = true;
        return Type::VAAPI;
    }
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        LOG_VERBOSE << "VAAPI hardware decoder not available: " << errbuf << " (error code: " << ret << ")";
    }
    if (hwDeviceCtx) {
        av_buffer_unref(&hwDeviceCtx);
        hwDeviceCtx = nullptr;
    }
    #endif
    
    // Check VideoToolbox (macOS)
    #ifdef __APPLE__
    hwDeviceCtx = nullptr;
    ret = av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0);
    if (ret >= 0 && hwDeviceCtx != nullptr) {
        LOG_INFO << "Hardware decoder detected: VideoToolbox (macOS)";
        av_buffer_unref(&hwDeviceCtx);
        cached_hw_type = Type::VIDEOTOOLBOX;
        hw_type_detected = true;
        return Type::VIDEOTOOLBOX;
    }
    if (hwDeviceCtx) {
        av_buffer_unref(&hwDeviceCtx);
        hwDeviceCtx = nullptr;
    }
    #endif
    
    // Check DXVA2 (Windows)
    #ifdef _WIN32
    hwDeviceCtx = nullptr;
    ret = av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_DXVA2, nullptr, nullptr, 0);
    if (ret >= 0 && hwDeviceCtx != nullptr) {
        LOG_INFO << "Hardware decoder detected: DXVA2 (Windows)";
        av_buffer_unref(&hwDeviceCtx);
        cached_hw_type = Type::DXVA2;
        hw_type_detected = true;
        return Type::DXVA2;
    }
    if (hwDeviceCtx) {
        av_buffer_unref(&hwDeviceCtx);
        hwDeviceCtx = nullptr;
    }
    #endif
    
    LOG_INFO << "No hardware decoder detected, will use software decoding";
    cached_hw_type = Type::NONE;
    hw_type_detected = true;
    return Type::NONE;
}

AVHWDeviceType HardwareDecoder::getFFmpegDeviceType(Type type) {
    switch (type) {
        case Type::QSV:
            return AV_HWDEVICE_TYPE_QSV;
        case Type::VAAPI:
            return AV_HWDEVICE_TYPE_VAAPI;
        case Type::CUDA:
            return AV_HWDEVICE_TYPE_CUDA;
        case Type::VIDEOTOOLBOX:
            return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
        case Type::DXVA2:
            return AV_HWDEVICE_TYPE_DXVA2;
        default:
            return AV_HWDEVICE_TYPE_NONE;
    }
}

AVPixelFormat HardwareDecoder::getHardwarePixelFormat(Type type, AVCodecID codecId) {
    // Get hardware pixel format for the given decoder type and codec
    // This depends on what the hardware decoder outputs
    
    switch (type) {
        case Type::QSV:
            // QSV outputs QSV frames
            return AV_PIX_FMT_QSV;
        case Type::VAAPI:
            // VAAPI typically outputs NV12 or YUV420P
            return AV_PIX_FMT_VAAPI;
        case Type::CUDA:
            // CUDA/NVDEC outputs CUDA frames
            return AV_PIX_FMT_CUDA;
        case Type::VIDEOTOOLBOX:
            // VideoToolbox outputs VideoToolbox frames
            return AV_PIX_FMT_VIDEOTOOLBOX;
        case Type::DXVA2:
            // DXVA2 outputs DXVA2 frames
            return AV_PIX_FMT_DXVA2_VLD;
        default:
            return AV_PIX_FMT_NONE;
    }
}

bool HardwareDecoder::isAvailableForCodec(AVCodecID codecId) {
    // Check if hardware decoder is available for the given codec
    Type hwType = detectAvailable();
    return isAvailableForCodec(codecId, hwType);
}

bool HardwareDecoder::isAvailableForCodec(AVCodecID codecId, Type hwType) {
    // Check if hardware decoder is available for the given codec
    if (hwType == Type::NONE) {
        return false;
    }
    
    // Check if the codec supports hardware decoding
    // This works for ALL codecs - if a hardware decoder exists in FFmpeg, it will be found
    // Examples: h264_qsv, hevc_vaapi, vp9_cuda, av1_qsv, mpeg2_vaapi, etc.
    // When FFmpeg is updated with new hardware decoders, they are automatically discovered
    
    // FFmpeg 4.0+ (58.x) changed return type to const AVCodec*
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 0, 0)
    const AVCodec* codec = avcodec_find_decoder(codecId);
#else
    AVCodec* codec = avcodec_find_decoder(codecId);
#endif
    if (!codec) {
        return false;
    }
    
    // Check if hardware pixel format is available for this codec
    AVPixelFormat hwPixFmt = getHardwarePixelFormat(hwType, codecId);
    if (hwPixFmt == AV_PIX_FMT_NONE) {
        return false;
    }
    
    // Try to find hardware decoder for this codec
    // FFmpeg hardware decoders are typically named like "h264_vaapi", "h264_qsv", "hevc_cuda", etc.
    // This uses avcodec_find_decoder_by_name() which dynamically queries FFmpeg at runtime,
    // so it will automatically discover new hardware decoders when FFmpeg is updated.
    std::string codecName = avcodec_get_name(codecId);
    std::string hwCodecName;
    
    switch (hwType) {
        case Type::QSV:
            hwCodecName = codecName + "_qsv";
            break;
        case Type::VAAPI:
            hwCodecName = codecName + "_vaapi";
            break;
        case Type::CUDA:
            // FFmpeg uses "cuvid" suffix for NVIDIA CUDA decoders (e.g., h264_cuvid, hevc_cuvid)
            hwCodecName = codecName + "_cuvid";
            break;
        case Type::VIDEOTOOLBOX:
            // VideoToolbox uses the same codec name
            hwCodecName = codecName;
            break;
        case Type::DXVA2:
            hwCodecName = codecName + "_dxva2";
            break;
        default:
            return false;
    }
    
    // Dynamically query FFmpeg for the hardware decoder
    // This will automatically discover new decoders when FFmpeg is updated
    // FFmpeg 4.0+ (58.x) changed return type to const AVCodec*
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 0, 0)
    const AVCodec* hwCodec = avcodec_find_decoder_by_name(hwCodecName.c_str());
#else
    AVCodec* hwCodec = avcodec_find_decoder_by_name(hwCodecName.c_str());
#endif
    bool available = hwCodec != nullptr;
    
    if (available) {
        LOG_VERBOSE << "Hardware decoder available for codec: " << codecName 
                    << " using " << getName(hwType);
    } else {
        // Only log at INFO level if hardware decoder type was detected but codec not supported
        // This avoids duplicate messages when no hardware decoder is available at all
        if (hwType != Type::NONE) {
            LOG_INFO << "Hardware decoder not available for codec: " << codecName 
                     << " (will use software decoding)";
        } else {
            LOG_VERBOSE << "Hardware decoder NOT available for codec: " << codecName 
                        << " (will use software decoding)";
        }
    }
    
    return available;
}

const char* HardwareDecoder::getName(Type type) {
    switch (type) {
        case Type::QSV:
            return "QSV";
        case Type::VAAPI:
            return "VAAPI";
        case Type::CUDA:
            return "CUDA";
        case Type::VIDEOTOOLBOX:
            return "VideoToolbox";
        case Type::DXVA2:
            return "DXVA2";
        default:
            return "None";
    }
}

} // namespace videocomposer

