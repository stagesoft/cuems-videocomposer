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

#include "MIDISyncSource.h"
#include "NullMIDIDriver.h"
#include "ALSASeqMIDIDriver.h"
#include "../utils/Logger.h"
#ifdef HAVE_MTCRECEIVER
#include "MtcReceiverMIDIDriver.h"
#endif
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace videocomposer {

MIDISyncSource::MIDISyncSource()
    : framerate_(25.0)
    , currentFrame_(-1)
    , connected_(false)
    , displayLatencyMs_(33)
{
    // Start with null driver (will be replaced when driver is chosen)
    driver_ = std::make_unique<NullMIDIDriver>();
    LOG_INFO << "MIDISyncSource: display latency compensation = "
             << displayLatencyMs_.load() << " ms";
}

MIDISyncSource::~MIDISyncSource() {
    disconnect();
}

void MIDISyncSource::setDisplayLatencyMs(long ms) {
    if (ms < 0) ms = 0;
    if (ms > 200) ms = 200;
    displayLatencyMs_.store(ms);
    LOG_INFO << "MIDISyncSource: display latency compensation updated to "
             << ms << " ms";
}

bool MIDISyncSource::connect(const char* param) {
    if (connected_) {
        disconnect();
    }

    if (param) {
        midiPort_ = param;
    } else {
        midiPort_ = "-1"; // Default: autodetect
    }

    // If no driver selected, try to get first available
    if (!driver_ || dynamic_cast<NullMIDIDriver*>(driver_.get())) {
        driver_ = MIDIDriverFactory::createFirstAvailable();
        if (!driver_) {
            driver_ = std::make_unique<NullMIDIDriver>();
            return false;
        }
    }

    // Open MIDI connection
    connected_ = driver_->open(midiPort_);
    if (connected_) {
        currentFrame_ = -1;
        mtcDecoder_.reset();
    }

    return connected_;
}

void MIDISyncSource::disconnect() {
    if (connected_ && driver_) {
        driver_->close();
        connected_ = false;
        currentFrame_ = -1;
        mtcDecoder_.reset();
    }
    midiPort_.clear();
}

bool MIDISyncSource::isConnected() const {
    return connected_ && driver_ && driver_->isConnected();
}

void MIDISyncSource::setClockAdjustment(bool enabled) {
    ALSASeqMIDIDriver* alsaDriver = dynamic_cast<ALSASeqMIDIDriver*>(driver_.get());
    if (alsaDriver) {
        alsaDriver->setClockAdjustment(enabled);
    }
}

void MIDISyncSource::setDelay(double delay) {
    ALSASeqMIDIDriver* alsaDriver = dynamic_cast<ALSASeqMIDIDriver*>(driver_.get());
    if (alsaDriver) {
        alsaDriver->setDelay(delay);
    }
}

void MIDISyncSource::setVerbose(bool verbose) {
#ifdef HAVE_MTCRECEIVER
    MtcReceiverMIDIDriver* mtcDriver = dynamic_cast<MtcReceiverMIDIDriver*>(driver_.get());
    if (mtcDriver) {
        mtcDriver->setVerbose(verbose);
    }
#endif
    ALSASeqMIDIDriver* alsaDriver = dynamic_cast<ALSASeqMIDIDriver*>(driver_.get());
    if (alsaDriver) {
        alsaDriver->setVerbose(verbose);
    }
}

int64_t MIDISyncSource::pollFrame(uint8_t* rolling) {
    if (!isConnected()) {
        return -1;
    }

    // Set framerate on driver if it supports it
#ifdef HAVE_MTCRECEIVER
    MtcReceiverMIDIDriver* mtcDriver = dynamic_cast<MtcReceiverMIDIDriver*>(driver_.get());
    if (mtcDriver) {
        mtcDriver->setFramerate(framerate_);
    }
#endif
    ALSASeqMIDIDriver* alsaDriver = dynamic_cast<ALSASeqMIDIDriver*>(driver_.get());
    if (alsaDriver) {
        alsaDriver->setFramerate(framerate_);
    }

    // Poll driver for frame (driver handles MIDI message parsing)
    int64_t frame = driver_->pollFrame();

    if (frame >= 0) {
        // Display-pipeline latency compensation: advance the chosen frame so
        // the buffer submitted now is the one visible at MTC = current wall
        // clock when the GPU/scanout pipeline actually presents it on screen.
        // Conversion is at MTC framerate; FramerateConverterSyncSource may
        // re-compute downstream from getTimeMs() for non-matching video fps.
        long latencyCompMs = displayLatencyMs_.load();
        if (latencyCompMs > 0 && framerate_ > 0.0) {
            frame += static_cast<int64_t>(std::round(
                static_cast<double>(latencyCompMs) * framerate_ / 1000.0));
        }
        currentFrame_ = frame;
    }

    // Determine rolling state based on driver type
    if (rolling) {
#ifdef HAVE_MTCRECEIVER
        MtcReceiverMIDIDriver* mtcDriver = dynamic_cast<MtcReceiverMIDIDriver*>(driver_.get());
        if (mtcDriver) {
            // mtcreceiver tracks rolling state via isTimecodeRunning
            *rolling = (frame >= 0 && MtcReceiver::isTimecodeRunning.load()) ? 1 : 0;
        } else
#endif
        {
            // For other drivers, assume rolling if we have a valid frame
            // MTC is considered "rolling" if we're receiving continuous timecode updates
            *rolling = (frame >= 0) ? 1 : 0;
        }
    }

    // #region DEBUG
    {
        static long long s_lastTickNs = 0;
        struct timespec _ts;
        clock_gettime(CLOCK_MONOTONIC, &_ts);
        long long _nowNs = (long long)_ts.tv_sec * 1000000000LL + _ts.tv_nsec;
        if (_nowNs - s_lastTickNs >= 1000000000LL) {
            s_lastTickNs = _nowNs;
#ifdef HAVE_MTCRECEIVER
            long _mtcMs = (long)MtcReceiver::mtcHead.load();
#else
            long _mtcMs = -1;
#endif
            FILE* _f = fopen("/tmp/.claude/debug.log", "a");
            if (_f) {
                long _gtm = getTimeMs();
                fprintf(_f, "[DEBUG H3/H4] VIDEO_TICK wall_ns=%lld mtc_ms=%ld getTimeMs=%ld frame=%lld\n",
                        (long long)_nowNs, _mtcMs, _gtm, (long long)frame);
                fclose(_f);
            }
        }
    }
    // #endregion DEBUG

    return frame;
}

int64_t MIDISyncSource::getCurrentFrame() const {
    return currentFrame_;
}

const char* MIDISyncSource::getName() const {
    return "MIDI";
}

const char* MIDISyncSource::getCurrentDriverName() const {
    if (driver_) {
        return driver_->getName();
    }
    return "None";
}

bool MIDISyncSource::chooseDriver(const std::string& driverName) {
    if (connected_) {
        return false; // Can't change driver while connected
    }

    auto newDriver = MIDIDriverFactory::create(driverName);
    if (newDriver && newDriver->isSupported()) {
        driver_ = std::move(newDriver);
        return true;
    }

    return false;
}

bool MIDISyncSource::wasFullFrameReceived() {
#ifdef HAVE_MTCRECEIVER
    MtcReceiverMIDIDriver* mtcDriver = dynamic_cast<MtcReceiverMIDIDriver*>(driver_.get());
    if (mtcDriver) {
        return mtcDriver->wasFullFrameReceived();
    }
#endif
    // Other drivers don't support full frame detection yet
    return false;
}

long MIDISyncSource::getTimeMs() const {
#ifdef HAVE_MTCRECEIVER
    long baseMtcMs = MtcReceiver::mtcHead.load();
    bool isRunning = MtcReceiver::isTimecodeRunning.load();

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long long nowUs = static_cast<long long>(now.tv_sec) * 1000000LL
                    + now.tv_nsec / 1000LL;

    // MtcReceiver::mtcHead advances in ~10ms discrete steps (2 MIDI quarter-
    // frames at 25 fps).  Position-correcting PLLs (errorUs/20) introduce
    // ~500µs per-vsync jitter that pushes frame boundary crossings to wrong
    // vsyncs when the vsync phase aligns with frame boundaries.
    //
    // Solution: rate-correcting extrapolation.  Advance a µs counter at a
    // continuously adjusted rate so the output is perfectly smooth (zero
    // position jitter) while the rate slowly tracks the MTC source clock
    // (eliminating long-term drift).
    //
    // The rate is estimated from mtcHead step intervals: each time mtcHead
    // changes, measure the ratio of MTC advance to wall-clock advance and
    // feed it into an exponential moving average (α=0.05).  This gives a
    // stable rate estimate with a ~2s time constant.
    //
    // Static locals are safe: called only from the single-threaded render loop.
    static long long s_smoothUs  = -1;
    static long long s_lastWcUs  =  0;
    static double    s_rate      = 1.0;  // MTC µs per wall µs
    static long      s_prevMtcMs = -1;
    static long long s_prevMtcWcUs = 0;  // wall time when mtcHead last changed

    if (!isRunning || s_smoothUs < 0) {
        s_smoothUs   = static_cast<long long>(baseMtcMs) * 1000LL;
        s_lastWcUs   = nowUs;
        s_rate       = 1.0;
        s_prevMtcMs  = baseMtcMs;
        s_prevMtcWcUs = nowUs;
        return baseMtcMs + displayLatencyMs_.load();
    }

    // Detect mtcHead change → update rate estimate
    bool justSnapped = false;
    if (baseMtcMs != s_prevMtcMs) {
        long long wcElapsedUs = nowUs - s_prevMtcWcUs;
        long mtcStepMs = baseMtcMs - s_prevMtcMs;

        // Only update rate from forward, reasonable steps
        if (mtcStepMs > 0 && mtcStepMs < 100 && wcElapsedUs > 1000) {
            double measuredRate = (static_cast<double>(mtcStepMs) * 1000.0)
                                / static_cast<double>(wcElapsedUs);
            // Exponential moving average, α=0.05 → τ ≈ 20 MTC steps ≈ 200ms
            s_rate = 0.95 * s_rate + 0.05 * measuredRate;
        }

        // Large jump (seek / cue change): snap and reset
        if (mtcStepMs > 200 || mtcStepMs < -10) {
            s_smoothUs = static_cast<long long>(baseMtcMs) * 1000LL;
            // Reset wall-clock anchor so the unconditional advance below
            // contributes 0 µs on this call. Without this, sparse callers
            // (e.g. 1 Hz instrumentation polls) re-add up to the 100 ms
            // wallDelta cap on top of the just-snapped baseMtc, and
            // justSnapped suppresses the anti-drift correction — yielding
            // a steady-state +100 ms bias visible to FramerateConverter
            // when video fps differs from MTC fps.
            s_lastWcUs = nowUs;
            s_rate = 1.0;
            justSnapped = true;
        }

        s_prevMtcMs   = baseMtcMs;
        s_prevMtcWcUs = nowUs;
    }

    // Advance by wall-clock delta × rate (smooth, drift-free)
    long long wallDeltaUs = nowUs - s_lastWcUs;
    s_lastWcUs = nowUs;
    if (wallDeltaUs < 0)      wallDeltaUs = 0;
    if (wallDeltaUs > 100000) wallDeltaUs = 100000;

    s_smoothUs += static_cast<long long>(
        static_cast<double>(wallDeltaUs) * s_rate);

    // --- Layer 2: position anti-drift (mpv-style) ---
    // After rate advance, nudge s_smoothUs toward baseMtcMs to prevent
    // unbounded drift from rate EMA settling below 1.0.
    // Skip on the same call where a large-jump snap fired — s_smoothUs
    // was just set to baseMtcMs and the rate advance only added one
    // poll-interval; errorMs would be tiny or trigger a spurious snap.
    // justSnapped is intentionally local: same-call suppression is all
    // that's needed; on the next call errorMs will be small.
    if (!justSnapped) {
        long long smoothMs = s_smoothUs / 1000LL;
        long long errorMs  = static_cast<long long>(baseMtcMs) - smoothMs;
        constexpr long long SNAP_THRESHOLD_MS  = 10;
        constexpr int       CORRECTION_DIVISOR = 10;
        if (std::abs(errorMs) > SNAP_THRESHOLD_MS) {
            s_smoothUs = static_cast<long long>(baseMtcMs) * 1000LL;
            s_rate = 1.0;
        } else if (errorMs != 0) {
            s_smoothUs += (errorMs * 1000LL) / CORRECTION_DIVISOR;
        }
    }

    return static_cast<long>(s_smoothUs / 1000LL) + displayLatencyMs_.load();
#else
    return -1;
#endif
}

} // namespace videocomposer

