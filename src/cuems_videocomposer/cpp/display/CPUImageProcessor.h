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

