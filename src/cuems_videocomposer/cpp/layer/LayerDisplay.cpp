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

#include "LayerDisplay.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace videocomposer {

LayerDisplay::LayerDisplay()
    : preparedFrameOnGPU_(false)
    , frameReady_(false)
    , sourceFrameCpu_(nullptr)
    , sourceFrameGpu_(nullptr)
{
}

LayerDisplay::~LayerDisplay() {
}

bool LayerDisplay::prepareFrame(const FrameBuffer* cpuFrame, const GPUTextureFrameBuffer* gpuFrame, 
                                bool isFrameOnGPU, bool isHAPCodec) {
    frameReady_ = false;
    sourceFrameCpu_ = nullptr;
    sourceFrameGpu_ = nullptr;

    // Check if we can skip modifications (no crop/panorama)
    if (canSkipModifications(isHAPCodec)) {
        // No modifications needed - store pointer to source frame (zero-copy)
        if (isFrameOnGPU && gpuFrame && gpuFrame->isValid()) {
            sourceFrameGpu_ = gpuFrame;
            preparedFrameOnGPU_ = true;
            frameReady_ = true;
            return true;
        } else if (!isFrameOnGPU && cpuFrame && cpuFrame->isValid()) {
            // For CPU frames without modifications, just point to source
            sourceFrameCpu_ = cpuFrame;
            preparedFrameOnGPU_ = false;
            frameReady_ = true;
            return true;
        }
        return false;
    }

    // Apply modifications (crop/panorama only - scale/rotation handled by OpenGL)
    // This path requires actual pixel manipulation, so we need our own buffer
    
    if (isFrameOnGPU && gpuFrame && gpuFrame->isValid()) {
        // Frame is on GPU - try GPU processing first
        if (applyModificationsGPU(*gpuFrame, preparedGpuBuffer_)) {
            sourceFrameGpu_ = nullptr;  // Using our processed buffer
            preparedFrameOnGPU_ = true;
            frameReady_ = true;
            return true;
        } else {
            // GPU processing failed - just use source frame as-is
            sourceFrameGpu_ = gpuFrame;
            preparedFrameOnGPU_ = true;
            frameReady_ = true;
            return true;
        }
    } else if (!isFrameOnGPU && cpuFrame && cpuFrame->isValid()) {
        // Frame is on CPU - apply CPU processing (crop/panorama)
        if (applyModificationsCPU(*cpuFrame, preparedCpuBuffer_)) {
            sourceFrameCpu_ = nullptr;  // Using our processed buffer
            preparedFrameOnGPU_ = false;
            frameReady_ = true;
            return true;
        }
        return false;
    }

    return false;
}

bool LayerDisplay::getPreparedFrame(const FrameBuffer*& cpuBuffer, const GPUTextureFrameBuffer*& gpuBuffer) const {
    if (!frameReady_) {
        cpuBuffer = nullptr;
        gpuBuffer = nullptr;
        return false;
    }

    if (preparedFrameOnGPU_) {
        // Return pointer to GPU frame (either source or processed)
        gpuBuffer = sourceFrameGpu_ ? sourceFrameGpu_ : &preparedGpuBuffer_;
        cpuBuffer = nullptr;
        return true; // true = on GPU
    } else {
        // Return pointer to CPU frame (either source or processed)
        cpuBuffer = sourceFrameCpu_ ? sourceFrameCpu_ : &preparedCpuBuffer_;
        gpuBuffer = nullptr;
        return false; // false = on CPU
    }
}

bool LayerDisplay::applyModificationsGPU(const GPUTextureFrameBuffer& input, GPUTextureFrameBuffer& output) {
    // Use GPUImageProcessor for GPU-side modifications
    // For HAP textures, most operations are done via texture coordinates (zero-copy)
    // For other GPU textures, shader-based processing can be added later
    
    if (!gpuProcessor_.canProcess(properties_, input.isHAPTexture())) {
        // GPU processor cannot handle this - just copy
        output = input;
        return true;
    }
    
    return gpuProcessor_.processGPU(input, output, properties_, frameInfo_);
}

bool LayerDisplay::applyModificationsCPU(const FrameBuffer& input, FrameBuffer& output) {
    // Use CPUImageProcessor for CPU-side modifications (fallback)
    // This handles crop, panorama when GPU processing unavailable
    
    if (!input.isValid()) {
        return false;
    }
    
    if (!cpuProcessor_.canProcess(properties_, false)) {
        // CPU processor cannot handle this - just copy
        if (!output.isValid() || output.info().width != input.info().width || 
            output.info().height != input.info().height) {
            if (!output.allocate(input.info())) {
                return false;
            }
        }
        memcpy(output.data(), input.data(), input.size());
        return true;
    }
    
    return cpuProcessor_.processCPU(input, output, properties_, frameInfo_);
}

bool LayerDisplay::canSkipModifications(bool isHAPCodec) const {
    // Use ImageProcessor to check if modifications can be skipped
    // This delegates to the appropriate processor (GPU or CPU)
    if (isHAPCodec) {
        // For HAP, use GPU processor's skip logic
        return gpuProcessor_.canSkip(properties_, true);
    } else {
        // For other codecs, check both processors
        // If GPU can skip, we can skip; otherwise check CPU
        if (gpuProcessor_.canSkip(properties_, false)) {
            return true;
        }
        return cpuProcessor_.canSkip(properties_, false);
    }
}

} // namespace videocomposer
