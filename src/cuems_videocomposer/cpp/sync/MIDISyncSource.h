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

#ifndef VIDEOCOMPOSER_MIDISYNCSOURCE_H
#define VIDEOCOMPOSER_MIDISYNCSOURCE_H

#include "SyncSource.h"
#include "MIDIDriver.h"
#include "MTCDecoder.h"
#include "MtcInvalidHold.h"
#include "QfSeqlock.h"
#include <atomic>
#include <string>
#include <memory>
#include <cstdint>

namespace videocomposer {

/**
 * MIDISyncSource - MIDI Time Code (MTC) synchronization
 * 
 * Implements SyncSource interface for MTC over MIDI.
 * Uses pure C++ implementation with driver abstraction.
 */
class MIDISyncSource : public SyncSource {
public:
    MIDISyncSource();
    virtual ~MIDISyncSource();

    // SyncSource interface
    bool connect(const char* param = nullptr) override;
    void disconnect() override;
    bool isConnected() const override;
    int64_t pollFrame(uint8_t* rolling = nullptr) override;
    int64_t getCurrentFrame() const override;
    const char* getName() const override;
    
    /**
     * Get the name of the current MIDI driver
     * @return Driver name (e.g., "mtcreceiver", "ALSA-Sequencer")
     */
    const char* getCurrentDriverName() const;

    /**
     * Set framerate for frame calculation
     * @param fps Framerate in frames per second
     */
    void setFramerate(double fps) { framerate_ = fps; }

    /**
     * Get the framerate of this sync source's timecode
     * @return Framerate in frames per second
     */
    double getFramerate() const override { return framerate_; }

    /**
     * Set MIDI clock adjustment mode
     * @param enabled If true, enable clock adjustment
     */
    void setClockAdjustment(bool enabled);

    /**
     * Set frame delay for clock adjustment
     * @param delay Delay in seconds (1.0/fps, or 0 to calculate from framerate)
     */
    void setDelay(double delay);

    /**
     * Set verbose mode
     * @param verbose If true, enable verbose output
     */
    void setVerbose(bool verbose);

    /**
     * Choose MIDI driver by name
     * @param driverName Driver name (e.g., "ALSA-Sequencer", "PORTMIDI")
     * @return true if driver selected successfully
     */
    bool chooseDriver(const std::string& driverName);
    
    /**
     * Check if a full frame SYSEX was just received (indicates position jump/seek needed)
     * @return true if a full frame was received since last check
     */
    bool wasFullFrameReceived() override;
    
    /**
     * Get MTC timecode position in ms, with display-pipeline latency
     * compensation applied (returns wire-MTC + displayLatencyMs_ so the
     * frame chosen at wall-clock T is the one visible at MTC = T).
     */
    long getTimeMs() const override;

    /**
     * The smooth MTC clock evaluated at monotonicNs (plan 1amfwm7gpz7 §5.2 rule 2): runs the same state advance as
     * getTimeMs() once per distinct instant (memoised, so every layer of a poll gets the same value), then
     * extrapolates from "now" to the requested instant at the current rate. While the timecode is not running or
     * the D13 hold is active the clock is pinned and the pinned value is returned as is, whatever the instant.
     */
    double getTimeMsAt(int64_t monotonicNs) const override;

    /**
     * Set the display-pipeline latency compensation in ms. Values outside
     * [0, 200] are clamped. Thread-safe (atomic).
     */
    void setDisplayLatencyMs(long ms);

    /**
     * Get the display-pipeline latency compensation in ms.
     * Thread-safe (atomic read).
     */
    long getDisplayLatencyMs() const override { return displayLatencyMs_.load(); }

    /**
     * Undecodable timecode (D13): register mtcreceiver's tick callback and
     * judge the quarter frames with the audio player's anchored-run rule.
     * After 800 ms of a moving head without an anchored tick (a doubled
     * rtpmidid subscription) the source holds: pollFrame() and getTimeMs()
     * return the values they had at that instant and full-frame
     * notifications are swallowed, until the first anchored tick. Call once,
     * from the application's own global sync source, after connect(); a no-op
     * unless the connected driver is the mtcreceiver one. The callback is the
     * single static one of mtcreceiver: nothing else may register it.
     */
    bool enableAnchoredLiveness();
    bool isAnchoredLivenessEnabled() const { return livenessEnabled_; }
    bool isHeldOnInvalidTimecode() const { return hold_.held(); }

private:
    std::unique_ptr<MIDIDriver> driver_;
    MTCDecoder mtcDecoder_;
    std::string midiPort_;
    double framerate_;
    int64_t currentFrame_;
    bool connected_;
    std::atomic<long> displayLatencyMs_;

    // D13: the anchored-run liveness rule and its hold (render thread only,
    // except ticks_, whose writer is the MIDI thread)
    void updateLiveness();
    long getTimeMsFollowing() const;   // the smoothed MTC time (today's getTimeMs)
    TickSeqlock ticks_;
    TickSample lastTick_;
    MtcInvalidHold hold_;
    bool livenessEnabled_ = false;
    int64_t frozenFrame_ = -1;
    mutable long lastTimeMs_ = -1;
    long frozenTimeMs_ = -1;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_MIDISYNCSOURCE_H

