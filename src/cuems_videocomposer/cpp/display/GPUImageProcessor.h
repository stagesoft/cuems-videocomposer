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

#ifndef VIDEOCOMPOSER_GPUIMAGEPROCESSOR_H
#define VIDEOCOMPOSER_GPUIMAGEPROCESSOR_H

#include "ImageProcessor.h"

namespace videocomposer {

/**
 * GPUImageProcessor - GPU-side image processing
 * 
 * Uses OpenGL shaders and texture coordinates for:
 * - Crop (via texture coordinates)
 * - Panorama mode (via texture coordinates)
 * - Scale (via viewport/transform)
 * - Rotation (via transform matrix)
 * 
 * For HAP textures, most operations can be done via texture coordinates
 * without actual pixel processing (zero-copy).
 */
class GPUImageProcessor : public ImageProcessor {
public:
    GPUImageProcessor();
    ~GPUImageProcessor() override;

    bool processCPU(const FrameBuffer& input, FrameBuffer& output,
                   const LayerProperties& properties,
                   const FrameInfo& frameInfo) override;

    bool processGPU(const GPUTextureFrameBuffer& input, GPUTextureFrameBuffer& output,
                   const LayerProperties& properties,
                   const FrameInfo& frameInfo) override;

    bool canProcess(const LayerProperties& properties, bool isHAPCodec) const override;

    bool canSkip(const LayerProperties& properties, bool isHAPCodec) const override;

private:
    // Calculate texture coordinates for crop/panorama
    void calculateTextureCoordinates(const LayerProperties& properties,
                                    const FrameInfo& frameInfo,
                                    float& texX, float& texY,
                                    float& texWidth, float& texHeight) const;

    // Check if GPU processing is available
    bool isGPUAvailable() const;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_GPUIMAGEPROCESSOR_H

