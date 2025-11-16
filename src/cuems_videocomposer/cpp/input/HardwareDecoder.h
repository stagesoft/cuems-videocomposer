#ifndef VIDEOCOMPOSER_HARDWAREDECODER_H
#define VIDEOCOMPOSER_HARDWAREDECODER_H

#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

namespace videocomposer {

/**
 * HardwareDecoder - Utility for detecting and managing hardware video decoders
 * 
 * Supports:
 * - VAAPI (Linux, Intel/AMD GPUs)
 * - CUDA (NVIDIA GPUs)
 * - VideoToolbox (macOS)
 * - DXVA2 (Windows)
 */
class HardwareDecoder {
public:
    enum class Type {
        NONE,           // No hardware decoder available
        VAAPI,          // VAAPI (Linux)
        CUDA,           // CUDA/NVDEC (NVIDIA)
        VIDEOTOOLBOX,   // VideoToolbox (macOS)
        DXVA2           // DXVA2 (Windows)
    };

    /**
     * Detect available hardware decoder type
     * @return Type of hardware decoder available, or NONE if none available
     */
    static Type detectAvailable();

    /**
     * Get FFmpeg hardware device type for the given decoder type
     * @param type Hardware decoder type
     * @return AVHWDeviceType enum value, or AV_HWDEVICE_TYPE_NONE if not supported
     */
    static AVHWDeviceType getFFmpegDeviceType(Type type);

    /**
     * Get hardware pixel format for the given decoder type
     * @param type Hardware decoder type
     * @param codecId Codec ID (H264, HEVC, AV1)
     * @return AVPixelFormat enum value, or AV_PIX_FMT_NONE if not supported
     */
    static AVPixelFormat getHardwarePixelFormat(Type type, AVCodecID codecId);

    /**
     * Check if hardware decoder is available for the given codec
     * @param codecId Codec ID (H264, HEVC, AV1)
     * @return true if hardware decoder is available for this codec
     */
    static bool isAvailableForCodec(AVCodecID codecId);

    /**
     * Get name of hardware decoder type (for logging)
     * @param type Hardware decoder type
     * @return String name of the decoder type
     */
    static const char* getName(Type type);
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_HARDWAREDECODER_H

