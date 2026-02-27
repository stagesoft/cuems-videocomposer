/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (C) 2020-2026 Stage Lab Coop.
 * Author: Ion Reguera <ion@stagelab.coop>
 *
 * This file is part of cuems-videocomposer.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

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
 * - QSV (Intel Quick Sync Video - Linux/Windows)
 * - VAAPI (Linux, Intel/AMD GPUs)
 * - CUDA (NVIDIA GPUs)
 * - VideoToolbox (macOS)
 * - DXVA2 (Windows)
 */
class HardwareDecoder {
public:
    enum class Type {
        NONE,           // No hardware decoder available
        QSV,            // Intel Quick Sync Video (QSV/libmfx)
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
     * Check if hardware decoder is available for the given codec (with pre-detected type)
     * @param codecId Codec ID (H264, HEVC, AV1)
     * @param hwType Pre-detected hardware decoder type
     * @return true if hardware decoder is available for this codec
     */
    static bool isAvailableForCodec(AVCodecID codecId, Type hwType);

    /**
     * Get name of hardware decoder type (for logging)
     * @param type Hardware decoder type
     * @return String name of the decoder type
     */
    static const char* getName(Type type);
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_HARDWAREDECODER_H

