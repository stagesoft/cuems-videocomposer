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

#ifndef VIDEOCOMPOSER_VIDEOLAYER_H
#define VIDEOCOMPOSER_VIDEOLAYER_H

#include "LayerProperties.h"
#include "LayerPlayback.h"
#include "LayerDisplay.h"
#include "OutputFit.h"
#include "../input/InputSource.h"
#include "../sync/SyncSource.h"
#include "../video/FrameBuffer.h"
#include "../video/GPUTextureFrameBuffer.h"
#include <memory>
#include <cstdint>
#include <string>

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

    // Update priority (decoupled from z-order): lower = updated earlier.
    // Drivers get priority 0, shared secondaries get priority 1.
    void setUpdatePriority(int p) { updatePriority_ = p; }
    int getUpdatePriority() const { return updatePriority_; }

    // Direct access to playback for shared-layer setup
    LayerPlayback& playback() { return playback_; }

    // Properties access
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
    
    // Get prepared frame for rendering (returns const pointers - zero-copy)
    // Returns true if frame is on GPU, false if on CPU
    bool getPreparedFrame(const FrameBuffer*& cpuBuffer, const GPUTextureFrameBuffer*& gpuBuffer) const;
    
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
    
    // MTC follow control (enable/disable MTC following for this layer)
    void setMtcFollow(bool enabled);
    bool getMtcFollow() const;
    
    // Reverse playback (multiplies timescale by -1.0 and adjusts offset)
    void reverse();

    // Place the layer on one output region (layer/<id>/fit_output). The
    // placement is applied at once and applied again by update() whenever the
    // source's dimensions change or stop being provisional (the command
    // usually arrives before the async load has delivered an input). Every
    // application logs a journal::fitOutput() line. An explicit position or
    // scale command clears it.
    void setOutputFit(const std::string& label, const FitRegion& region,
                      int canvasWidth, int canvasHeight, FitMode mode);
    void clearOutputFit() { outputFit_.active = false; }
    bool hasOutputFit() const { return outputFit_.active; }

private:
    struct OutputFitState {
        bool active = false;
        std::string label;  // cue/layer id, for the log line
        FitRegion region;
        int canvasWidth = 0;
        int canvasHeight = 0;
        FitMode mode = FitMode::Fill;
        // What the last application was computed from
        int appliedWidth = -1;
        int appliedHeight = -1;
        std::string appliedBasis;
    };
    OutputFitState outputFit_;
    void applyOutputFit(bool force);

    // Composed components
    LayerPlayback playback_;
    LayerDisplay display_;

    // Layer identification
    int layerId_;

    // Update priority (decoupled from z-order for shared-decoder ordering)
    int updatePriority_ = 0;
    
    // Backward compatibility: CPU frame buffer cache
    mutable FrameBuffer frameBufferCache_;
    mutable bool frameBufferCacheValid_;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_VIDEOLAYER_H

