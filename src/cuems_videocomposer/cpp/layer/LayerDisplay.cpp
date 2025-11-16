#include "LayerDisplay.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace videocomposer {

LayerDisplay::LayerDisplay()
    : preparedFrameOnGPU_(false)
    , frameReady_(false)
{
}

LayerDisplay::~LayerDisplay() {
}

bool LayerDisplay::prepareFrame(const FrameBuffer& cpuFrame, const GPUTextureFrameBuffer& gpuFrame, 
                                bool isFrameOnGPU, bool isHAPCodec) {
    frameReady_ = false;

    // Check if we can skip modifications (HAP with no transforms)
    if (canSkipModifications(isHAPCodec)) {
        // No modifications needed - use frame directly
        if (isFrameOnGPU && gpuFrame.isValid()) {
            preparedGpuBuffer_ = gpuFrame;
            preparedFrameOnGPU_ = true;
            frameReady_ = true;
            return true;
        } else if (!isFrameOnGPU && cpuFrame.isValid()) {
            // For CPU frames, we still need to prepare (upload to GPU later)
            preparedCpuBuffer_ = cpuFrame;
            preparedFrameOnGPU_ = false;
            frameReady_ = true;
            return true;
        }
        return false;
    }

    // Apply modifications
    // For now, this is a simplified version
    // Full GPU-first processing will be implemented in future phases
    
    if (isFrameOnGPU && gpuFrame.isValid()) {
        // Frame is on GPU - try GPU processing first
        if (applyModificationsGPU(gpuFrame, preparedGpuBuffer_)) {
            preparedFrameOnGPU_ = true;
            frameReady_ = true;
            return true;
        } else {
            // GPU processing failed - fallback to CPU
            // This would require downloading from GPU, which we avoid for now
            // For now, just use the GPU frame as-is
            preparedGpuBuffer_ = gpuFrame;
            preparedFrameOnGPU_ = true;
            frameReady_ = true;
            return true;
        }
    } else if (!isFrameOnGPU && cpuFrame.isValid()) {
        // Frame is on CPU - apply CPU processing
        if (applyModificationsCPU(cpuFrame, preparedCpuBuffer_)) {
            preparedFrameOnGPU_ = false;
            frameReady_ = true;
            return true;
        }
        return false;
    }

    return false;
}

bool LayerDisplay::getPreparedFrame(FrameBuffer& cpuBuffer, GPUTextureFrameBuffer& gpuBuffer) const {
    if (!frameReady_) {
        return false;
    }

    if (preparedFrameOnGPU_) {
        gpuBuffer = preparedGpuBuffer_;
        return true; // true = on GPU
    } else {
        cpuBuffer = preparedCpuBuffer_;
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
    // This handles crop, panorama, scale, rotation when GPU processing unavailable
    
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

