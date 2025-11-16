#ifndef VIDEOCOMPOSER_VIDEOLAYER_H
#define VIDEOCOMPOSER_VIDEOLAYER_H

#include "LayerProperties.h"
#include "LayerPlayback.h"
#include "LayerDisplay.h"
#include "../input/InputSource.h"
#include "../sync/SyncSource.h"
#include "../video/FrameBuffer.h"
#include "../video/GPUTextureFrameBuffer.h"
#include <memory>
#include <cstdint>

namespace videocomposer {

/**
 * VideoLayer - Represents a single video layer with its own input and sync
 * 
 * Each layer has:
 * - An InputSource (video file, live video, stream, etc.)
 * - A SyncSource (MIDI, LTC, manual, etc.)
 * - Display properties (position, size, opacity, etc.)
 * - Playback state (playing, paused, current frame)
 */
class VideoLayer {
public:
    VideoLayer();
    ~VideoLayer();

    // Layer management
    void setInputSource(std::unique_ptr<InputSource> input);
    void setSyncSource(std::unique_ptr<SyncSource> sync);
    
    InputSource* getInputSource() const;
    SyncSource* getSyncSource() const;

    // Properties
    LayerProperties& properties();
    const LayerProperties& properties() const;

    // Playback control
    bool play();
    bool pause();
    bool isPlaying() const;
    
    bool seek(int64_t frameNumber);
    int64_t getCurrentFrame() const;
    
    // Update layer (called from main loop)
    void update();
    
    // Render layer (called from display backend)
    bool render(FrameBuffer& outputBuffer);

    // Get layer state
    bool isReady() const;
    FrameInfo getFrameInfo() const;
    bool isHAPCodec() const;

    // Get frame buffer (for rendering) - backward compatibility
    // Returns CPU frame buffer (for now, until all callers are updated)
    const FrameBuffer& getFrameBuffer() const;
    
    // Get prepared frame for rendering (new API - supports GPU textures)
    // Returns true if frame is on GPU, false if on CPU
    bool getPreparedFrame(FrameBuffer& cpuBuffer, GPUTextureFrameBuffer& gpuBuffer) const;
    
    // Check if current frame is on GPU
    bool isFrameOnGPU() const;

    // Layer ID
    void setLayerId(int id) { layerId_ = id; }
    int getLayerId() const { return layerId_; }
    
    // Time-scaling (applied to sync source frames)
    void setTimeOffset(int64_t offset);
    int64_t getTimeOffset() const;
    
    void setTimeScale(double scale);
    double getTimeScale() const;
    
    void setWraparound(bool enabled);
    bool getWraparound() const;
    
    // Reverse playback (multiplies timescale by -1.0 and adjusts offset)
    void reverse();

private:
    // Composed components
    LayerPlayback playback_;
    LayerDisplay display_;
    
    // Layer identification
    int layerId_;
    
    // Backward compatibility: CPU frame buffer cache
    mutable FrameBuffer frameBufferCache_;
    mutable bool frameBufferCacheValid_;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_VIDEOLAYER_H

