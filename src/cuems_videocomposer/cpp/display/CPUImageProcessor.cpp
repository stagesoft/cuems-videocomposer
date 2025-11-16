#include "CPUImageProcessor.h"
#include "../utils/Logger.h"
#include "../video/FrameFormat.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace videocomposer {

CPUImageProcessor::CPUImageProcessor() {
}

CPUImageProcessor::~CPUImageProcessor() {
}

bool CPUImageProcessor::processCPU(const FrameBuffer& input, FrameBuffer& output,
                                   const LayerProperties& properties,
                                   const FrameInfo& frameInfo) {
    if (!input.isValid()) {
        return false;
    }

    // Check if we can skip processing
    if (canSkip(properties, false)) {
        // No modifications needed - just copy
        if (!output.isValid() || output.info().width != input.info().width || 
            output.info().height != input.info().height) {
            if (!output.allocate(input.info())) {
                return false;
            }
        }
        std::memcpy(output.data(), input.data(), input.size());
        return true;
    }

    // Apply modifications in order: crop/panorama -> scale -> rotation
    FrameBuffer temp1, temp2;
    const FrameBuffer* current = &input;
    FrameBuffer* next = &temp1;

    // Step 1: Crop or Panorama
    if (properties.panoramaMode) {
        if (!applyPanorama(*current, *next, properties, frameInfo)) {
            return false;
        }
        current = next;
        next = (next == &temp1) ? &temp2 : &temp1;
    } else if (properties.crop.enabled) {
        if (!applyCrop(*current, *next, properties, frameInfo)) {
            return false;
        }
        current = next;
        next = (next == &temp1) ? &temp2 : &temp1;
    }

    // Step 2: Scale (if needed)
    bool hasScale = (std::abs(properties.scaleX - 1.0f) > 0.001f || 
                     std::abs(properties.scaleY - 1.0f) > 0.001f);
    if (hasScale) {
        if (!applyScale(*current, *next, properties, frameInfo)) {
            return false;
        }
        current = next;
        next = (next == &temp1) ? &temp2 : &temp1;
    }

    // Step 3: Rotation (if needed)
    bool hasRotation = (std::abs(properties.rotation) > 0.001f);
    if (hasRotation) {
        if (!applyRotation(*current, *next, properties, frameInfo)) {
            return false;
        }
        current = next;
    }

    // Copy final result to output
    if (current != &output) {
        if (!output.isValid() || output.info().width != current->info().width || 
            output.info().height != current->info().height) {
            if (!output.allocate(current->info())) {
                return false;
            }
        }
        std::memcpy(output.data(), current->data(), current->size());
    }

    return true;
}

bool CPUImageProcessor::processGPU(const GPUTextureFrameBuffer& input, GPUTextureFrameBuffer& output,
                                  const LayerProperties& properties,
                                  const FrameInfo& frameInfo) {
    // CPU processor cannot process GPU textures directly
    // This would require downloading from GPU first
    LOG_VERBOSE << "CPUImageProcessor::processGPU called - not supported, use GPUImageProcessor";
    return false;
}

bool CPUImageProcessor::canProcess(const LayerProperties& properties, bool isHAPCodec) const {
    // CPU processor can handle all operations for CPU frames
    return true;
}

bool CPUImageProcessor::canSkip(const LayerProperties& properties, bool isHAPCodec) const {
    // Check if any modifications are enabled
    bool hasCrop = properties.crop.enabled;
    bool hasPanorama = properties.panoramaMode;
    bool hasScale = (std::abs(properties.scaleX - 1.0f) > 0.001f || 
                     std::abs(properties.scaleY - 1.0f) > 0.001f);
    bool hasRotation = (std::abs(properties.rotation) > 0.001f);

    // Skip if no modifications at all
    return !hasCrop && !hasPanorama && !hasScale && !hasRotation;
}

bool CPUImageProcessor::applyCrop(const FrameBuffer& input, FrameBuffer& output,
                                  const LayerProperties& properties,
                                  const FrameInfo& frameInfo) {
    if (!input.isValid() || !properties.crop.enabled) {
        return false;
    }

    const auto& crop = properties.crop;
    
    // Validate crop rectangle
    if (crop.x < 0 || crop.y < 0 || 
        crop.x + crop.width > frameInfo.width ||
        crop.y + crop.height > frameInfo.height ||
        crop.width <= 0 || crop.height <= 0) {
        return false;
    }

    // Create output frame info
    FrameInfo outputInfo = frameInfo;
    outputInfo.width = crop.width;
    outputInfo.height = crop.height;

    if (!output.allocate(outputInfo)) {
        return false;
    }

    // Calculate bytes per pixel from format
    int bytesPerPixel = getBytesPerPixel(frameInfo.format);
    int inputStride = frameInfo.width * bytesPerPixel;
    int outputStride = crop.width * bytesPerPixel;

    // Copy cropped region
    const uint8_t* src = input.data() + (crop.y * inputStride) + (crop.x * bytesPerPixel);
    uint8_t* dst = output.data();

    for (int y = 0; y < crop.height; ++y) {
        std::memcpy(dst, src, outputStride);
        src += inputStride;
        dst += outputStride;
    }

    return true;
}

bool CPUImageProcessor::applyPanorama(const FrameBuffer& input, FrameBuffer& output,
                                      const LayerProperties& properties,
                                      const FrameInfo& frameInfo) {
    if (!input.isValid() || !properties.panoramaMode) {
        return false;
    }

    // Panorama mode: crop to 50% width with pan offset
    int cropWidth = frameInfo.width / 2;
    int maxOffset = frameInfo.width - cropWidth;
    
    // Clamp pan offset
    int panOffset = properties.panOffset;
    if (panOffset < 0) panOffset = 0;
    if (panOffset > maxOffset) panOffset = maxOffset;

    // Create output frame info
    FrameInfo outputInfo = frameInfo;
    outputInfo.width = cropWidth;
    outputInfo.height = frameInfo.height;

    if (!output.allocate(outputInfo)) {
        return false;
    }

    // Calculate bytes per pixel from format
    int bytesPerPixel = getBytesPerPixel(frameInfo.format);
    int inputStride = frameInfo.width * bytesPerPixel;
    int outputStride = cropWidth * bytesPerPixel;

    // Copy panorama region
    const uint8_t* src = input.data() + (panOffset * bytesPerPixel);
    uint8_t* dst = output.data();

    for (int y = 0; y < frameInfo.height; ++y) {
        std::memcpy(dst, src, outputStride);
        src += inputStride;
        dst += outputStride;
    }

    return true;
}

bool CPUImageProcessor::applyScale(const FrameBuffer& input, FrameBuffer& output,
                                  const LayerProperties& properties,
                                  const FrameInfo& frameInfo) {
    if (!input.isValid()) {
        return false;
    }

    // Calculate output dimensions
    int outputWidth = static_cast<int>(frameInfo.width * properties.scaleX);
    int outputHeight = static_cast<int>(frameInfo.height * properties.scaleY);

    if (outputWidth <= 0 || outputHeight <= 0) {
        return false;
    }

    // Create output frame info
    FrameInfo outputInfo = frameInfo;
    outputInfo.width = outputWidth;
    outputInfo.height = outputHeight;

    if (!output.allocate(outputInfo)) {
        return false;
    }

    // Simple nearest-neighbor scaling for now
    // TODO: Implement bilinear/bicubic interpolation for better quality
    int bytesPerPixel = getBytesPerPixel(frameInfo.format);
    int inputStride = frameInfo.width * bytesPerPixel;
    int outputStride = outputWidth * bytesPerPixel;

    const uint8_t* src = input.data();
    uint8_t* dst = output.data();

    for (int y = 0; y < outputHeight; ++y) {
        int srcY = (y * frameInfo.height) / outputHeight;
        const uint8_t* srcRow = src + (srcY * inputStride);
        
        for (int x = 0; x < outputWidth; ++x) {
            int srcX = (x * frameInfo.width) / outputWidth;
            const uint8_t* srcPixel = srcRow + (srcX * bytesPerPixel);
            
            std::memcpy(dst, srcPixel, bytesPerPixel);
            dst += bytesPerPixel;
        }
    }

    return true;
}

bool CPUImageProcessor::applyRotation(const FrameBuffer& input, FrameBuffer& output,
                                     const LayerProperties& properties,
                                     const FrameInfo& frameInfo) {
    if (!input.isValid()) {
        return false;
    }

    // For now, only support 90-degree increments
    // Normalize rotation to 0-360 range
    float rotation = std::fmod(properties.rotation, 360.0f);
    if (rotation < 0) rotation += 360.0f;

    // Round to nearest 90-degree increment
    int rotation90 = static_cast<int>(std::round(rotation / 90.0f)) % 4;

    if (rotation90 == 0) {
        // No rotation - just copy
        if (!output.isValid() || output.info().width != input.info().width || 
            output.info().height != input.info().height) {
            if (!output.allocate(input.info())) {
                return false;
            }
        }
        std::memcpy(output.data(), input.data(), input.size());
        return true;
    }

    // Calculate output dimensions (swap for 90/270)
    int outputWidth = (rotation90 == 1 || rotation90 == 3) ? frameInfo.height : frameInfo.width;
    int outputHeight = (rotation90 == 1 || rotation90 == 3) ? frameInfo.width : frameInfo.height;

    // Create output frame info
    FrameInfo outputInfo = frameInfo;
    outputInfo.width = outputWidth;
    outputInfo.height = outputHeight;

    if (!output.allocate(outputInfo)) {
        return false;
    }

    // Rotate pixel by pixel
    int bytesPerPixel = getBytesPerPixel(frameInfo.format);
    int inputStride = frameInfo.width * bytesPerPixel;
    int outputStride = outputWidth * bytesPerPixel;

    const uint8_t* src = input.data();
    uint8_t* dst = output.data();

    for (int y = 0; y < outputHeight; ++y) {
        for (int x = 0; x < outputWidth; ++x) {
            int srcX, srcY;
            
            // Calculate source coordinates based on rotation
            switch (rotation90) {
                case 1: // 90 degrees clockwise
                    srcX = y;
                    srcY = frameInfo.width - 1 - x;
                    break;
                case 2: // 180 degrees
                    srcX = frameInfo.width - 1 - x;
                    srcY = frameInfo.height - 1 - y;
                    break;
                case 3: // 270 degrees clockwise (90 counter-clockwise)
                    srcX = frameInfo.height - 1 - y;
                    srcY = x;
                    break;
                default:
                    srcX = x;
                    srcY = y;
                    break;
            }

            const uint8_t* srcPixel = src + (srcY * inputStride) + (srcX * bytesPerPixel);
            uint8_t* dstPixel = dst + (y * outputStride) + (x * bytesPerPixel);
            
            std::memcpy(dstPixel, srcPixel, bytesPerPixel);
        }
    }

    return true;
}

int CPUImageProcessor::getBytesPerPixel(PixelFormat format) {
    switch (format) {
        case PixelFormat::RGB24:
            return 3;
        case PixelFormat::RGBA32:
        case PixelFormat::BGRA32:
            return 4;
        case PixelFormat::YUV420P:
            // YUV420P is planar, but we typically work with packed RGB
            // Default to 3 for RGB conversion
            return 3;
        case PixelFormat::UYVY422:
            // UYVY422 is 2 bytes per pixel (packed YUV)
            return 2;
        default:
            // Default to 4 (RGBA) for safety
            return 4;
    }
}

} // namespace videocomposer

