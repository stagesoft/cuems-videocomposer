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

#ifndef VIDEOCOMPOSER_FRAMERATECONVERTERSYNCSOURCE_H
#define VIDEOCOMPOSER_FRAMERATECONVERTERSYNCSOURCE_H

#include <string>
#include <utility>
#include <vector>
#include <atomic>
#include "SyncSource.h"
#include "../input/InputSource.h"

namespace videocomposer {

/**
 * FramerateConverterSyncSource - Wraps any SyncSource and converts framerate
 * 
 * This adapter converts frames from the sync source's framerate to the input
 * source's framerate using Option 2 (resample): videoFrame = rint(syncFrame * inputFps / syncFps)
 * 
 * This is timecode-agnostic and works with:
 * - Any sync source (MIDI, LTC, JACK, etc.)
 * - Any input source (video files, live feeds, streams, etc.)
 * 
 * The conversion is applied automatically if both framerates are known and different.
 */
class FramerateConverterSyncSource : public SyncSource {
public:
    /**
     * Create a framerate converter that wraps a sync source (non-owning reference)
     * @param syncSource The sync source to wrap (keeps reference, does not take ownership)
     * @param inputSource The input source to get target framerate from (keeps reference)
     */
    FramerateConverterSyncSource(SyncSource* syncSource, InputSource* inputSource);
    
    virtual ~FramerateConverterSyncSource() = default;

    // SyncSource interface - delegates to wrapped sync source
    bool connect(const char* param = nullptr) override;
    void disconnect() override;
    bool isConnected() const override;
    int64_t pollFrame(uint8_t* rolling = nullptr) override;
    int64_t getCurrentFrame() const override;
    const char* getName() const override;
    double getFramerate() const override;

    /**
     * Update the input source reference (e.g., when input source changes)
     * @param inputSource New input source (can be nullptr)
     */
    void setInputSource(InputSource* inputSource) { inputSource_ = inputSource; }
    
    /**
     * Get the underlying sync source's framerate (e.g. MTC 25fps) before conversion.
     * Returns <= 0 if unknown.
     */
    double getSourceFramerate() const;

    /**
     * Check if a full frame SYSEX was just received - delegates to wrapped sync source
     */
    bool wasFullFrameReceived() override;

    /**
     * Delegate to wrapped sync source's getTimeMs()
     */
    long getTimeMs() const override;

    /**
     * Delegate to wrapped sync source's getDisplayLatencyMs()
     */
    long getDisplayLatencyMs() const override;

    /**
     * Get the rolling state from the last pollFrame() call.
     * Used by shared secondary layers to read the driver's rolling state
     * without calling pollFrame() themselves.
     */
    bool getCurrentRolling() const { return lastRolling_; }

    /// Cadence engine counters (plan 1amfwm7gpz7 §5.3, the lag line); tickPath = the vblank-tick engine chose the
    /// last frame (false = the legacy smoother).
    struct CadenceCounters {
        int64_t corrMinus = 0, corrPlus = 0, skips = 0, drops = 0, stalls = 0, reanchors = 0;
        double phiMax = 0.0;
        bool tickPath = false;
    };
    CadenceCounters getCadenceCounters() const { return cadenceCounters_; }
    /// Test seam: keep every cadence event (stall, corr-, corr+, skip, anchor, reanchor) with its vblank.
    void setCadenceEventCapture(bool on) { cadenceCapture_ = on; cadenceEvents_.clear(); }
    const std::vector<std::pair<int64_t, std::string>>& cadenceEvents() const { return cadenceEvents_; }

private:
    SyncSource* wrappedSyncSource_;  // Non-owning reference to sync source
    InputSource* inputSource_;  // Non-owning reference

    // Per-instance cadence smoother state (was incorrectly static — shared across all layers)
    int64_t   cadenceDisplayFrame_  = -1;
    int       cadenceVsyncCount_    =  0;
    double    cadenceVsyncPeriodMs_ = 16.667;  // initial guess
    long long cadenceLastCallUs_    =  0;

    CadenceCounters cadenceCounters_;
    bool cadenceCapture_ = false;

    // ---- the vblank-tick engine (plan 1amfwm7gpz7 §5.2; model: cuems-RELATIONS baselines/test3-saltitos-20261104/model/engine.py)
    int64_t tickPathPoll(double inputFps);          // < 0: the tick path does not apply (legacy smoother)
    void cadenceAnchor(double pos, int64_t msc);
    int cadenceNextHold();
    int cadenceTakeCorrection(int n, int64_t msc, bool& skip);
    void cadenceEvent(int64_t msc, const char* kind);
    void cadenceLagLine(int64_t msc, double inputFps);
    bool    anchored_ = false;
    bool    needReanchor_ = false;
    double  lastPos_ = 0.0;
    bool    havePos_ = false;
    int64_t D_ = 0;                 // the displayed content frame (absolute MTC position in content frames)
    int64_t mD_ = 0;                // the presentation vblank at which D_ was admitted (the schedule grid)
    int     nD_ = 1;                // scheduled hold of D_ in vsyncs (0 = a dropped frame, drop regime only)
    double  err_ = 0.0;             // Bresenham carry of the next frame (ideal − grid admission, vsyncs)
    double  errCur_ = 0.0;          // carry of the current frame D_: what φ uses
    double  r_ = 0.0;               // (1/fps)/period
    int     integerN_ = 0;
    bool    drop_ = false;
    double  ratioFps_ = 0.0;        // the fps/period the ratio was computed for (a change re-anchors)
    int64_t ratioPeriodNs_ = 0;
    int64_t lastMsc_ = -1;          // the engine's vblank (presentation target) of the previous poll
    int64_t lastTickMsc_ = -1;
    int64_t lastNowMsc_ = -1;
    bool    hasCorr_ = false;
    int64_t lastCorrMsc_ = 0;
    int     pendingCorrection_ = 0;
    int64_t inFlightUntil_ = -1;
    double  phiRing_[16] = {0.0};
    int     phiRingN_ = 0;
    int     phiRingHead_ = 0;
    int     invalidTicks_ = 0;
    int     frozenPolls_ = 0;
    bool    frozenWarned_ = false;
    bool    pathLoggedTick_ = false;
    bool    cadenceLineLogged_ = false;
    int64_t lagLineMsc_ = -1;
    double  lagPos0_ = 0.0;
    int64_t lagMsc0_ = -1;
    int     eventsThisSecond_ = 0;
    // a stable id per converter (one per layer and cue) for the event and lag lines: "layer=c<id>"
    static inline std::atomic<int> nextCadenceId_{0};
    const int cadenceId_ = ++nextCadenceId_;
    int64_t eventsSecondMsc_ = -1;
    std::vector<std::pair<int64_t, std::string>> cadenceEvents_;

    // Cached results from last pollFrame() — used by shared secondary layers
    int64_t lastSmoothedFrame_ = -1;  // last frame returned by pollFrame()
    bool    lastRolling_ = false;     // last rolling state from pollFrame()
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_FRAMERATECONVERTERSYNCSOURCE_H

