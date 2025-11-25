#ifndef VIDEOCOMPOSER_LAYERDISPLAY_H
#define VIDEOCOMPOSER_LAYERDISPLAY_H

#include "LayerProperties.h"
#include "../video/FrameBuffer.h"
#include "../video/GPUTextureFrameBuffer.h"
#include "../video/FrameFormat.h"
#include "../display/GPUImageProcessor.h"
#include "../display/CPUImageProcessor.h"
#include <memory>

namespace videocomposer {

/**
 * LayerDisplay - Handles rendering and image modifications for a layer
 * 
 * This component is responsible for:
 * - Applying image modifications (pan, crop, scale, rotation, etc.)
 * - Preparing frames for rendering (GPU-first, CPU fallback)
 * - Managing display properties
 * - Optimizing for HAP textures (skip processing when possible)
 * 
 * This is separated from LayerPlayback to allow independent optimization
 * of playback vs rendering paths.
 */
class LayerDisplay {
public:
    LayerDisplay();
    ~LayerDisplay();

    // Set layer properties
    void setProperties(const LayerProperties& props) { properties_ = props; }
    const LayerProperties& getProperties() const { return properties_; }
    LayerProperties& getProperties() { return properties_; }

    // Prepare frame for rendering
    // Takes frame from LayerPlayback (CPU or GPU) and applies modifications
    // Returns true if frame is ready for rendering
    bool prepareFrame(const FrameBuffer* cpuFrame, const GPUTextureFrameBuffer* gpuFrame, bool isFrameOnGPU, bool isHAPCodec);

    // Get prepared frame buffer (for rendering) - returns const pointers to avoid copies
    // Returns true if frame is on GPU, false if on CPU
    bool getPreparedFrame(const FrameBuffer*& cpuBuffer, const GPUTextureFrameBuffer*& gpuBuffer) const;

    // Check if current frame is on GPU
    bool isFrameOnGPU() const { return preparedFrameOnGPU_; }

    // Check if frame is ready for rendering
    bool isReady() const { return frameReady_; }

    // Get frame info (for aspect ratio calculations, etc.)
    void setFrameInfo(const FrameInfo& info) { frameInfo_ = info; }
    const FrameInfo& getFrameInfo() const { return frameInfo_; }

private:
    LayerProperties properties_;
    FrameInfo frameInfo_;
    
    // Prepared frame buffers (used when modifications are applied)
    FrameBuffer preparedCpuBuffer_;
    GPUTextureFrameBuffer preparedGpuBuffer_;
    bool preparedFrameOnGPU_;
    bool frameReady_;
    
    // Source frame pointers (used for zero-copy when no modifications needed)
    const FrameBuffer* sourceFrameCpu_;
    const GPUTextureFrameBuffer* sourceFrameGpu_;

    // Image processors (GPU-first, CPU fallback)
    GPUImageProcessor gpuProcessor_;
    CPUImageProcessor cpuProcessor_;
    
    // Internal methods
    bool applyModificationsGPU(const GPUTextureFrameBuffer& input, GPUTextureFrameBuffer& output);
    bool applyModificationsCPU(const FrameBuffer& input, FrameBuffer& output);
    
    // Check if modifications can be skipped (e.g., no crop/panorama)
    bool canSkipModifications(bool isHAPCodec) const;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_LAYERDISPLAY_H

