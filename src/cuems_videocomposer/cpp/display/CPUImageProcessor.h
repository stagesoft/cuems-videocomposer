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

#ifndef VIDEOCOMPOSER_CPUIMAGEPROCESSOR_H
#define VIDEOCOMPOSER_CPUIMAGEPROCESSOR_H

#include "ImageProcessor.h"

namespace videocomposer {

/**
 * CPUImageProcessor - CPU-side image processing
 * 
 * Uses CPU pixel manipulation for:
 * - Crop (copy pixel region)
 * - Panorama mode (copy 50% width region with offset)
 * - Scale (resampling/interpolation)
 * - Rotation (affine transformation)
 * 
 * This is a fallback when GPU processing is not available or not possible.
 */
class CPUImageProcessor : public ImageProcessor {
public:
    CPUImageProcessor();
    ~CPUImageProcessor() override;

    bool processCPU(const FrameBuffer& input, FrameBuffer& output,
                   const LayerProperties& properties,
                   const FrameInfo& frameInfo) override;

    bool processGPU(const GPUTextureFrameBuffer& input, GPUTextureFrameBuffer& output,
                  const LayerProperties& properties,
                  const FrameInfo& frameInfo) override;

    bool canProcess(const LayerProperties& properties, bool isHAPCodec) const override;

    bool canSkip(const LayerProperties& properties, bool isHAPCodec) const override;

private:
    // Apply crop operation
    bool applyCrop(const FrameBuffer& input, FrameBuffer& output,
                  const LayerProperties& properties,
                  const FrameInfo& frameInfo);

    // Apply panorama mode (50% width crop with offset)
    bool applyPanorama(const FrameBuffer& input, FrameBuffer& output,
                      const LayerProperties& properties,
                      const FrameInfo& frameInfo);

    // Apply scale operation (simple nearest-neighbor for now)
    bool applyScale(const FrameBuffer& input, FrameBuffer& output,
                   const LayerProperties& properties,
                   const FrameInfo& frameInfo);

    // Apply rotation (90/180/270 degrees for now)
    bool applyRotation(const FrameBuffer& input, FrameBuffer& output,
                      const LayerProperties& properties,
                      const FrameInfo& frameInfo);
    
    // Helper to get bytes per pixel from pixel format
    int getBytesPerPixel(PixelFormat format);
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_CPUIMAGEPROCESSOR_H

