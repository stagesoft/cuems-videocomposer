#include "HardwareDecoder.h"
#include "../utils/Logger.h"
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include <sstream>
#include <cstdio>

#ifdef __linux__
#ifdef HAVE_VAAPI_INTEROP
#include <va/va.h>
#include <va/va_drm.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef HAVE_X11
#include <X11/Xlib.h>
#include <va/va_x11.h>
#endif
#endif
#endif

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/hwcontext_vaapi.h>
#include <stdarg.h>
}

namespace videocomposer {

// Cache the detected hardware decoder type to avoid re-detection and duplicate logging
static HardwareDecoder::Type cached_hw_type = HardwareDecoder::Type::NONE;
static bool hw_type_detected = false;

// FFmpeg log callback state for suppressing messages during probing
static std::vector<std::string> captured_ffmpeg_messages;
static std::vector<std::string> captured_libva_errors;
static bool is_probing_hardware = false;

// Custom FFmpeg log callback that filters out VAAPI errors during probing
static void ffmpeg_log_callback(void* ptr, int level, const char* fmt, va_list vl) {
    // Only capture during hardware decoder probing
    if (!is_probing_hardware) {
        // Use default FFmpeg behavior when not probing
        av_log_default_callback(ptr, level, fmt, vl);
        return;
    }
    
    // Format the message
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, vl);
    std::string message(buffer);
    
    // Filter out VAAPI-related errors during probing (these are normal when probing)
    // Check for common VAAPI error patterns
    bool is_vaapi_error = (
        message.find("VAAPI") != std::string::npos ||
        message.find("vaGetDriverNameByIndex") != std::string::npos ||
        message.find("Failed to initialise VAAPI connection") != std::string::npos ||
        message.find("AVHWDeviceContext") != std::string::npos
    );
    
    // Only capture non-VAAPI errors or if level is ERROR or higher
    // VAAPI errors during probing are expected and should be suppressed
    if (!is_vaapi_error || level <= AV_LOG_ERROR) {
        captured_ffmpeg_messages.push_back(message);
    }
    // Otherwise, suppress the message (don't call original callback)
}

#ifdef __linux__
#ifdef HAVE_VAAPI_INTEROP
// Helper function to create VADisplay with callbacks (like mpv does)
// This allows us to suppress libva stderr messages during detection
// Returns: VADisplay, and sets nativeDisplay/type for cleanup
struct VADisplayInfo {
    VADisplay display;
    void* nativeDisplay;  // X11 Display* or nullptr
    int drmFd;            // DRM fd or -1
    bool isX11;
};

static VADisplayInfo create_va_display_with_callbacks() {
    VADisplayInfo info = {nullptr, nullptr, -1, false};
    
    // Try X11 first (most common)
    #ifdef HAVE_X11
    Display* xDisplay = XOpenDisplay(nullptr);
    if (xDisplay) {
        VADisplay vaDisplay = vaGetDisplay(xDisplay);
        if (vaDisplay) {
            // Set up libva callbacks to capture errors (will only log if no decoder found)
            vaSetErrorCallback(vaDisplay, [](void* context, const char* msg) {
                // Capture libva errors during hardware decoder detection
                // They're expected when probing - will only be logged if no decoder succeeds
                if (is_probing_hardware) {
                    std::string error_msg = std::string("libva: ") + msg;
                    // Remove trailing newline if present
                    if (!error_msg.empty() && error_msg.back() == '\n') {
                        error_msg.pop_back();
                    }
                    captured_libva_errors.push_back(error_msg);
                }
            }, nullptr);
            
            vaSetInfoCallback(vaDisplay, [](void* context, const char* msg) {
                // Suppress libva info messages during detection
            }, nullptr);
            
            info.display = vaDisplay;
            info.nativeDisplay = xDisplay;
            info.isX11 = true;
            return info;
        } else {
            XCloseDisplay(xDisplay);
        }
    }
    #endif
    
    // Fallback to DRM if X11 failed
    int drmFd = open("/dev/dri/renderD128", O_RDWR);
    if (drmFd >= 0) {
        VADisplay vaDisplay = vaGetDisplayDRM(drmFd);
        if (vaDisplay) {
            // Set up libva callbacks to capture errors (will only log if no decoder found)
            vaSetErrorCallback(vaDisplay, [](void* context, const char* msg) {
                // Capture libva errors during hardware decoder detection
                // They're expected when probing - will only be logged if no decoder succeeds
                if (is_probing_hardware) {
                    std::string error_msg = std::string("libva: ") + msg;
                    // Remove trailing newline if present
                    if (!error_msg.empty() && error_msg.back() == '\n') {
                        error_msg.pop_back();
                    }
                    captured_libva_errors.push_back(error_msg);
                }
            }, nullptr);
            
            vaSetInfoCallback(vaDisplay, [](void* context, const char* msg) {
                // Suppress libva info messages during detection
            }, nullptr);
            
            info.display = vaDisplay;
            info.drmFd = drmFd;
            info.isX11 = false;
            return info;
        } else {
            close(drmFd);
        }
    }
    
    return info;
}

// Helper function to test VAAPI with our own VADisplay (like mpv does)
static bool test_vaapi_with_display(VADisplay vaDisplay) {
    if (!vaDisplay) {
        return false;
    }
    
    // Try to initialize VAAPI
    int major, minor;
    VAStatus status = vaInitialize(vaDisplay, &major, &minor);
    if (status != VA_STATUS_SUCCESS) {
        return false;
    }
    
    // Try to create hardware device context using our VADisplay
    AVBufferRef* hwDeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VAAPI);
    if (!hwDeviceCtx) {
        vaTerminate(vaDisplay);
        return false;
    }
    
    AVHWDeviceContext* hwctx = (AVHWDeviceContext*)hwDeviceCtx->data;
    AVVAAPIDeviceContext* vactx = (AVVAAPIDeviceContext*)hwctx->hwctx;
    vactx->display = vaDisplay;
    
    // Initialize the device context
    int ret = av_hwdevice_ctx_init(hwDeviceCtx);
    
    // Clean up
    av_buffer_unref(&hwDeviceCtx);
    vaTerminate(vaDisplay);
    
    return (ret >= 0);
}

// Clean up VADisplay and native resources
static void cleanup_va_display(VADisplayInfo& info) {
    if (info.display) {
        // VADisplay should already be terminated by test_vaapi_with_display
        // But we need to clean up native resources
        if (info.isX11 && info.nativeDisplay) {
            #ifdef HAVE_X11
            XCloseDisplay((Display*)info.nativeDisplay);
            #endif
        } else if (info.drmFd >= 0) {
            close(info.drmFd);
        }
    }
}
#endif
#endif

HardwareDecoder::Type HardwareDecoder::detectAvailable() {
    // Return cached result if already detected
    if (hw_type_detected) {
        return cached_hw_type;
    }
    
    // Set up FFmpeg log callback to suppress VAAPI errors during probing
    captured_ffmpeg_messages.clear();
    captured_libva_errors.clear();
    is_probing_hardware = true;
    av_log_set_callback(ffmpeg_log_callback);
    // Set log level to capture warnings and errors
    int original_log_level = av_log_get_level();
    av_log_set_level(AV_LOG_WARNING); // Capture warnings and errors
    
    // Try to detect available hardware decoders
    // Check in order of preference: VAAPI > CUDA > QSV > VideoToolbox > DXVA2
    // VAAPI is checked first because we have VaapiInterop for zero-copy GPU texture uploads
    
    AVBufferRef* hwDeviceCtx = nullptr;
    int ret = -1;
    Type foundType = Type::NONE;
    std::vector<std::pair<std::string, std::string>> failedProbes; // (name, error)
    
    // Check VAAPI first (Linux, Intel/AMD) - preferred for zero-copy with VaapiInterop
    // Use mpv's approach: create our own VADisplay with callbacks to suppress libva stderr messages
    #ifdef __linux__
    #ifdef HAVE_VAAPI_INTEROP
    VADisplayInfo vaInfo = create_va_display_with_callbacks();
    if (vaInfo.display) {
        if (test_vaapi_with_display(vaInfo.display)) {
            foundType = Type::VAAPI;
        } else {
            failedProbes.push_back(std::make_pair("VAAPI", "Failed to initialize VAAPI device context"));
        }
        // Clean up VADisplay and native resources
        cleanup_va_display(vaInfo);
    } else {
        failedProbes.push_back(std::make_pair("VAAPI", "Failed to create VADisplay"));
    }
    #else
    // Fallback: use FFmpeg's method if VAAPI interop not available
    hwDeviceCtx = nullptr;
    ret = av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
    if (ret >= 0 && hwDeviceCtx != nullptr) {
        foundType = Type::VAAPI;
        av_buffer_unref(&hwDeviceCtx);
        hwDeviceCtx = nullptr;
    } else {
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
            failedProbes.push_back(std::make_pair("VAAPI", std::string(errbuf)));
        }
        if (hwDeviceCtx) {
            av_buffer_unref(&hwDeviceCtx);
            hwDeviceCtx = nullptr;
        }
    }
    #endif
    #endif
    
    // Check CUDA second (NVIDIA)
    #ifdef __linux__
    if (foundType == Type::NONE) {
        hwDeviceCtx = nullptr;
        ret = av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
        if (ret >= 0 && hwDeviceCtx != nullptr) {
            foundType = Type::CUDA;
            av_buffer_unref(&hwDeviceCtx);
            hwDeviceCtx = nullptr;
        } else {
            if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
                failedProbes.push_back(std::make_pair("CUDA", std::string(errbuf)));
            }
            if (hwDeviceCtx) {
                av_buffer_unref(&hwDeviceCtx);
                hwDeviceCtx = nullptr;
            }
        }
    }
    #endif
    
    // Check QSV third (Intel Quick Sync Video - Linux/Windows)
    #if defined(__linux__) || defined(_WIN32)
    if (foundType == Type::NONE) {
        hwDeviceCtx = nullptr;
        ret = av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_QSV, nullptr, nullptr, 0);
        if (ret >= 0 && hwDeviceCtx != nullptr) {
            foundType = Type::QSV;
            av_buffer_unref(&hwDeviceCtx);
            hwDeviceCtx = nullptr;
        } else {
            if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
                failedProbes.push_back(std::make_pair("QSV", std::string(errbuf)));
            }
            if (hwDeviceCtx) {
                av_buffer_unref(&hwDeviceCtx);
                hwDeviceCtx = nullptr;
            }
        }
    }
    #endif
    
    // Check VideoToolbox (macOS)
    #ifdef __APPLE__
    if (foundType == Type::NONE) {
        hwDeviceCtx = nullptr;
        ret = av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0);
        if (ret >= 0 && hwDeviceCtx != nullptr) {
            foundType = Type::VIDEOTOOLBOX;
            av_buffer_unref(&hwDeviceCtx);
            hwDeviceCtx = nullptr;
        } else {
            if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
                failedProbes.push_back(std::make_pair("VideoToolbox", std::string(errbuf)));
            }
            if (hwDeviceCtx) {
                av_buffer_unref(&hwDeviceCtx);
                hwDeviceCtx = nullptr;
            }
        }
    }
    #endif
    
    // Check DXVA2 (Windows)
    #ifdef _WIN32
    if (foundType == Type::NONE) {
        hwDeviceCtx = nullptr;
        ret = av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_DXVA2, nullptr, nullptr, 0);
        if (ret >= 0 && hwDeviceCtx != nullptr) {
            foundType = Type::DXVA2;
            av_buffer_unref(&hwDeviceCtx);
            hwDeviceCtx = nullptr;
        } else {
            if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
                failedProbes.push_back(std::make_pair("DXVA2", std::string(errbuf)));
            }
            if (hwDeviceCtx) {
                av_buffer_unref(&hwDeviceCtx);
                hwDeviceCtx = nullptr;
            }
        }
    }
    #endif
    
    // Restore original FFmpeg log callback
    is_probing_hardware = false;
    av_log_set_callback(av_log_default_callback);
    av_log_set_level(original_log_level);
    
    // Log results based on whether we found a decoder
    if (foundType != Type::NONE) {
        // Success: log the found decoder, and only verbose log failed probes
        if (foundType == Type::VAAPI) {
            LOG_INFO << "Hardware decoder detected: VAAPI (Intel/AMD)";
        } else if (foundType == Type::CUDA) {
            LOG_INFO << "Hardware decoder detected: CUDA (NVIDIA)";
        } else if (foundType == Type::QSV) {
            LOG_INFO << "Hardware decoder detected: QSV (Intel Quick Sync Video)";
        } else if (foundType == Type::VIDEOTOOLBOX) {
            LOG_INFO << "Hardware decoder detected: VideoToolbox (macOS)";
        } else if (foundType == Type::DXVA2) {
            LOG_INFO << "Hardware decoder detected: DXVA2 (Windows)";
        }
        
        // Log failed probes as verbose only (normal when probing)
        for (const auto& probe : failedProbes) {
            LOG_VERBOSE << probe.first << " hardware decoder not available: " << probe.second 
                        << " (probing)";
        }
        
        // Clear captured FFmpeg and libva messages - we don't need them if a decoder succeeded
        captured_ffmpeg_messages.clear();
        captured_libva_errors.clear();
    } else {
        // No decoder found: log all failures as warnings (something might be wrong)
        LOG_WARNING << "No hardware decoder detected. Probe results:";
        for (const auto& probe : failedProbes) {
            LOG_WARNING << "  - " << probe.first << ": " << probe.second;
        }
        
        // Show captured FFmpeg messages only if no decoder was found
        if (!captured_ffmpeg_messages.empty()) {
            LOG_WARNING << "FFmpeg messages during hardware decoder detection:";
            for (const auto& msg : captured_ffmpeg_messages) {
                // Remove trailing newlines and log
                std::string clean_msg = msg;
                while (!clean_msg.empty() && (clean_msg.back() == '\n' || clean_msg.back() == '\r')) {
                    clean_msg.pop_back();
                }
                if (!clean_msg.empty()) {
                    LOG_WARNING << "  " << clean_msg;
                }
            }
        }
        
        // Show captured libva errors only if no decoder was found
        if (!captured_libva_errors.empty()) {
            LOG_WARNING << "libva errors during hardware decoder detection:";
            for (const auto& msg : captured_libva_errors) {
                LOG_WARNING << "  " << msg;
            }
        }
        
        LOG_INFO << "Will use software decoding";
    }
    
    cached_hw_type = foundType;
    hw_type_detected = true;
    return foundType;
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
    
    // Check hardware decoder availability
    // NOTE: Different hardware decoders work differently:
    // - QSV/CUVID: Have dedicated wrapper decoders (h264_qsv, h264_cuvid)
    // - VAAPI: Uses standard decoder with hw_device_ctx attached (hwaccel mechanism)
    // - VideoToolbox: Uses standard decoder with hw_device_ctx (like VAAPI)
    
    std::string codecName = avcodec_get_name(codecId);
    
    // For VAAPI and VideoToolbox, check if the standard decoder supports hwaccel
    // by verifying the hw_device_ctx method is available
    if (hwType == Type::VAAPI || hwType == Type::VIDEOTOOLBOX) {
        // Find the standard software decoder
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 0, 0)
        const AVCodec* decoder = avcodec_find_decoder(codecId);
#else
        AVCodec* decoder = avcodec_find_decoder(codecId);
#endif
        if (!decoder) {
            LOG_VERBOSE << "No decoder found for codec: " << codecName;
            return false;
        }
        
        // Check if this decoder supports hardware acceleration via hw_device_ctx
        // by looking for the appropriate pixel format in its hw_configs
        AVHWDeviceType targetDeviceType = getFFmpegDeviceType(hwType);
        bool supportsHwaccel = false;
        
        for (int i = 0; ; i++) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, i);
            if (!config) break;
            
            if (config->device_type == targetDeviceType &&
                (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                supportsHwaccel = true;
                break;
            }
        }
        
        if (supportsHwaccel) {
            LOG_VERBOSE << "Hardware acceleration available for codec: " << codecName 
                        << " using " << getName(hwType) << " (hwaccel method)";
        } else {
            LOG_INFO << "Hardware acceleration not available for codec: " << codecName 
                     << " via " << getName(hwType) << " (will use software decoding)";
        }
        return supportsHwaccel;
    }
    
    // For QSV, CUVID, DXVA2: check for dedicated wrapper decoders
    std::string hwCodecName;
    switch (hwType) {
        case Type::QSV:
            hwCodecName = codecName + "_qsv";
            break;
        case Type::CUDA:
            // FFmpeg uses "cuvid" suffix for NVIDIA CUDA decoders (e.g., h264_cuvid, hevc_cuvid)
            hwCodecName = codecName + "_cuvid";
            break;
        case Type::DXVA2:
            hwCodecName = codecName + "_dxva2";
            break;
        default:
            return false;
    }
    
    // Dynamically query FFmpeg for the hardware decoder
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
        if (hwType != Type::NONE) {
            LOG_INFO << "Hardware decoder " << hwCodecName << " not available for codec: " << codecName 
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

