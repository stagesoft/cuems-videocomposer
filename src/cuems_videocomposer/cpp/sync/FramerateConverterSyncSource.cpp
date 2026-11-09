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

#include "FramerateConverterSyncSource.h"
#include "PresentTick.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>

namespace videocomposer {

FramerateConverterSyncSource::FramerateConverterSyncSource(
    SyncSource* syncSource,
    InputSource* inputSource)
    : wrappedSyncSource_(syncSource)
    , inputSource_(inputSource)
{
    if (!wrappedSyncSource_) {
        // Can't work without a sync source
        wrappedSyncSource_ = nullptr;
    }
}

bool FramerateConverterSyncSource::connect(const char* param) {
    if (!wrappedSyncSource_) {
        return false;
    }
    return wrappedSyncSource_->connect(param);
}

void FramerateConverterSyncSource::disconnect() {
    if (wrappedSyncSource_) {
    wrappedSyncSource_->disconnect();
    }
}

bool FramerateConverterSyncSource::isConnected() const {
    if (!wrappedSyncSource_) {
        return false;
    }
    return wrappedSyncSource_->isConnected();
}

int64_t FramerateConverterSyncSource::pollFrame(uint8_t* rolling) {
    if (!wrappedSyncSource_) {
        return -1;
    }
    // Get frame from wrapped sync source
    int64_t syncFrame = wrappedSyncSource_->pollFrame(rolling);

    if (syncFrame >= 0 && inputSource_) {
        double syncFps = wrappedSyncSource_->getFramerate();
        if (syncFps > 0) {
            FrameInfo info = inputSource_->getFrameInfo();
            double inputFps = info.framerate;

            // Every content rate goes through the time-based path and the cadence
            // smoother below, including content at the MTC rate (CUEMS MTC is
            // always 25 fps). Equal rates used to take the raw MTC frame from
            // mtcHead, which moves in 10 ms quarter-frame steps at whatever moment
            // each quarter frame is processed: with any load on the box, frames
            // near a vsync boundary tipped early and 25 fps on 60 Hz showed about
            // one single-vsync frame per second (measured on .111, 2026-10-20;
            // 0 per 140 s with this path, a clean 3:2).
            // Tick path (plan 1amfwm7gpz7 §5.2): when the display backend publishes the pacing CRTC's vblank tick
            // and the current vblank, the frame is chosen by the vblank-counted engine below; otherwise, or when
            // the engine declines (no tick, a frozen tick, the kill switch), the legacy smoother runs unchanged.
            if (inputFps > 0 && !cadenceLegacyForced()) {
                const int64_t tickFrame = tickPathPoll(inputFps);
                if (tickFrame >= 0) {
                    syncFrame = tickFrame;
                    cadenceCounters_.tickPath = true;
                    goto cadence_done;
                }
            }
            cadenceCounters_.tickPath = false;
            if (inputFps > 0) {
                // When framerates differ, compute video frame directly from
                // the continuous millisecond time to avoid double-quantization.
                long timeMs = wrappedSyncSource_->getTimeMs();
                if (timeMs >= 0) {
                    double seconds = (static_cast<double>(timeMs) + phaseMs_) / 1000.0;   // + the offset's phase (1amfwm7h924)
                    int64_t rawFrame = static_cast<int64_t>(std::floor(seconds * inputFps));

                    // Vsync-aware cadence smoother.
                    // When display_rate / video_fps is close to an integer N
                    // (e.g. 60/29.97 ≈ 2), enforce a minimum hold of N vsyncs
                    // per frame so the phase drift between vsync and frame
                    // boundaries never causes visible 1-vsync / (N+1)-vsync
                    // stutter pairs.  When the ratio is NOT near-integer
                    // (e.g. 60/24 = 2.5, 60/25 = 2.4), use floor(ratio) and
                    // let the drift catch-up produce the natural N:(N+1)
                    // pulldown pattern (e.g. 3:2 for 24fps @ 60Hz).
                    //
                    // The display refresh rate is measured from the actual call
                    // interval (pollFrame is called once per vsync) instead of
                    // being hardcoded, so this works for any display rate.
                    struct timespec _now;
                    clock_gettime(CLOCK_MONOTONIC, &_now);
                    long long nowUs = static_cast<long long>(_now.tv_sec) * 1000000LL
                                    + _now.tv_nsec / 1000LL;

                    if (cadenceLastCallUs_ > 0) {
                        double deltaMs = static_cast<double>(nowUs - cadenceLastCallUs_) / 1000.0;
                        // Only accept plausible vsync intervals (5–50ms)
                        if (deltaMs > 5.0 && deltaMs < 50.0) {
                            cadenceVsyncPeriodMs_ = 0.95 * cadenceVsyncPeriodMs_ + 0.05 * deltaMs;
                        }
                    }
                    cadenceLastCallUs_ = nowUs;

                    double frameDurMs = 1000.0 / inputFps;
                    double ratio = frameDurMs / cadenceVsyncPeriodMs_;
                    int idealVsyncs = static_cast<int>(std::floor(ratio));
                    if (idealVsyncs < 1) idealVsyncs = 1;

                    if (cadenceDisplayFrame_ < 0
                        || rawFrame < cadenceDisplayFrame_
                        || rawFrame > cadenceDisplayFrame_ + 2) {
                        cadenceDisplayFrame_ = rawFrame;
                        cadenceVsyncCount_   = 0;
                    } else {
                        cadenceVsyncCount_++;
                        int64_t drift = rawFrame - cadenceDisplayFrame_;

                        if (drift >= 1 && cadenceVsyncCount_ >= idealVsyncs) {
                            cadenceDisplayFrame_++;
                            cadenceVsyncCount_ = 0;
                        } else if (drift >= 2) {
                            cadenceDisplayFrame_++;
                            cadenceVsyncCount_ = 0;
                        }
                    }

                    syncFrame = cadenceDisplayFrame_;

                } else {
                    // Fallback: frame-based conversion (may skip frames)
                    syncFrame = static_cast<int64_t>(std::floor(
                        static_cast<double>(syncFrame) * inputFps / syncFps));
                }
            }
        }
    }

cadence_done:
    // Cache results for shared secondary layers
    lastSmoothedFrame_ = syncFrame;
    if (rolling) lastRolling_ = (*rolling != 0);

    return syncFrame;
}

void FramerateConverterSyncSource::setPhaseMs(double ms) {
    if (!std::isfinite(ms) || std::fabs(ms - phaseMs_) < 1e-6) {
        return;
    }
    phaseMs_ = ms;
    anchored_ = false;            // the next tick re-anchors, uncounted: the cue is placed anew, not corrected
    cadenceDisplayFrame_ = -1;    // the legacy smoother takes the new position at its next poll
}

int64_t FramerateConverterSyncSource::getCurrentFrame() const {
    if (lastSmoothedFrame_ >= 0) {
        return lastSmoothedFrame_;  // return smoothed frame after first poll
    }
    // Cold-start: no pollFrame() yet — fall through to raw MTC frame
    if (!wrappedSyncSource_) {
        return -1;
    }
    return wrappedSyncSource_->getCurrentFrame();
}

const char* FramerateConverterSyncSource::getName() const {
    if (!wrappedSyncSource_) {
        return "None";
    }
    return wrappedSyncSource_->getName();
}

double FramerateConverterSyncSource::getFramerate() const {
    // Return the input source's framerate if available, otherwise sync source's framerate
    if (inputSource_) {
        FrameInfo info = inputSource_->getFrameInfo();
        if (info.framerate > 0) {
            return info.framerate;
        }
    }
    if (!wrappedSyncSource_) {
        return -1.0;
    }
    return wrappedSyncSource_->getFramerate();
}

double FramerateConverterSyncSource::getSourceFramerate() const {
    if (wrappedSyncSource_) {
        return wrappedSyncSource_->getFramerate();
    }
    return -1.0;
}

bool FramerateConverterSyncSource::wasFullFrameReceived() {
    if (!wrappedSyncSource_) {
        return false;
    }
    return wrappedSyncSource_->wasFullFrameReceived();
}

long FramerateConverterSyncSource::getTimeMs() const {
    if (wrappedSyncSource_) {
        return wrappedSyncSource_->getTimeMs();
    }
    return -1;
}

long FramerateConverterSyncSource::getDisplayLatencyMs() const {
    if (wrappedSyncSource_) {
        return wrappedSyncSource_->getDisplayLatencyMs();
    }
    return 0;
}

// ============================================================================================================
// The vblank-tick cadence engine (plan 1amfwm7gpz7 §5.2). This is a transcription of the executable reference
// model cuems-RELATIONS/baselines/test3-saltitos-20261104/model/engine.py; a rule changes there first.
// Units: vblanks of the pacing CRTC; `msc` here is the presentation target of the frame chosen at this poll.
// Rounding: floor(x + 0.5), halves away from zero (the model's lrint).
// ============================================================================================================

namespace {
constexpr double kExactTol = 0.02;        // |r − round r| ≤ → an integer hold pattern
constexpr double kDropTol = 0.98;         // r below → the drop regime (0-holds allowed)
constexpr double kH = 0.60;               // the dead band, vsyncs (Ion 2026-11-05; step 1b verifies)
constexpr int kLowpass = 16;              // φ averaged over the last 16 vblanks
constexpr int kSpacing = 30;              // vblanks between corrections
constexpr double kMaxCatchupFrames = 4.0; // beyond this gap the walk re-anchors instead
constexpr int kFrozenPolls = 4;           // tick unchanged while `now` advanced → WARNING + legacy
constexpr int kInvalidReset = 8;          // consecutive invalid ticks → reset + re-anchor
constexpr int kEventsPerSecond = 20;

int64_t cadenceRound(double x) {
    return x >= 0.0 ? static_cast<int64_t>(std::floor(x + 0.5)) : -static_cast<int64_t>(std::floor(-x + 0.5));
}
} // namespace

void FramerateConverterSyncSource::cadenceEvent(int64_t msc, const char* kind) {
    if (cadenceCapture_) {
        cadenceEvents_.emplace_back(msc, kind);
    }
    // the per-event journal line (§5.3), gated like CadenceStats, rate-limited, never for inherent drops
    static const bool statsOn = [] {
        const char* v = std::getenv("VIDEOCOMPOSER_CADENCE_STATS");
        return v && (v[0] == '1' || v[0] == 't');
    }();
    if (!statsOn) {
        return;
    }
    const int64_t vblanksPerSecond = ratioPeriodNs_ > 0 ? (1000000000LL / ratioPeriodNs_) : 60;
    if (eventsSecondMsc_ < 0 || msc - eventsSecondMsc_ >= vblanksPerSecond) {
        eventsSecondMsc_ = msc;
        eventsThisSecond_ = 0;
    }
    if (++eventsThisSecond_ > kEventsPerSecond) {
        return;
    }
    LOG_INFO << "cadence-event layer=c" << cadenceId_ << " kind=" << kind << " msc=" << msc
             << " frame=" << D_ << " hold=" << nD_;
}

void FramerateConverterSyncSource::cadenceLagLine(int64_t msc, double inputFps) {
    static const bool statsOn = [] {
        const char* v = std::getenv("VIDEOCOMPOSER_CADENCE_STATS");
        return v && (v[0] == '1' || v[0] == 't');
    }();
    if (!statsOn || ratioPeriodNs_ <= 0) {
        return;
    }
    const int64_t window = 60LL * 1000000000LL / ratioPeriodNs_;   // 60 s in vblanks
    if (lagLineMsc_ < 0) {
        lagLineMsc_ = msc;
        return;
    }
    if (msc - lagLineMsc_ < window) {
        return;
    }
    lagLineMsc_ = msc;
    // eps = the clock's drift against the grid over the window: (Δpos·r − Δmsc)/Δmsc
    double eps = 0.0;
    if (lagMsc0_ >= 0 && msc > lagMsc0_ && havePos_) {
        eps = ((lastPos_ - lagPos0_) * r_ - static_cast<double>(msc - lagMsc0_)) / static_cast<double>(msc - lagMsc0_);
    }
    lagPos0_ = lastPos_;
    lagMsc0_ = msc;
    LOG_INFO << "cadence-lag layer=c" << cadenceId_ << " fps=" << inputFps << " r=" << r_ << " H=" << kH
             << " corrections(-/+)=" << cadenceCounters_.corrMinus << "/" << cadenceCounters_.corrPlus
             << " skips=" << cadenceCounters_.skips << " drops=" << cadenceCounters_.drops
             << " stalls=" << cadenceCounters_.stalls << " reanchors=" << cadenceCounters_.reanchors
             << " phiMax=" << cadenceCounters_.phiMax << " eps=" << eps;
}

int FramerateConverterSyncSource::cadenceNextHold() {
    errCur_ = err_;                       // the carry of the frame just admitted: what φ measures against
    if (integerN_) {
        return integerN_;
    }
    int n = static_cast<int>(cadenceRound(r_ + err_));
    if (!drop_ && n < 1) {
        n = 1;
    }
    if (n < 0) {
        n = 0;
    }
    err_ += r_ - static_cast<double>(n);
    return n;
}

int FramerateConverterSyncSource::cadenceTakeCorrection(int n, int64_t msc, bool& skip) {
    skip = false;
    const int p = pendingCorrection_;
    if (p == 0) {
        return n;
    }
    if (!integerN_) {
        // Non-integer ratio (Bresenham, drop regime): the correction waits for the hold it can change without leaving
        // A = {floor r, ceil r} (execution amendment E12): a -1 turns the next ceil(r) hold into floor(r), a +1 the
        // next floor(r) into ceil(r). 25p@60: a 3 becomes a 2, never a 2 a 1 (bench row n3: five 1-holds in 18 min
        // with the first rule); 30p@50: a 1 becomes a 2, never a 2 a 3; drop regime: a +1 shows a frame that would
        // have been dropped. At most two holds of delay.
        const int lo = static_cast<int>(std::floor(r_));
        if ((p < 0 && n != lo + 1) || (p > 0 && n != lo)) {
            return n;
        }
    }
    if (p < 0) {
        if (integerN_ == 1) {
            skip = true;                  // content at the display rate: the inherent event (K3)
        } else {
            n -= 1;
        }
        ++cadenceCounters_.corrMinus;
        if (skip) {
            ++cadenceCounters_.skips;
        }
    } else {
        n += 1;
        ++cadenceCounters_.corrPlus;
    }
    pendingCorrection_ = 0;
    hasCorr_ = true;
    lastCorrMsc_ = msc;
    cadenceEvent(msc, p < 0 ? (skip ? "skip" : "corr-") : "corr+");
    return n;
}

void FramerateConverterSyncSource::cadenceAnchor(double pos, int64_t msc) {
    D_ = static_cast<int64_t>(std::floor(pos));
    const double frac = pos - static_cast<double>(D_);
    const int64_t a = cadenceRound(frac * r_);
    mD_ = msc - a;
    err_ = static_cast<double>(a) - frac * r_;
    nD_ = cadenceNextHold();
    anchored_ = true;
    pendingCorrection_ = 0;
    inFlightUntil_ = -1;
    phiRingN_ = 0;
    phiRingHead_ = 0;
    lastPos_ = pos;
    havePos_ = true;
}

int64_t FramerateConverterSyncSource::tickPathPoll(double inputFps) {
    const PresentTick tick = currentPresentTick();
    const VblankNow now = currentVblank();
    if (tick.msc < 0 || now.msc < 0 || !wrappedSyncSource_) {
        return -1;                        // no tick: the legacy smoother
    }
    // ---- validity and the frozen tick (§5.1)
    const bool advanced = tick.msc > lastTickMsc_;
    if (!advanced && lastTickMsc_ >= 0 && tick.msc == lastTickMsc_ && now.msc > lastNowMsc_) {
        if (++frozenPolls_ >= kFrozenPolls) {
            if (!frozenWarned_) {
                LOG_WARNING << "cadence: the vblank tick is frozen at msc " << tick.msc
                            << " while the CRTC advances (hung CRTC?): legacy smoother until it moves";
                frozenWarned_ = true;
                pathLoggedTick_ = false;
            }
            needReanchor_ = true;         // re-anchor (counted) when the tick moves again
            lastNowMsc_ = now.msc;
            return -1;
        }
    }
    lastNowMsc_ = now.msc;
    const bool valid = advanced && tick.ustNs > 0 && tick.periodNs > 0 && now.ustNs > 0;
    if (!valid) {
        if (++invalidTicks_ >= kInvalidReset) {
            lastTickMsc_ = -1;            // a persistent backward msc (counter reset, u32 wrap): start afresh
            invalidTicks_ = 0;
            needReanchor_ = true;         // the next valid tick re-anchors; the frame stays held meanwhile
        }
        return anchored_ ? D_ : -1;       // hold the frame for this poll; legacy before any anchor
    }
    invalidTicks_ = 0;
    frozenPolls_ = 0;
    if (frozenWarned_) {
        LOG_INFO << "cadence: the vblank tick moves again at msc " << tick.msc;
        frozenWarned_ = false;
    }
    lastTickMsc_ = tick.msc;
    if (!pathLoggedTick_) {
        LOG_INFO << "cadence: path=tick now=" << (now.fromIoctl ? "ioctl" : "tick");
        pathLoggedTick_ = true;
    }
    // ---- rule 1: ratio, target
    if (inputFps != ratioFps_ || tick.periodNs != ratioPeriodNs_) {
        ratioFps_ = inputFps;
        ratioPeriodNs_ = tick.periodNs;
        r_ = (1e9 / inputFps) / static_cast<double>(tick.periodNs);
        const int64_t n = cadenceRound(r_);
        integerN_ = (std::fabs(r_ - static_cast<double>(n)) <= kExactTol) ? static_cast<int>(n) : 0;
        drop_ = (r_ < kDropTol && integerN_ == 0);
        anchored_ = false;                // a change of fps or period re-anchors (research §D.2 rule 5)
        LOG_INFO << "cadence: layer=c" << cadenceId_ << " fps " << inputFps << ", vsync " << (tick.periodNs / 1e6) << " ms, r " << r_
                 << (integerN_ ? " -> hold " : (drop_ ? " -> drop regime (" : " -> bresenham ("))
                 << (integerN_ ? std::to_string(integerN_) + " (exact)" : std::to_string(r_) + ")")
                 << " H=" << kH << " filter=" << kLowpass;
    }
    const int64_t tgt = presentationTarget(now, tick, kPipelineDepth);
    const int64_t excess = tgt - (now.msc + kPipelineDepth);
    // ---- rule 2: the clock at the presentation vblank
    const double posMs = wrappedSyncSource_->getTimeMsAt(now.ustNs + excess * tick.periodNs);
    if (posMs < 0.0) {
        return -1;                        // not following / no clock yet
    }
    const double pos = (posMs + phaseMs_) * inputFps / 1000.0;   // + the engine offset's sub-frame phase (1amfwm7h924)
    const int64_t msc = tgt;
    const bool gap = lastMsc_ >= 0 && msc - lastMsc_ > 1;
    if (gap) {
        ++cadenceCounters_.stalls;
        cadenceEvent(msc, "stall");
    }
    if (!anchored_ || needReanchor_) {
        if (anchored_ && lastMsc_ >= 0) {
            ++cadenceCounters_.reanchors;
            cadenceEvent(msc, "reanchor");
        }
        needReanchor_ = false;
        cadenceAnchor(pos, msc);
    } else if (havePos_ && std::fabs(pos - lastPos_) < 1e-6) {
        // ---- rule 3: stopped clock. phi = (pos − D) r − (msc − mD) + errCur = 0 → re-base mD
        mD_ = msc - cadenceRound((pos - static_cast<double>(D_)) * r_ + errCur_);
        lastMsc_ = msc;
        return D_;
    }
    lastPos_ = pos;
    havePos_ = true;
    // ---- rule 5: the walk on the grid, bounded
    if (static_cast<double>(msc - (mD_ + nD_)) > kMaxCatchupFrames * std::max(r_, 1.0)) {
        ++cadenceCounters_.reanchors;
        cadenceEvent(msc, "reanchor-catchup");
        cadenceAnchor(pos, msc);
    }
    while (msc >= mD_ + nD_) {
        D_ += 1;
        mD_ += nD_;
        int n = cadenceNextHold();
        const bool had = pendingCorrection_ != 0;
        bool skip = false;
        n = cadenceTakeCorrection(n, msc, skip);
        if (had && pendingCorrection_ == 0) {
            inFlightUntil_ = mD_ + n;     // the corrected hold ends here: φ has moved by then
        }
        if (skip) {
            D_ += 1;
        }
        if (n == 0) {
            ++cadenceCounters_.drops;     // the drop regime: an inherent dropped frame, no event line
        }
        nD_ = n;
    }
    // ---- rule 7: discontinuity, after the walk
    if (std::fabs(pos - static_cast<double>(D_)) >= 2.0) {
        ++cadenceCounters_.reanchors;
        cadenceEvent(msc, "reanchor");
        cadenceAnchor(pos, msc);
        while (msc >= mD_ + nD_) {
            D_ += 1;
            mD_ += nD_;
            const int n = cadenceNextHold();
            if (n == 0) {
                ++cadenceCounters_.drops;
            }
            nD_ = n;
        }
    }
    // ---- rule 6: the band on the ideal phase of the current frame, low-passed; a correction in flight blocks arming
    const double phi = (pos - static_cast<double>(D_)) * r_ - static_cast<double>(msc - mD_) + errCur_;
    cadenceCounters_.phiMax = std::max(cadenceCounters_.phiMax, std::fabs(phi));
    const bool inflight = inFlightUntil_ >= 0 && msc < inFlightUntil_;
    if (inFlightUntil_ >= 0 && msc >= inFlightUntil_) {
        inFlightUntil_ = -1;
        phiRingN_ = 0;
        phiRingHead_ = 0;
    }
    phiRing_[phiRingHead_] = phi;
    phiRingHead_ = (phiRingHead_ + 1) % kLowpass;
    if (phiRingN_ < kLowpass) {
        ++phiRingN_;
    }
    double phiF = 0.0;
    for (int i = 0; i < phiRingN_; ++i) {
        phiF += phiRing_[i];
    }
    phiF /= static_cast<double>(phiRingN_);
    if (!gap && !inflight && pendingCorrection_ == 0 && (!hasCorr_ || msc - lastCorrMsc_ >= kSpacing)) {
        if (phiF > kH) {
            pendingCorrection_ = -1;
        } else if (phiF < -kH) {
            pendingCorrection_ = +1;
        }
    }
    lastMsc_ = msc;
    cadenceLagLine(msc, inputFps);
    return D_;
}

} // namespace videocomposer
