#include "HardwareDecoder.h"
#include "../utils/Logger.h"
#include <cstring>
#include <string>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavcodec/avcodec.h>
}

namespace videocomposer {

HardwareDecoder::Type HardwareDecoder::detectAvailable() {
    // Try to detect available hardware decoders
    // Check in order of preference: CUDA > VAAPI > VideoToolbox > DXVA2
    
    AVBufferRef* hwDeviceCtx = nullptr;
    
    // Check CUDA (NVIDIA)
    #ifdef __linux__
    hwDeviceCtx = nullptr;
    int ret = av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
    if (ret >= 0 && hwDeviceCtx != nullptr) {
        LOG_INFO << "Hardware decoder detected: CUDA (NVIDIA)";
        av_buffer_unref(&hwDeviceCtx);
        return Type::CUDA;
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
        return Type::VAAPI;
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
        return Type::DXVA2;
    }
    if (hwDeviceCtx) {
        av_buffer_unref(&hwDeviceCtx);
        hwDeviceCtx = nullptr;
    }
    #endif
    
    LOG_INFO << "No hardware decoder detected, will use software decoding";
    return Type::NONE;
}

AVHWDeviceType HardwareDecoder::getFFmpegDeviceType(Type type) {
    switch (type) {
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
    if (hwType == Type::NONE) {
        return false;
    }
    
    // Check if the codec supports hardware decoding
    // Most hardware decoders support H.264 and HEVC
    // AV1 support varies by hardware
    
    AVCodec* codec = avcodec_find_decoder(codecId);
    if (!codec) {
        return false;
    }
    
    // Check if hardware pixel format is available for this codec
    AVPixelFormat hwPixFmt = getHardwarePixelFormat(hwType, codecId);
    if (hwPixFmt == AV_PIX_FMT_NONE) {
        return false;
    }
    
    // Try to find hardware decoder for this codec
    // FFmpeg hardware decoders are typically named like "h264_vaapi", "hevc_cuda", etc.
    std::string codecName = avcodec_get_name(codecId);
    std::string hwCodecName;
    
    switch (hwType) {
        case Type::VAAPI:
            hwCodecName = codecName + "_vaapi";
            break;
        case Type::CUDA:
            hwCodecName = codecName + "_cuda";
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
    
    AVCodec* hwCodec = avcodec_find_decoder_by_name(hwCodecName.c_str());
    bool available = hwCodec != nullptr;
    
    if (available) {
        LOG_VERBOSE << "Hardware decoder available for codec: " << codecName 
                    << " using " << getName(hwType);
    } else {
        LOG_VERBOSE << "Hardware decoder NOT available for codec: " << codecName 
                    << " (will use software decoding)";
    }
    
    return available;
}

const char* HardwareDecoder::getName(Type type) {
    switch (type) {
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

