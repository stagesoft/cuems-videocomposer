#include "GPUImageProcessor.h"
#include "../utils/Logger.h"
#include <cmath>
#include <algorithm>

namespace videocomposer {

GPUImageProcessor::GPUImageProcessor() {
}

GPUImageProcessor::~GPUImageProcessor() {
}

bool GPUImageProcessor::processCPU(const FrameBuffer& input, FrameBuffer& output,
                                   const LayerProperties& properties,
                                   const FrameInfo& frameInfo) {
    // GPU processor cannot process CPU frames
    // This should be handled by CPUImageProcessor
    LOG_VERBOSE << "GPUImageProcessor::processCPU called - not supported, use CPUImageProcessor";
    return false;
}

bool GPUImageProcessor::processGPU(const GPUTextureFrameBuffer& input, GPUTextureFrameBuffer& output,
                                  const LayerProperties& properties,
                                  const FrameInfo& frameInfo) {
    if (!input.isValid()) {
        return false;
    }

    // For GPU processing, we can often skip actual pixel manipulation
    // and use texture coordinates instead (especially for HAP)
    
    // Check if we can skip processing
    if (canSkip(properties, input.isHAPTexture())) {
        // No modifications needed - just copy reference
        output = input;
        return true;
    }

    // For HAP textures, most operations can be done via texture coordinates
    // without creating a new texture (handled in renderer)
    if (input.isHAPTexture()) {
        // HAP: Crop/panorama can be done via texture coordinates
        // Scale/rotation can be done via transform matrix
        // For now, just pass through - renderer will handle it
        output = input;
        return true;
    }

    // For non-HAP GPU textures, we may need shader-based processing
    // For now, just pass through - full shader implementation will come later
    output = input;
    return true;
}

bool GPUImageProcessor::canProcess(const LayerProperties& properties, bool isHAPCodec) const {
    // GPU processor can handle all operations for GPU textures
    // For HAP, operations are done via texture coordinates (zero-copy)
    // For other GPU textures, shaders can be used
    return true;
}

bool GPUImageProcessor::canSkip(const LayerProperties& properties, bool isHAPCodec) const {
    // Check if any GPU-side modifications are enabled
    // NOTE: Scale and rotation are handled by OpenGL transforms, not texture processing
    // Only crop and panorama require texture coordinate manipulation
    bool hasCrop = properties.crop.enabled;
    bool hasPanorama = properties.panoramaMode;

    // Skip GPU texture processing if no crop/panorama
    // (scale/rotation/opacity are handled by OpenGL transforms and blending)
    return !hasCrop && !hasPanorama;
}

void GPUImageProcessor::calculateTextureCoordinates(const LayerProperties& properties,
                                                    const FrameInfo& frameInfo,
                                                    float& texX, float& texY,
                                                    float& texWidth, float& texHeight) const {
    if (frameInfo.width == 0 || frameInfo.height == 0) {
        texX = 0.0f;
        texY = 0.0f;
        texWidth = 1.0f;
        texHeight = 1.0f;
        return;
    }

    // Panorama mode: crop to 50% width with pan offset
    if (properties.panoramaMode) {
        float cropWidth = frameInfo.width / 2.0f;
        float maxOffset = frameInfo.width - cropWidth;
        
        // Clamp pan offset
        int panOffset = properties.panOffset;
        if (panOffset < 0) panOffset = 0;
        if (panOffset > static_cast<int>(maxOffset)) panOffset = static_cast<int>(maxOffset);
        
        // Calculate texture coordinates
        texX = static_cast<float>(panOffset) / frameInfo.width;
        texY = 0.0f;
        texWidth = cropWidth / frameInfo.width;
        texHeight = 1.0f;
    }
    // General crop
    else if (properties.crop.enabled) {
        // Calculate crop rectangle in texture coordinates (0.0 to 1.0)
        texX = static_cast<float>(properties.crop.x) / frameInfo.width;
        texY = static_cast<float>(properties.crop.y) / frameInfo.height;
        texWidth = static_cast<float>(properties.crop.width) / frameInfo.width;
        texHeight = static_cast<float>(properties.crop.height) / frameInfo.height;
        
        // Clamp to valid range
        if (texX < 0.0f) texX = 0.0f;
        if (texY < 0.0f) texY = 0.0f;
        if (texX + texWidth > 1.0f) texWidth = 1.0f - texX;
        if (texY + texHeight > 1.0f) texHeight = 1.0f - texY;
    }
    else {
        // No crop - use full texture
        texX = 0.0f;
        texY = 0.0f;
        texWidth = 1.0f;
        texHeight = 1.0f;
    }
}

bool GPUImageProcessor::isGPUAvailable() const {
    // TODO: Check if OpenGL context is available
    // For now, assume GPU is available
    return true;
}

} // namespace videocomposer

