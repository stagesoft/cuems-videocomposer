#ifndef VIDEOCOMPOSER_LAYERPLAYBACK_H
#define VIDEOCOMPOSER_LAYERPLAYBACK_H

#include "../input/InputSource.h"
#include "../sync/SyncSource.h"
#include "../video/FrameBuffer.h"
#include "../video/GPUTextureFrameBuffer.h"
#include <memory>
#include <cstdint>

namespace videocomposer {

/**
 * LayerPlayback - Handles sync and frame loading for a layer
 * 
 * This component is responsible for:
 * - Polling sync source (MIDI, LTC, etc.)
 * - Converting sync frames to input frames (with time-scaling)
 * - Loading frames from InputSource (CPU or GPU)
 * - Managing playback state (playing, paused)
 * 
 * This is separated from LayerDisplay to allow independent optimization
 * of playback vs rendering paths.
 */
class LayerPlayback {
public:
    LayerPlayback();
    ~LayerPlayback();

    // Set input and sync sources
    void setInputSource(std::unique_ptr<InputSource> input);
    void setSyncSource(std::unique_ptr<SyncSource> sync);
    
    InputSource* getInputSource() const { return inputSource_.get(); }
    SyncSource* getSyncSource() const { return syncSource_.get(); }

    // Playback control
    bool play();
    bool pause();
    bool isPlaying() const { return playing_; }
    
    bool seek(int64_t frameNumber);
    int64_t getCurrentFrame() const { return currentFrame_; }
    
    // Update playback (called from main loop)
    // Polls sync source and loads frames as needed
    void update();
    
    // Get frame buffer (CPU or GPU) - returns const references to avoid copies
    // Returns true if frame is on GPU, false if on CPU
    bool getFrameBuffer(const FrameBuffer*& cpuBuffer, const GPUTextureFrameBuffer*& gpuBuffer) const;
    
    // Check if current frame is on GPU
    bool isFrameOnGPU() const { return frameOnGPU_; }
    
    // Check if current source is HAP codec
    bool isHAPCodec() const;
    
    // Get layer state
    bool isReady() const;
    FrameInfo getFrameInfo() const;
    
    // Time-scaling (applied to sync source frames)
    void setTimeOffset(int64_t offset) { timeOffset_ = offset; }
    int64_t getTimeOffset() const { return timeOffset_; }
    
    void setTimeScale(double scale) { timeScale_ = scale; }
    double getTimeScale() const { return timeScale_; }
    
    void setWraparound(bool enabled) { wraparound_ = enabled; }
    bool getWraparound() const { return wraparound_; }
    
    // MTC follow control (enable/disable MTC following for this layer)
    void setMtcFollow(bool enabled) { mtcFollow_ = enabled; }
    bool getMtcFollow() const { return mtcFollow_; }
    
    // Reverse playback (multiplies timescale by -1.0 and adjusts offset)
    void reverse();
    
    // Check if playback has reached the end
    // Returns true if playback has ended, false otherwise
    bool checkPlaybackEnd() const;

private:
    std::unique_ptr<InputSource> inputSource_;
    std::unique_ptr<SyncSource> syncSource_;
    
    // Playback state
    bool playing_;
    int64_t currentFrame_;
    int64_t lastSyncFrame_;
    int64_t timeOffset_;  // Time offset applied to sync frames
    double timeScale_;    // Time multiplier (default: 1.0)
    bool wraparound_;     // Enable wrap-around/loop
    bool mtcFollow_;      // Enable/disable MTC following (default: true)
    
    // MTC sync state (per-layer, not static!)
    bool wasRolling_;        // Previous rolling state for change detection
    int64_t lastLoggedFrame_; // Last logged frame for periodic logging
    int debugCounter_;       // Debug counter for periodic logging
    
    // Frame buffers (CPU and GPU)
    FrameBuffer cpuFrameBuffer_;
    GPUTextureFrameBuffer gpuFrameBuffer_;
    bool frameOnGPU_;     // True if current frame is in GPU buffer
    
    // Internal methods
    void updateFromSyncSource();
    bool loadFrame(int64_t frameNumber);
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_LAYERPLAYBACK_H

