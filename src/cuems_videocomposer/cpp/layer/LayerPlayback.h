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

    // Set input and sync sources (non-shared — converts to shared_ptr internally)
    void setInputSource(std::unique_ptr<InputSource> input);
    void setSyncSource(std::unique_ptr<SyncSource> sync);

    // Set input and sync sources (shared — for shared decoder layers)
    void setInputSource(std::shared_ptr<InputSource> input, bool isShared, bool isDriver);
    void setSyncSource(std::shared_ptr<SyncSource> sync);

    InputSource* getInputSource() const { return inputSource_.get(); }
    SyncSource* getSyncSource() const { return syncSource_.get(); }
    std::shared_ptr<InputSource> getSharedInputSource() const { return inputSource_; }
    std::shared_ptr<SyncSource> getSharedSyncSource() const { return syncSource_; }
    bool hasInputSource() const { return inputSource_ != nullptr; }

    // Shared decoder flags
    bool isSharedLayer() const { return isSharedLayer_; }
    bool isDecodeDriver() const { return isDecodeDriver_; }
    void setDecodeDriver(bool v) { isDecodeDriver_ = v; }

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
    std::shared_ptr<InputSource> inputSource_;
    std::shared_ptr<SyncSource> syncSource_;

    // Shared decoder flags (NOT derived from use_count — explicit)
    bool isSharedLayer_ = false;   // true when sharing an InputSource with other layers
    bool isDecodeDriver_ = false;  // true when this layer drives decode (calls readFrame/readFrameToTexture)
    
    // Playback state
    bool playing_;
    int64_t currentFrame_;
    int64_t lastSyncFrame_;
    int64_t timeOffset_;  // Time offset applied to sync frames
    double timeScale_;    // Time multiplier (default: 1.0)
    bool wraparound_;     // Enable wrap-around/loop (seeks to 0 when playback ends)
    bool mtcFollow_;      // Enable/disable MTC following. Default FALSE: a layer
                          // ignores its sync source until the engine sends
                          // /mtcfollow 1 at the cue's start (see 0499270).
    
    // MTC sync state (per-layer, not static!)
    bool wasRolling_;        // Previous rolling state for change detection
    int64_t lastLoggedFrame_; // Last logged frame for periodic logging
    int debugCounter_;       // Debug counter for periodic logging
    bool loggedExceededDuration_; // True if we've logged "frame exceeded duration" message
    
    // Frame pacing diagnosis (per-layer)
    int64_t vsyncCount_;           // Approximate vsync counter (incremented each update)
    int64_t lastFrameChangeVsync_; // vsyncCount_ when frame last changed
    int64_t lastVideoFrame_;       // Last video frame number for pacing diagnosis
    
    // Frame buffers (CPU and GPU)
    FrameBuffer cpuFrameBuffer_;
    GPUTextureFrameBuffer gpuFrameBuffer_;
    bool frameOnGPU_;     // True if current frame is in GPU buffer
    
    // Internal methods
    void updateFromSyncSource();
    bool loadFrame(int64_t frameNumber);
    bool copyFromDriverCache(int64_t frameNumber);  // For shared secondary layers
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_LAYERPLAYBACK_H

