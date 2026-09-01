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
#include "../input/InputSource.h"
#include "../sync/SyncSource.h"
#include "../video/FrameBuffer.h"
#include "../video/GPUTextureFrameBuffer.h"
#include <memory>
#include <string>
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

    // Cue UUID this layer was created for, when it has one. Carried so the
    // guard's refusal messages and the load-outcome record can name the cue
    // the operator knows, not just an internal slot number.
    void setCueId(const std::string& cueId) { cueId_ = cueId; }
    const std::string& getCueId() const { return cueId_; }
    
    // Time-scaling (applied to sync source frames)
    void setTimeOffset(int64_t offset);
    int64_t getTimeOffset() const;
    
    void setTimeScale(double scale);
    double getTimeScale() const;
    
    void setWraparound(bool enabled);
    bool getWraparound() const;
    
    /**
     * MTC follow control - and the hang guard's only decision point.
     *
     * Enabling is what turns a loaded layer into a decoding one, so this is
     * where a 4K-class session is reserved against the cap. Returns false
     * when the guard refused: the layer then stays loaded and holding its
     * frame, following stays off, and everything already playing is
     * untouched. Disabling always succeeds and gives the slot back.
     */
    bool setMtcFollow(bool enabled);
    bool getMtcFollow() const;

    /**
     * Re-run the guard decision after the input source changes.
     *
     * OSC arrives in whatever order it arrives: /mtcfollow 1 can land before
     * the file has finished loading, and a layer that is already following
     * can have its source replaced by a re-arm. In both cases the layer is
     * only really decoding once it has a source, so the reservation is
     * reconciled here rather than assumed at the moment the command arrived.
     */
    void reconcileGuardReservation();

    /**
     * Take over the guard slot of the decode driver being removed.
     *
     * Used when this layer is promoted to decode driver for a shared source:
     * the decode session continues, it just has a new owner, so the slot moves
     * with it rather than being released and re-requested.
     */
    void inheritGuardReservation(VideoLayer& fromLayer);
    
    // Reverse playback (multiplies timescale by -1.0 and adjusts offset)
    void reverse();

private:
    // Composed components
    LayerPlayback playback_;
    LayerDisplay display_;

    // Layer identification
    int layerId_;
    std::string cueId_;

    /**
     * Whether this layer currently holds a slot in the guard's ledger.
     *
     * The reservation belongs to the layer, not to the input source, and that
     * is deliberate: re-arming a cue over a loaded layer replaces the source
     * while the layer - and its slot - persist, so a re-load is net-zero and
     * can never be refused on account of the session it is replacing. Getting
     * that wrong would leave an output silently showing the previous cue's
     * media.
     */
    bool guardReserved_ = false;

    /** Applies the guard decision for a desired follow state. */
    bool applyGuardedFollow(bool wantFollow);

    // Update priority (decoupled from z-order for shared-decoder ordering)
    int updatePriority_ = 0;
    
    // Backward compatibility: CPU frame buffer cache
    mutable FrameBuffer frameBufferCache_;
    mutable bool frameBufferCacheValid_;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_VIDEOLAYER_H

