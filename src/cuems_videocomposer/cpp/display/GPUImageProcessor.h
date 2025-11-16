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

