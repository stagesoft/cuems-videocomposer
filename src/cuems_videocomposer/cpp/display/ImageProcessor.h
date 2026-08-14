/*
 * SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
 *
 * This file is part of cuems-videocomposer.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef VIDEOCOMPOSER_IMAGEPROCESSOR_H
#define VIDEOCOMPOSER_IMAGEPROCESSOR_H

#include "../layer/LayerProperties.h"
#include "../video/FrameBuffer.h"
#include "../video/GPUTextureFrameBuffer.h"
#include "../video/FrameFormat.h"

namespace videocomposer {

/**
 * ImageProcessor - Abstract interface for image processing operations
 * 
 * This interface defines operations for applying image modifications:
 * - Crop (rectangle or panorama mode)
 * - Scale (X/Y scaling)
 * - Rotation
 * - Pan (for panorama mode)
 * 
 * Implementations:
 * - GPUImageProcessor: GPU-side processing (shaders, texture coordinates)
 * - CPUImageProcessor: CPU-side processing (pixel manipulation)
 */
class ImageProcessor {
public:
    virtual ~ImageProcessor() = default;

    /**
     * Process a CPU frame buffer
     * @param input Input frame buffer
     * @param output Output frame buffer (will be allocated if needed)
     * @param properties Layer properties (crop, scale, rotation, etc.)
     * @param frameInfo Original frame information
     * @return true on success, false on failure
     */
    virtual bool processCPU(const FrameBuffer& input, FrameBuffer& output,
                           const LayerProperties& properties,
                           const FrameInfo& frameInfo) = 0;

    /**
     * Process a GPU texture frame buffer
     * @param input Input GPU texture
     * @param output Output GPU texture (will be allocated if needed)
     * @param properties Layer properties (crop, scale, rotation, etc.)
     * @param frameInfo Original frame information
     * @return true on success, false on failure
     */
    virtual bool processGPU(const GPUTextureFrameBuffer& input, GPUTextureFrameBuffer& output,
                           const LayerProperties& properties,
                           const FrameInfo& frameInfo) = 0;

    /**
     * Check if this processor can handle the given operation
     * @param properties Layer properties to check
     * @param isHAPCodec Whether the input is HAP codec
     * @return true if processor can handle, false otherwise
     */
    virtual bool canProcess(const LayerProperties& properties, bool isHAPCodec) const = 0;

    /**
     * Check if modifications can be skipped (no-op case)
     * @param properties Layer properties to check
     * @param isHAPCodec Whether the input is HAP codec
     * @return true if no processing needed, false otherwise
     */
    virtual bool canSkip(const LayerProperties& properties, bool isHAPCodec) const = 0;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_IMAGEPROCESSOR_H

