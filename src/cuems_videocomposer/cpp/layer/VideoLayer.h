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
#include <string>
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

    // Cue UUID this layer was created for, when it has one. Carried so the
    // guard's refusal messages and the load-outcome record can name the cue
    // the operator knows, not just an internal slot number.
    void setCueId(const std::string& cueId) { cueId_ = cueId; }
    const std::string& getCueId() const { return cueId_; }
    
    // Time-scaling (applied to sync source frames)
    void setTimeOffset(int64_t offset);
    int64_t getTimeOffset() const;
    // The engine's integer /offset: day-aligned before use (LayerPlayback)
    void setEngineTimeOffset(int64_t rawFrames);
    bool hasPendingEngineOffset() const;
    
    void setTimeScale(double scale);
    double getTimeScale() const;
    
    void setWraparound(bool enabled);
    bool getWraparound() const;

    // The engine's loop period and pass count (869ejxng3 section 4.4); see
    // LayerPlayback::setLoopPeriod.
    void setLoopPeriod(int64_t num, int64_t den, int64_t frames);
    void clearLoopPeriod() { playback_.clearLoopPeriod(); }
    void setLoopPasses(int passes) { playback_.setLoopPasses(passes); }
    
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

    /**
     * /visible (the router's handler, moved here so the gate below sees it).
     * Unchanged without visible_at: 0->1 sets awaitingFrame. With a visible_at
     * gate pending, 1 is covered by the gate (no early flip, no awaitingFrame:
     * it would skip frame 0) and 0 drops the gate.
     */
    void setVisible(bool visible);

    /**
     * /visible_at (869feynrj, plan §4.6). 1: stay invisible and become
     * visible on the first presented frame whose content time is >= 0 (the
     * first update whose poll gives a raw content frame >= 0 with that frame
     * loaded), in that same update, so the flip rides the frame's
     * submission. 0: drop a pending reveal, or hide a layer the gate already
     * revealed (the engine's cancel is final, D5). Journal lines:
     * journal::visibleAt*().
     */
    enum class VisibleAtOutcome { Armed, Unchanged, Dropped, Hidden, Ignored };
    VisibleAtOutcome setVisibleAt(bool on);
    bool visibleAtPending() const { return visibleAtPending_; }

    // A gate still pending this long logs one WARNING (journal::
    // visibleAtPendingLong). The engine arms it at start - F (~0.1 s) and sets
    // video layers up at most ~5 s before their start (run_videoCue's
    // run-ahead), so past 6 s the start is not coming: /mtcfollow lost, MTC
    // stopped, or the offset never placed.
    static constexpr int64_t kVisibleAtPendingWarnMs = 6000;
    void setVisibleAtWarnAfterMsForTesting(int64_t ms) { visibleAtWarnAfterMs_ = ms; }
    int visibleAtPendingWarnings() const { return visibleAtPendingWarnings_; }

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

    // visible_at gate (869feynrj)
    bool visibleAtPending_ = false;
    bool revealedByGate_ = false;     // visible because the gate fired
    int64_t revealedAtMs_ = 0;        // steady clock, for the cancel line
    int64_t visibleAtArmedMs_ = 0;    // steady clock, for the pending WARNING
    bool visibleAtWarned_ = false;    // once per gate
    int64_t visibleAtWarnAfterMs_ = kVisibleAtPendingWarnMs;
    int visibleAtPendingWarnings_ = 0;
    std::string logTag() const;

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

