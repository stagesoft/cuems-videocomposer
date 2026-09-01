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

#ifndef VIDEOCOMPOSER_SATURATION_MONITOR_H
#define VIDEOCOMPOSER_SATURATION_MONITOR_H

#include "MachineProfile.h"
#include "MonitorState.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace videocomposer {

/**
 * SaturationMonitor - says when the box is struggling. Never changes anything.
 *
 * ## Warning and refusing are different questions
 *
 * Everything this class reads is a *saturation* signal: decode occupancy,
 * dropped frames, VRAM and GTT pressure. None of it may gate admission, and
 * the reason is measured rather than stylistic - 99.6 % decode occupancy
 * survived a run that 86.9 % occupancy hung. A threshold over these numbers
 * gets both cases wrong. So they warn, loudly and legibly, and the operator
 * decides; only the session cap in HangGuard may refuse.
 *
 * ## What each signal is worth
 *
 * - **Decode occupancy** (`/proc/self/fdinfo`, deduplicated by drm-client-id
 *   and differenced) is the honest measure of how hard the decode engine is
 *   working. amdgpu only: i915 does not export drm-engine-dec and reads 0.0 %
 *   while genuinely busy, so on Intel it is reported unavailable exactly once
 *   and the warnings ride the frame-miss counters instead.
 * - **Frame misses and pacing slips** are what the operator actually sees, and
 *   they work on every GPU.
 * - **VRAM + GTT** are directional only. The footprints of the runs that hung
 *   are censored by the client kills that accompany them, and two clean runs
 *   sat at 96 % and at full VRAM, so this is context in a message, never a
 *   trigger on its own.
 * - **IO_PAGE_FAULT** is the mechanism itself: every recorded ring timeout was
 *   preceded by one, and four bursts in five ended in a timeout roughly ten
 *   seconds later. It is the sharpest "it is happening now" signal available -
 *   and it is still only an alarm, because by the time the fault is logged the
 *   faulting job can no longer signal its fence. It diagnoses; it cannot save
 *   the frame. Reading it needs /dev/kmsg, which the cuems user may not be
 *   allowed to open; where that is so the channel reports itself unavailable
 *   rather than quietly reading clean.
 *
 * ## Thread and safety
 *
 * Runs on its own thread and touches no layer, input source or decode queue.
 * It reads sysfs, procfs and the atomics that producers publish into
 * SaturationSignals - nothing else. It never takes the decode queue's access
 * gate, which the recovery worker can hold for seconds at a time.
 */
class SaturationMonitor {
public:
    SaturationMonitor();
    ~SaturationMonitor();

    /** Start sampling. Safe to call when the profile has no GPU at all. */
    void start(const MachineProfile& profile);

    /** Stop and join. Idempotent. */
    void stop();

    /** Publish this object's fields into the shared snapshot. */
    void fillMonitorState(MonitorState& out) const;

    /**
     * Sampling period. One second is fast enough to catch the ~10 s lead
     * between a page-fault burst and the ring watchdog, and slow enough that
     * the whole instrument costs nothing measurable.
     */
    static constexpr std::chrono::milliseconds SAMPLE_PERIOD{1000};

    /**
     * Seconds a condition must hold before WARN is entered.
     *
     * Named here because a project load briefly drives every one of these
     * signals hard - the arm prefill burst is ~3.5 s of concurrent decoding -
     * and a monitor that shouted on every load would be turned off within a
     * week. Five seconds outlasts the burst without hiding a real problem.
     */
    static constexpr int WARN_SUSTAIN_SECONDS = 5;

    /** Decode occupancy at or above which the engine is considered saturated. */
    static constexpr double OCCUPANCY_WARN_PERCENT = 85.0;

    /** Frame misses per second at or above which delivery is considered degraded. */
    static constexpr double MISS_RATE_WARN_PER_SEC = 5.0;

    /** Distinct layers erroring at once that makes it a platform event, not a file. */
    static constexpr int ERROR_BURST_LAYERS = 2;

    /** Window over which those layers are counted. */
    static constexpr long ERROR_BURST_WINDOW_MS = 10000;

private:
    void run();
    void sample();
    void evaluate(double occupancy, double missRate, int erroringLayers, int newFaults);
    void logTransition(SaturationLevel from, SaturationLevel to, const std::string& why);

    // Signal readers. Each returns a sentinel rather than throwing when the
    // platform does not provide it.
    double readDecodeOccupancyPercent();   // -1 when unavailable
    void   readMemory();
    int    readNewPageFaults();            // -1 when the channel is unavailable

    MachineProfile profile_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    // --- published state -------------------------------------------------
    std::atomic<int>  level_{static_cast<int>(SaturationLevel::clear)};
    std::atomic<long> alarms_{0};
    std::atomic<long> pageFaultAlarms_{0};
    std::atomic<bool> pageFaultAvailable_{false};
    std::atomic<bool> occupancyAvailable_{false};
    std::atomic<bool> memoryAvailable_{false};
    std::atomic<long long> occupancyMilliPercent_{-1000};  // x1000, -1000 = unavailable
    std::atomic<long> vramUsedMb_{-1};
    std::atomic<long> vramTotalMb_{-1};
    std::atomic<long> gttUsedMb_{-1};
    std::atomic<long long> lastTransitionMs_{0};

    // --- sampler-private state (monitor thread only) ---------------------
    long long lastDecNs_ = -1;
    std::chrono::steady_clock::time_point lastSampleAt_{};
    long lastMissTotal_ = 0;
    int sustainedSeconds_ = 0;
    int kmsgFd_ = -1;
    bool announcedOccupancyUnavailable_ = false;
    bool announcedPageFaultUnavailable_ = false;
    long suppressedTransitions_ = 0;
    std::chrono::steady_clock::time_point lastTransitionLog_{};

    std::string vramUsedPath_;
    std::string vramTotalPath_;
    std::string gttUsedPath_;
};

/** Process-global monitor, started at application init. */
SaturationMonitor& saturationMonitor();

} // namespace videocomposer

#endif // VIDEOCOMPOSER_SATURATION_MONITOR_H
