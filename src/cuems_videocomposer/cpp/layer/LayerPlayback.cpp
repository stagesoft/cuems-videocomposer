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

#include "LayerPlayback.h"
#include "../guard/SaturationSignals.h"
#include "../utils/Logger.h"
#include "../utils/SMPTEUtils.h"
#include "../sync/SyncSource.h"
#include "../sync/FramerateConverterSyncSource.h"
#include "../sync/EngineOffsetSplit.h"
#include "../display/CadenceStats.h"
#include "../input/HAPVideoInput.h"
#include "../input/VideoFileInput.h"
#ifdef HAVE_MTCRECEIVER
#include "../../mtcreceiver/mtcreceiver.h"  // toLocalDay (day alignment)
#endif
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <chrono>

namespace videocomposer {

LayerPlayback::LayerPlayback()
    : playing_(false)
    , currentFrame_(-1)
    , lastSyncFrame_(-1)
    , timeOffset_(0)
    , timeScale_(1.0)
    , wraparound_(false)
    , mtcFollow_(false)  // Default: don't follow MTC until engine sends /mtcfollow
    , wasRolling_(false)
    , lastLoggedFrame_(-1)
    , debugCounter_(0)
    , loggedExceededDuration_(false)
    , vsyncCount_(0)
    , lastFrameChangeVsync_(0)
    , lastVideoFrame_(-1)
    , frameOnGPU_(false)
{
}

LayerPlayback::~LayerPlayback() {
    pause();
}

void LayerPlayback::setInputSource(std::unique_ptr<InputSource> input) {
    pause();
    inputSource_ = std::move(input);
    isSharedLayer_ = false;
    isDecodeDriver_ = false;
    currentFrame_ = -1;
    lastSyncFrame_ = -1;
    liveFrames_ = 0;
    frameOnGPU_ = false;
    resetStartProbe();   // a new load: no window carries over (it re-arms on the cue's next offset)
}

void LayerPlayback::setInputSource(std::shared_ptr<InputSource> input, bool isShared, bool isDriver) {
    pause();
    inputSource_ = std::move(input);
    isSharedLayer_ = isShared;
    isDecodeDriver_ = isDriver;
    currentFrame_ = -1;
    lastSyncFrame_ = -1;
    liveFrames_ = 0;
    frameOnGPU_ = false;
    resetStartProbe();   // a new load: no window carries over (it re-arms on the cue's next offset)
}

void LayerPlayback::setSyncSource(std::unique_ptr<SyncSource> sync) {
    syncSource_ = std::move(sync);
    lastSyncFrame_ = -1;
}

void LayerPlayback::setSyncSource(std::shared_ptr<SyncSource> sync) {
    syncSource_ = std::move(sync);
    lastSyncFrame_ = -1;
}

bool LayerPlayback::play() {
    if (!isReady()) {
        return false;
    }
    playing_ = true;
    return true;
}

bool LayerPlayback::pause() {
    playing_ = false;
    return true;
}

bool LayerPlayback::seek(int64_t frameNumber) {
    if (!inputSource_) {
        return false;
    }
    
    if (inputSource_->seek(frameNumber)) {
        currentFrame_ = frameNumber;
        lastSyncFrame_ = -1;
        return true;
    }
    
    return false;
}

void LayerPlayback::update() {
    contentStarted_ = false;
    contentFrame_ = INT64_MIN;
    if (!isReady()) {
        return;
    }

    // Live sources (NDI, ...) have no timeline: show the newest frame as soon
    // as it arrives, with or without MTC, mtcfollow or a sync source. Before
    // this, a live layer only pulled frames while following a moving MTC, so
    // outside a show it stayed black. currentFrame_ counts delivered frames
    // (it is what VideoLayer watches to see that a fresh frame arrived).
    if (inputSource_->isLiveStream()) {
        if (loadFrame(-1)) {
            currentFrame_ = ++liveFrames_;
        }
        return;
    }

    // Always check sync source, even when not playing
    // This allows automatic start when MTC is received
    if (syncSource_ && syncSource_->isConnected()) {
        updateFromSyncSource();
    } else if (playing_) {
        // No sync source but playing - manual playback mode
        // (This case can be handled separately if needed)
    }
}

namespace {

int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Without an open mtcreceiver driver (none in the build, or MIDI fell back to
// another driver) nothing will ever make its head authoritative.
bool mtcReceiverActive() {
#ifdef HAVE_MTCRECEIVER
    return dayalign::mtcReceiverDrivers().load() > 0;
#else
    return false;
#endif
}

bool asReceived(long engineMs, long& localMs, long& k) {
    localMs = engineMs;
    k = 0;
    return true;
}

} // namespace

void LayerPlayback::setEngineTimeOffset(int64_t rawFrames, const std::string& tag) {
    engineOffset_.latch(rawFrames, steadyNowMs(), tag);
    // Resolved on receipt when the head is already authoritative (the normal
    // case): the offset then takes effect, and getTimeOffset() reports it,
    // exactly as before. Only a head that is not authoritative leaves it pending.
    resolveEngineOffset();
}

void LayerPlayback::resolveEngineOffset() {
    if (!engineOffset_.pending()) {
        return;
    }
    if (!mtcReceiverActive()) {
        // Fail open, as before this change, but say so once per process.
        const int64_t raw = engineOffset_.raw();
        timeOffset_ = engineOffset_.tryResolve(asReceived).frames;
        std::string line;
        if (dayalign::processLog().onAsReceived(raw, line)) {
            LOG_WARNING << line;
        }
        return;
    }
#ifdef HAVE_MTCRECEIVER
    const dayalign::EngineOffsetLatch::Outcome o = engineOffset_.tryResolve(MtcReceiver::toLocalDay);
    if (o.status == dayalign::EngineOffsetLatch::Status::Resolved) {
        // 25 fps MTC frames, day-aligned; the fps conversion in
        // updateFromSyncSource() still applies.
        timeOffset_ = o.frames;
        std::string line;
        bool warning = false;
        // mtcHead is re-read here, after toLocalDay(): the head printed in the
        // k line can differ from the one k was computed with by a few ms.
        if (dayalign::processLog().onResolved(o.k, o.engineMs, MtcReceiver::mtcHead.load(), line, warning)) {
            if (warning) {
                LOG_WARNING << line;
            } else {
                LOG_INFO << line;
            }
        }
        if (dayalign::processLog().onFrameRate(MtcReceiver::curFrameRate.load(), line)) {
            LOG_WARNING << line;
        }
        return;
    }
    int64_t ageMs = 0;
    if (engineOffset_.unresolvedWarningDue(steadyNowMs(), MtcReceiver::isTimecodeRunning.load(), ageMs)) {
        LOG_WARNING << "MTC day-align: engine offset " << engineOffset_.raw() << " (engine "
                    << o.engineMs << " ms) for layer " << engineOffset_.tag() << " unresolved for "
                    << ageMs << " ms with MTC running (MTC head not authoritative yet); the layer"
                       " does not follow MTC until it is";
    }
#endif
}

void LayerPlayback::resetStartProbe() {
    startProbeArmed_ = false;
    startProbeN_ = 0;
    startProbeSkipped_ = 0;
}

void LayerPlayback::startProbe(int64_t frame, FramerateConverterSyncSource* c, double srcFps, double fps) {
    // An engineering line, not an operator one (one start scatters by about +-6 ms with the vsync grid; it means
    // something averaged over many GOs): off unless the cadence diagnostics are on, as CadenceStats decides.
    static const bool statsOn = CadenceStats::enabledFromEnv();
    if (!statsOn) {
        return;
    }
    if (timeOffset_ != startProbeOffset_) {          // a new engine offset: a new start to measure
        startProbeOffset_ = timeOffset_;
        resetStartProbe();
        startProbeArmed_ = (timeOffset_ != 0);
    }
    if (!startProbeArmed_) {
        return;
    }
    // A shared secondary shows its driver's frame; one promoted to driver later would measure mid-cue. A time scale,
    // no converter or no rates: nothing to measure against. All disarm until the next offset.
    if ((isSharedLayer_ && !isDecodeDriver_) || timeScale_ != 1.0 || !c || srcFps <= 0.0 || fps <= 0.0) {
        resetStartProbe();
        return;
    }
    if (frame < 0) {
        return;                                      // before the cue's start (preload): wait
    }
    if (inputSource_) {
        const int64_t total = inputSource_->getFrameInfo().totalFrames;
        if (total > 0 && frame >= total) {           // past the media's end: the layer holds its last frame
            resetStartProbe();
            return;
        }
    }
    if (!c->lastClockAtPresentation()) {
        ++startProbeSkipped_;                        // legacy path or no clock this poll: another reference, skipped
        return;
    }
    const double clk = c->lastClockMs();
    const double frameMs = 1000.0 / fps;
    if (startProbeN_ > 0 && clk - startProbeLastClock_ > 100.0) {
        resetStartProbe();                           // a pause inside the window (follow off, MTC gap): no longer a start
        return;
    }
    const double offMs = static_cast<double>(timeOffset_) * 1000.0 / srcFps;
    const double behind = clk + offMs - static_cast<double>(frame) * frameMs;
    if (startProbeN_ == 0) {
        startProbeFirst_ = frame;
        startProbeClock0_ = clk;
        startProbeSum_ = 0.0;
        startProbeMin_ = startProbeMax_ = behind;
    }
    startProbeLastClock_ = clk;
    startProbeSum_ += behind;
    startProbeMin_ = std::min(startProbeMin_, behind);
    startProbeMax_ = std::max(startProbeMax_, behind);
    if (++startProbeN_ == 60) {
        char line[360];
        std::snprintf(line, sizeof line,
                      "start-probe: layer %s fps %.3f offset %lld MTC frames (%.1f ms), first frame %lld at clock %.3f ms;"
                      " chosen frame behind the cue's start over 60 vsyncs: mean %.2f min %.2f max %.2f ms"
                      " (frame %.2f ms; %d legacy-path polls skipped)",
                      engineOffset_.tag().c_str(), fps, static_cast<long long>(timeOffset_), offMs,
                      static_cast<long long>(startProbeFirst_), startProbeClock0_, startProbeSum_ / 60.0,
                      startProbeMin_, startProbeMax_, frameMs, startProbeSkipped_);
        LOG_INFO << line;
        resetStartProbe();
    }
}

void LayerPlayback::updateFromSyncSource() {
    if (!syncSource_ || !syncSource_->isConnected()) {
        // No sync source - manual playback or paused
        return;
    }

    // An engine offset is never used raw: it waits for the MTC head to be
    // authoritative and is then converted into this process's MTC day.
    resolveEngineOffset();
    
    // Check if MTC following is disabled for this layer
    if (!mtcFollow_) {
        // Layer ignores sync source - manual control only
        return;
    }

    // NOTE: These were previously static variables shared across ALL layers (bug!)
    // Now they are instance variables, each layer has its own state

    // The engine offset in content frames, exactly (1amfwm7h924): whole frames added below, the sub-frame
    // remainder handed to the converter as a phase of its clock before it polls. Only the layer that polls sets it
    // (a shared secondary reads the driver's frame, and carries the same cue offset). A time scale other than 1
    // keeps the old whole-frame offset with no phase: the phase is in clock time, the scale is not.
    auto* converter = dynamic_cast<FramerateConverterSyncSource*>(syncSource_.get());
    EngineOffsetSplit offsetSplit;
    offsetSplit.frames = timeOffset_;
    if (converter) {
        if (timeOffset_ != 0) {
            offsetSplit = splitEngineOffset(timeOffset_, converter->getSourceFramerate(), converter->getFramerate());
        }
        if (timeScale_ != 1.0) {
            offsetSplit.phaseMs = 0.0;
        }
        if (!(isSharedLayer_ && !isDecodeDriver_)) {
            converter->setPhaseMs(offsetSplit.phaseMs);
            phaseMismatchPolls_ = 0;
        } else if (std::fabs(offsetSplit.phaseMs - converter->getPhaseMs()) > 1e-6) {
            // A secondary shows the driver's frame, so the driver's phase. The engine sends one offset to every
            // layer of a cue (run_cue.py), so a difference is only the moment between two of those sends; one that
            // lasts means this layer can be up to one frame off its own offset. Said once, after ~0.5 s.
            if (++phaseMismatchPolls_ == 30) {
                LOG_WARNING << "offset phase: shared layer " << engineOffset_.tag() << " has offset " << timeOffset_
                            << " (phase " << offsetSplit.phaseMs << " ms) but its decode driver's phase is "
                            << converter->getPhaseMs() << " ms: up to one content frame off its own offset";
            }
        } else {
            phaseMismatchPolls_ = 0;
        }
    }

    uint8_t rolling = 0;
    int64_t syncFrame;
    if (isSharedLayer_ && !isDecodeDriver_) {
        // Secondary shared layer: read cached smoothed frame from driver's last pollFrame()
        syncFrame = syncSource_->getCurrentFrame();
        // Read cached rolling state from driver's last pollFrame()
        if (converter) {
            rolling = converter->getCurrentRolling() ? 1 : 0;
        }
    } else {
        // Driver or non-shared layer: advance the cadence smoother
        syncFrame = syncSource_->pollFrame(&rolling);
    }

    // Engine offset not placed yet (MTC head not authoritative): fail closed,
    // the layer loads no frame, as before its cue starts. The poll above still
    // runs exactly as when following: a driver's pollFrame() advances the
    // cadence smoother its shared secondaries read, once per vsync. Skipping
    // the frame-0 fallback further down is intentional too (fail closed).
    if (engineOffset_.pending()) {
        return;
    }
    
    // Debug: log frame and rolling state periodically
    debugCounter_++;
    if (debugCounter_ % 60 == 0) {  // Log every 60 calls (~1 second at 60fps)
        LOG_VERBOSE << "MTC poll: syncFrame=" << syncFrame << ", rolling=" << (int)rolling;
    }
    
    // Log MTC status changes
    if (rolling != 0 && !wasRolling_) {
        // MTC started rolling
        LOG_INFO << "MTC: Started rolling - playback starting (frame=" << syncFrame << ")";
        wasRolling_ = true;
    } else if (rolling == 0 && wasRolling_) {
        // MTC stopped rolling
        LOG_INFO << "MTC: Stopped rolling - playback paused";
        wasRolling_ = false;
    }
    
    // Also log when we have a frame but not rolling (for debugging)
    if (syncFrame >= 0 && rolling == 0 && debugCounter_ % 300 == 0) {
        LOG_VERBOSE << "MTC: Has frame " << syncFrame << " but not rolling (waiting for MTC to start)";
    }
    
    // Automatically start playing when MTC is rolling (rolling != 0)
    // OR when we have valid frame data (MTC is providing timecode)
    // This ensures playback starts automatically when MTC is received on startup
    // even if the rolling detection is temporarily false due to timing
    if ((rolling != 0 || syncFrame >= 0) && !playing_) {
        // Start playing if MTC is rolling OR we have valid frame data
        // This handles cases where rolling detection is temporarily false
        // but we're still receiving valid MTC timecode
        playing_ = true;
        if (syncFrame >= 0 && rolling == 0) {
            LOG_VERBOSE << "MTC: Starting playback with frame " << syncFrame << " (rolling detection pending)";
        }
    }
    
    // Update playing state based on rolling
    // Only pause if we truly have no MTC data (not just rolling=false)
    // If rolling stops but we still have frame data, keep playing
    // (MTC might be paused but still providing position)
    if (rolling == 0 && syncFrame < 0 && playing_) {
        // No MTC data at all - pause playback
        playing_ = false;
    }
    
    // Log MTC timecode periodically (every 30 frames or when frame changes significantly).
    // Both branches at DEBUG: with MTC rolling this fired ~13k times/day into the
    // journal at the default level — steady-state position is diagnostics, not
    // operations. Raise the level when actually chasing sync.
    if (syncFrame >= 0 && rolling != 0) {
        if (lastLoggedFrame_ < 0 || std::abs(syncFrame - lastLoggedFrame_) >= 30) {
            // Format timecode for display
            FrameInfo info = getFrameInfo();
            if (info.framerate > 0.0) {
                std::string smpte = SMPTEUtils::frameToSmpteString(syncFrame, info.framerate);
                LOG_DEBUG << "MTC: " << smpte << " (frame " << syncFrame << ", rolling)";
            } else {
                LOG_DEBUG << "MTC: frame " << syncFrame << " (rolling)";
            }
            lastLoggedFrame_ = syncFrame;
        }
    } else if (syncFrame >= 0 && rolling == 0) {
        // Log when we have a frame but not rolling (stopped). Same >=30 frame
        // throttle as the rolling branch — the bare frame-changed gate fired
        // once per MTC position update.
        if (lastLoggedFrame_ < 0 || std::abs(syncFrame - lastLoggedFrame_) >= 30) {
            FrameInfo info = getFrameInfo();
            if (info.framerate > 0.0) {
                std::string smpte = SMPTEUtils::frameToSmpteString(syncFrame, info.framerate);
                LOG_DEBUG << "MTC: " << smpte << " (frame " << syncFrame << ", stopped)";
            } else {
                LOG_DEBUG << "MTC: frame " << syncFrame << " (stopped)";
            }
            lastLoggedFrame_ = syncFrame;
        }
    }
    
    // Process frame updates only if we have a valid frame
    if (syncFrame >= 0) {
        // Apply time-scaling: multiply by timescale, then add offset.
        // syncFrame is already in video fps (converted by FramerateConverterSyncSource, with the offset's phase).
        // timeOffset_ arrives from the engine in MTC fps (always 25fps): its whole content frames, computed above
        // (EngineOffsetSplit.h; it used to be floored on its own, which made non-25p start late, 1amfwm7h924).
        int64_t adjustedFrame = static_cast<int64_t>(std::floor(static_cast<double>(syncFrame) * timeScale_)) + offsetSplit.frames;
        startProbe(adjustedFrame, converter, converter ? converter->getSourceFramerate() : 0.0,
                   converter ? converter->getFramerate() : 0.0);
        contentFrame_ = adjustedFrame;  // raw, for the visible_at gate (869feynrj)

        // The first position after an engine offset was placed: more than an
        // hour off the media means a wrong MTC day or an engine time far from now.
        if (engineOffset_.takePositionCheck() && inputSource_) {
            FrameInfo info = inputSource_->getFrameInfo();
            if (dayalign::implausiblePosition(adjustedFrame, info.totalFrames, info.framerate)) {
                LOG_WARNING << "MTC day-align: implausible position " << adjustedFrame << " (media "
                            << info.totalFrames << " frames) for layer " << engineOffset_.tag()
                            << " right after an engine offset (engine " << engineOffset_.lastEngineMs()
                            << " ms, k=" << engineOffset_.lastK()
                            << ") -- wrong MTC day, or an engine time far from now";
            }
        }
        
        // A looping cue with the engine's period wraps on it (869ejxng3);
        // without one, the totalFrames wrap/clamp below is unchanged. The tail
        // blank is decided afresh on every position (D15).
        tailBlank_ = false;
        const bool periodApplied = wraparound_ && loopPeriod_.valid() && inputSource_ &&
                                   applyLoopPeriod(adjustedFrame, syncFrame);

        // Clamp frame to valid range (no automatic wrapping - use loop instead)
        if (inputSource_ && !periodApplied) {
            FrameInfo info = inputSource_->getFrameInfo();
            int64_t totalFrames = info.totalFrames;
            
            if (totalFrames > 0) {
                if (adjustedFrame >= totalFrames) {
                    if (wraparound_) {
                        // Loop mode: wrap via modulo so the decode queue pre-buffers
                        // frame 0 seamlessly before the engine offset command arrives.
                        adjustedFrame = adjustedFrame % totalFrames;
                    } else {
                        // Non-loop mode: hold the last frame until MTC stops.
                        adjustedFrame = totalFrames - 1;
                    }
                } else if (adjustedFrame < 0) {
                    if (wraparound_) {
                        adjustedFrame = totalFrames - ((-adjustedFrame) % totalFrames);
                        if (adjustedFrame == totalFrames) adjustedFrame = 0;
                    } else {
                        adjustedFrame = 0;
                    }
                }
                loggedExceededDuration_ = (adjustedFrame == 0);
            }
        }
        
        // Check if a full frame SYSEX was received (indicates explicit position command)
        // Full frames require immediate seek/update regardless of frame number change
        // This works with any SyncSource (including FramerateConverterSyncSource wrapper)
        bool fullFrameReceived = syncSource_->wasFullFrameReceived();
        if (fullFrameReceived) {
            LOG_INFO << "MTC: Full frame SYSEX received - forcing seek to frame " << adjustedFrame;
        }
        
        // Match xjadeo's approach: always try to load the frame if it changed
        // xjadeo calls display_frame() every loop iteration, but only processes if frame changed
        // We do the same: poll MTC every update, but only load if frame changed
        // The seek logic inside loadFrame/readFrame will handle optimization
        // (no seek for consecutive frames, etc.)
        // However, full SYSEX frames are explicit position commands and must always seek
        // even if the frame number is the same (e.g., 00:00:00:00 to reset position)
        if (adjustedFrame != lastSyncFrame_ || fullFrameReceived) {
            vsyncCount_++;
            
            if (fullFrameReceived && !(isSharedLayer_ && !isDecodeDriver_)) {
                // Full frame received - force a seek first, then load
                // (Shared secondaries skip: seeking would corrupt driver's decoder)
                LOG_VERBOSE << "MTC: Full frame - forcing seek to frame " << adjustedFrame;
                if (inputSource_) {
                    inputSource_->resetSeekState();
                }
                if (inputSource_ && inputSource_->seek(adjustedFrame)) {
                    if (loadFrame(adjustedFrame)) {
                        currentFrame_ = adjustedFrame;
                        lastSyncFrame_ = adjustedFrame;
                    } else {
                        LOG_WARNING << "Failed to load frame " << adjustedFrame << " after full frame seek";
                    }
                } else {
                    LOG_WARNING << "Failed to seek to frame " << adjustedFrame << " for full frame update";
                }
            } else if (loadFrame(adjustedFrame)) {
                // Normal update - loadFrame() handles seek optimization internally
                // (no seek for consecutive frames, seeks for backwards/non-consecutive)
                currentFrame_ = adjustedFrame;
                lastSyncFrame_ = adjustedFrame;
                
                // Log frame display duration (vsyncs since last frame change)
                int64_t vsyncsSinceLast = vsyncCount_ - lastFrameChangeVsync_;
                if (lastVideoFrame_ >= 0 && vsyncsSinceLast > 4) {
                    LOG_WARNING << "Frame pacing: frame " << lastVideoFrame_ << " held for " 
                                << vsyncsSinceLast << " vsyncs (possible stall)";
                    signals::frameHeldLong();
                }
                lastFrameChangeVsync_ = vsyncCount_;
                lastVideoFrame_ = adjustedFrame;
            } else if (isSharedLayer_ && !isDecodeDriver_) {
                // Shared secondary: cache not ready yet — skip seek (would corrupt driver's decoder)
                signals::frameMissed();
            } else if (isHardwareDecodeLayer()) {
                // Hardware layer: a failed load is a decode-queue miss, not a
                // lost position. Seeking would flush the queue and restart the
                // decoder - turning a one-vsync miss into a real stall, on every
                // miss. Hold and retry next vsync.
                //
                // This branch is the saturation signal that matters. The pacing
                // warning above only fires when a frame *did* arrive, so a
                // wedged layer never reaches it and would report nothing at all;
                // a frame wanted and not delivered is counted here instead. It
                // is also GPU-agnostic, which is what makes the warnings work
                // on the Intel nodes, where decode occupancy is never reported.
                signals::frameMissed();
            } else {
                // If load fails, try seeking first (helps with keyframe-based codecs)
                LOG_WARNING << "Failed to load frame " << adjustedFrame << ", trying seek first";
                if (inputSource_ && inputSource_->seek(adjustedFrame)) {
                    if (loadFrame(adjustedFrame)) {
                        currentFrame_ = adjustedFrame;
                        lastSyncFrame_ = adjustedFrame;
                    } else {
                        LOG_WARNING << "Failed to load frame " << adjustedFrame << " even after seek";
                    }
                } else {
                    LOG_WARNING << "Failed to seek to frame " << adjustedFrame;
                }
            }
        }
        // ...and its pixels are in the buffer: a decode miss may report success holding an older frame (X1)
        contentStarted_ = contentFrame_ >= 0 && currentFrame_ == adjustedFrame &&
                          inputSource_->deliveredFrame(adjustedFrame) == adjustedFrame &&
                          (!(isSharedLayer_ && !isDecodeDriver_) || inputSource_->getCachedFrameNumber() == adjustedFrame);
    } else {
        // No valid sync frame yet (MTC not received or not rolling)
        // Show frame 0 as fallback so video is visible while waiting for MTC
        // This matches original xjadeo behavior when newFrame < 0 (uses userFrame)
        if (inputSource_ && currentFrame_ < 0) {
            // Only load frame 0 once if we haven't loaded any frame yet
            // This ensures video is visible even when waiting for MTC
            if (loadFrame(0)) {
                currentFrame_ = 0;
                LOG_INFO << "Loaded frame 0 as fallback (waiting for MTC)";
                // Keep lastSyncFrame_ as -1 so we reload when MTC arrives
            } else {
                LOG_WARNING << "Failed to load frame 0 as fallback";
            }
        }
    }
}

void LayerPlayback::setLoopPeriod(int64_t num, int64_t den, int64_t frames, const std::string& tag) {
    loopperiod::Period p;
    p.num = num;
    p.den = den;
    p.realFrames = frames > 0 ? frames : 0;
    if (!p.valid()) {
        if (loopPeriod_.valid()) {
            LOG_INFO << "loop period cleared for layer " << tag;
        }
        clearLoopPeriod();
        return;
    }
    loopTag_ = tag;
    if (p == loopPeriod_) {
        return;   // the per-pass re-send
    }
    loopPeriod_ = p;
    loopLogged_ = false;
    loopMismatchLogged_ = false;
    loopCutLogged_ = false;
    loopNoFramesLogged_ = false;
    loopEndLogged_ = false;
    loopLastPass_ = -2;
    tailBlank_ = false;
}

void LayerPlayback::clearLoopPeriod() {
    loopPeriod_ = loopperiod::Period();
    loopLogged_ = false;
    loopMismatchLogged_ = false;
    loopCutLogged_ = false;
    loopNoFramesLogged_ = false;
    loopEndLogged_ = false;
    loopLastPass_ = -2;
    tailBlank_ = false;
}

bool LayerPlayback::applyLoopPeriod(int64_t& adjustedFrame, int64_t syncFrame) {
    loopperiod::Rate rate;
    if (!inputSource_->getFrameRateRational(rate.num, rate.den)) {
        // No rational rate (not a file input): the double, on a 1001 base.
        const double fps = inputSource_->getFrameInfo().framerate;
        rate.num = static_cast<int64_t>(std::llround(fps * 1001.0));
        rate.den = 1001;
    }
    const char* rule = "none";
    const int64_t own = inputSource_->realFrames(&rule);
    const int64_t frames = loopPeriod_.realFrames > 0 ? loopPeriod_.realFrames : own;
    if (!rate.valid() || frames <= 0) {
        if (!loopNoFramesLogged_) {
            LOG_WARNING << "loop period for layer " << loopTag_ << " not applied: "
                        << (rate.valid() ? "no frame count" : "no frame rate")
                        << "; wrapping on the file's length";
            loopNoFramesLogged_ = true;
        }
        return false;
    }
    const loopperiod::TailKind kind = loopperiod::tailKind(loopPeriod_, rate, frames);
    const double tailMs = loopperiod::tailMs(loopPeriod_, rate, frames);
    if (!loopLogged_) {
        LOG_INFO << "loop period for layer " << loopTag_ << ": " << loopPeriod_.num << "/"
                 << loopPeriod_.den << " s = " << loopperiod::pfFrames(loopPeriod_, rate)
                 << " frames at " << rate.num << "/" << rate.den << " fps; frames engine "
                 << (loopPeriod_.realFrames > 0 ? std::to_string(loopPeriod_.realFrames) : std::string("-"))
                 << ", own " << own << " (" << rule << "); tail " << tailMs << " ms, "
                 << loopperiod::tailKindName(kind) << "; passes " << loopPasses_;
        loopLogged_ = true;
    }
    if (loopPeriod_.realFrames > 0 && own >= 0 && std::llabs(own - loopPeriod_.realFrames) > 2 &&
        !loopMismatchLogged_) {
        LOG_ERROR << "loop frame count for layer " << loopTag_ << ": engine " << loopPeriod_.realFrames
                  << ", own " << own << " (" << rule << "), more than 2 frames apart; the engine's rules";
        loopMismatchLogged_ = true;
    }
    // A period shorter than the video: within 0.5 s a same-file pair loops on
    // its audio length and the video is cut there (R23, INFO); beyond 0.5 s it
    // cannot come from the engine's rule (a bad sidecar or the test knob):
    // still applied, never dropped (H9, I4), ERROR.
    if (!loopCutLogged_ && kind == loopperiod::TailKind::Cut) {
        LOG_INFO << "loop period shorter than the video by " << -tailMs
                 << " ms, cut at the loop point (layer " << loopTag_ << ")";
        loopCutLogged_ = true;
    } else if (!loopCutLogged_ && kind == loopperiod::TailKind::CutBeyond) {
        LOG_ERROR << "loop period shorter than the video by " << -tailMs
                  << " ms, cutting at the period (layer " << loopTag_ << ")";
        loopCutLogged_ = true;
    }

    loopperiod::Shown shown = loopperiod::shownFrame(adjustedFrame, loopPeriod_, rate, frames, loopPasses_);
    // The decode queue's own bound: never ask past the container's length.
    const int64_t totalFrames = inputSource_->getFrameInfo().totalFrames;
    if (totalFrames > 0 && shown.frame > totalFrames - 1) {
        shown.frame = totalFrames - 1;
    }

    if (shown.pass > loopLastPass_ && shown.pass >= 1 && loopLastPass_ >= 0 && !shown.ended) {
        long mtcFrame = static_cast<long>(syncFrame);
#ifdef HAVE_MTCRECEIVER
        mtcFrame = MtcReceiver::mtcHead.load() / 40;
#endif
        const bool tailed = kind == loopperiod::TailKind::Held || kind == loopperiod::TailKind::NotDrawn;
        if (shown.pass <= 3) {
            LOG_INFO << "loop pass " << shown.pass + 1 << " of layer " << loopTag_ << " at MTC frame "
                     << mtcFrame << " (position " << adjustedFrame << ", shows " << shown.frame << ")"
                     << (tailed ? ", tail " + std::to_string(std::llround(tailMs)) + " ms, " +
                                      loopperiod::tailKindName(kind)
                                : std::string());
        } else {
            LOG_DEBUG << "loop pass " << shown.pass + 1 << " of layer " << loopTag_ << " at MTC frame "
                      << mtcFrame << " (position " << adjustedFrame << ", shows " << shown.frame << ")";
        }
    }
    if (shown.ended && !loopEndLogged_) {
        if (shown.blank) {
            LOG_INFO << "loop of layer " << loopTag_ << " ended after " << loopPasses_
                     << " passes, after a tail not drawn: nothing drawn (R2)";
        } else {
            LOG_INFO << "loop of layer " << loopTag_ << " ended after " << loopPasses_
                     << " passes; holding frame " << shown.frame;
        }
        loopEndLogged_ = true;
    }
    loopLastPass_ = shown.pass;
    tailBlank_ = shown.blank;
    adjustedFrame = shown.frame;
    return true;
}

bool LayerPlayback::isHardwareDecodeLayer() const {
    VideoFileInput* videoInput = dynamic_cast<VideoFileInput*>(inputSource_.get());
    return videoInput &&
           videoInput->getOptimalBackend() == InputSource::DecodeBackend::GPU_HARDWARE;
}

bool LayerPlayback::loadFrame(int64_t frameNumber) {
    if (!inputSource_ || !inputSource_->isReady()) {
        return false;
    }

    // --- Secondary shared layer: read from driver cache ---
    if (isSharedLayer_ && !isDecodeDriver_) {
        return copyFromDriverCache(frameNumber);
    }

    // Check if this is a live stream (NDI, V4L2, RTSP, etc.)
    if (inputSource_->isLiveStream()) {
        // Live streams: get latest available frame (ignore frameNumber)
        // The async buffer in LiveInputSource keeps frames ready.
        // Never wait here: this runs on the render thread once per loop, and
        // a 25 fps source would otherwise stall every output of a 60 Hz loop.
        // No new frame = keep showing the current one.
        if (inputSource_->readLatestFrame(cpuFrameBuffer_, 0)) {
            frameOnGPU_ = false;
            if (isDecodeDriver_) inputSource_->setCachedFrame(-1, cpuFrameBuffer_);
            return true;
        }
        return false;
    }

    // Check if this is a HAP codec
    HAPVideoInput* hapInput = dynamic_cast<HAPVideoInput*>(inputSource_.get());
    if (hapInput) {
#ifdef ENABLE_HAP_DIRECT
        // HAP with direct texture upload: decode directly to compressed DXT GPU texture
        if (hapInput->readFrameToTexture(frameNumber, gpuFrameBuffer_)) {
            frameOnGPU_ = true;
            if (isDecodeDriver_) inputSource_->setCachedFrame(frameNumber, gpuFrameBuffer_);
            return true;
        }
        // If direct upload fails, fall through to FFmpeg fallback (handled in readFrameToTexture)
        LOG_WARNING << "HAP direct decode failed for frame " << frameNumber;
        return false;
#else
        // HAP without direct upload: decode to CPU buffer as RGBA (FFmpeg fallback)
        if (hapInput->readFrame(frameNumber, cpuFrameBuffer_)) {
            frameOnGPU_ = false;
            if (isDecodeDriver_) inputSource_->setCachedFrame(frameNumber, cpuFrameBuffer_);
            return true;
        }
        return false;
#endif
    }

    bool success = false;
    static bool loggedBackend = false;  // Log decode backend once

    // Check if this is VideoFileInput with hardware decoding
    VideoFileInput* videoInput = dynamic_cast<VideoFileInput*>(inputSource_.get());
    if (videoInput) {
        // Check if hardware decoding is available and should be used
        InputSource::DecodeBackend backend = videoInput->getOptimalBackend();

        // Log the decode backend once at startup
        if (!loggedBackend) {
            if (backend == InputSource::DecodeBackend::GPU_HARDWARE) {
                LOG_INFO << "Decode path: GPU_HARDWARE (VAAPI zero-copy)";
            } else {
                LOG_INFO << "Decode path: CPU_SOFTWARE";
            }
            loggedBackend = true;
        }

        if (backend == InputSource::DecodeBackend::GPU_HARDWARE) {
            // Hardware decoding: decode directly to GPU texture
            if (videoInput->readFrameToTexture(frameNumber, gpuFrameBuffer_)) {
                frameOnGPU_ = true;
                success = true;
            } else {
                // A false here is NOT "hardware is broken, try software". On the
                // hardware path it means the decode queue had no frame this
                // vsync and there was no texture to hold - a transient miss,
                // normal during cold start. Falling back to readFrame() would
                // spin up a second, software decoder on the same file for a
                // frame the queue is about to deliver anyway, on the render
                // thread, every vsync it misses.
                //
                // Report nothing this vsync and let the next one retry.
                if (++hwMissLogCount_ % 120 == 1) {
                    LOG_WARNING << "Hardware decode returned no frame " << frameNumber
                                << " (queue miss); holding";
                }
            }
        } else if (videoInput->readFrame(frameNumber, cpuFrameBuffer_)) {
            // Software decoding: use CPU frame buffer
            frameOnGPU_ = false;
            success = true;
        }
    } else {
        // Other input sources: use CPU frame buffer (default)
        if (inputSource_->readFrame(frameNumber, cpuFrameBuffer_)) {
            frameOnGPU_ = false;
            success = true;
        }
    }

    // Populate cache for shared secondaries (all non-HAP, non-live paths)
    if (success && isDecodeDriver_) {
        if (frameOnGPU_)
            inputSource_->setCachedFrame(frameNumber, gpuFrameBuffer_);
        else
            inputSource_->setCachedFrame(frameNumber, cpuFrameBuffer_);
    }

    return success;
}

bool LayerPlayback::copyFromDriverCache(int64_t frameNumber) {
    if (!inputSource_->hasCachedFrame()) return false;  // Driver hasn't decoded anything yet

    int64_t cachedNum = inputSource_->getCachedFrameNumber();
    // For live sources cachedNum == -1 (no meaningful frame number) — don't warn
    if (cachedNum >= 0 && cachedNum != frameNumber) {
        LOG_DEBUG << "Shared layer frame mismatch: requested " << frameNumber
                  << ", driver cached " << cachedNum << " (timeOffset divergence?)";
    }

    if (inputSource_->isCachedOnGPU()) {
        auto* cached = inputSource_->getCachedGPUTexture();
        if (!cached) return false;
        gpuFrameBuffer_ = *cached;  // non-owning copy (ownsTexture_=false via copy ctor)
        frameOnGPU_ = true;
    } else {
        auto* cached = inputSource_->getCachedCPUFrame();
        if (!cached) return false;
        cpuFrameBuffer_.copyFrom(*cached);  // non-owning shallow copy (pointer + metadata, NOT pixels)
        frameOnGPU_ = false;
    }
    return true;
}

bool LayerPlayback::getFrameBuffer(const FrameBuffer*& cpuBuffer, const GPUTextureFrameBuffer*& gpuBuffer) const {
    if (frameOnGPU_) {
        // Frame is on GPU - return pointer to GPU buffer
        gpuBuffer = &gpuFrameBuffer_;
        cpuBuffer = nullptr;
        return true; // true = on GPU
    } else {
        // Frame is on CPU - return pointer to CPU buffer
        cpuBuffer = &cpuFrameBuffer_;
        gpuBuffer = nullptr;
        return false; // false = on CPU
    }
}

bool LayerPlayback::isHAPCodec() const {
    if (!inputSource_) {
        return false;
    }
    
    InputSource::CodecType codec = inputSource_->detectCodec();
    return codec == InputSource::CodecType::HAP ||
           codec == InputSource::CodecType::HAP_Q ||
           codec == InputSource::CodecType::HAP_ALPHA;
}

bool LayerPlayback::isReady() const {
    return inputSource_ != nullptr && inputSource_->isReady();
}

FrameInfo LayerPlayback::getFrameInfo() const {
    if (inputSource_) {
        return inputSource_->getFrameInfo();
    }
    return FrameInfo();
}

void LayerPlayback::reverse() {
    // Reverse playback: multiply timescale by -1.0 and adjust offset
    // to keep current frame displayed
    if (currentFrame_ >= 0) {
        // Calculate new offset to maintain current frame position
        // newFrame = (syncFrame * -timeScale) + newOffset
        // We want: currentFrame = (syncFrame * -timeScale) + newOffset
        // So: newOffset = currentFrame - (syncFrame * -timeScale)
        // But we don't have syncFrame here, so we use a simpler approach:
        // Set offset to currentFrame and negate timescale
        setTimeOffset(currentFrame_);
        timeScale_ = -timeScale_;
    } else {
        // Just negate timescale if no current frame
        timeScale_ = -timeScale_;
    }
}

bool LayerPlayback::checkPlaybackEnd() const {
    if (!inputSource_ || !inputSource_->isReady()) {
        return false;
    }
    
    if (currentFrame_ < 0) {
        return false; // No frame loaded yet
    }
    
    FrameInfo info = inputSource_->getFrameInfo();
    int64_t totalFrames = info.totalFrames;
    
    if (totalFrames <= 0) {
        return false; // Unknown duration
    }
    
    // Check if current frame is at or beyond the end
    return currentFrame_ >= totalFrames;
}

} // namespace videocomposer

