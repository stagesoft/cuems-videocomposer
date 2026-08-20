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

/**
 * PresentationTiming.h - Frame pacing and vsync timing (like mpv's present_sync)
 *
 * Tracks presentation timestamps from DRM page flip events to:
 * - Calculate actual vsync duration
 * - Detect skipped vsyncs (dropped frames)
 * - Provide accurate frame timing information
 *
 * Optionally tracks submit→flip latency (the swap-chain depth × vsync) for
 * runtime display-latency measurement at startup. Disabled by default;
 * enable via enableLatencyCapture() before driving frames.
 */

#ifndef VIDEOCOMPOSER_PRESENTATIONTIMING_H
#define VIDEOCOMPOSER_PRESENTATIONTIMING_H

#include <cstdint>
#include <cstddef>
#include <chrono>
#include <deque>
#include <mutex>
#include <vector>

namespace videocomposer {

/**
 * Stores timing information for a single presented frame
 */
struct PresentationEntry {
    int64_t ust = 0;              // Presentation timestamp (nanoseconds, monotonic)
    int64_t msc = 0;              // Vsync counter (frame number from display)
    int64_t vsync_duration = 0;   // Calculated vsync interval (ns)
    int64_t skipped_vsyncs = 0;   // Number of vsyncs skipped (dropped frames)
    int64_t display_time = 0;     // When frame was actually displayed (ns)
    bool valid = false;
};

/**
 * Statistics for swap-chain submit→flip latency over a captured sample window.
 */
struct LatencyStats {
    bool valid = false;
    int64_t medianNs = 0;
    int64_t p95Ns = 0;
    size_t  sampleCount = 0;
    int64_t expectedVsyncNs = 0;
};

/**
 * Tracks presentation timing for smooth frame pacing
 *
 * Usage:
 *   PresentationTiming timing;
 *   timing.init(60.0);  // Expected 60fps
 *
 *   // In page flip callback:
 *   timing.recordFlip(sec, usec, msc);
 *
 *   // To check timing:
 *   auto info = timing.getInfo();
 *   if (info.skipped_vsyncs > 0) {
 *       LOG_WARNING << "Dropped " << info.skipped_vsyncs << " frame(s)";
 *   }
 */
class PresentationTiming {
public:
    PresentationTiming();
    ~PresentationTiming() = default;

    /**
     * Initialize with expected refresh rate
     * @param refreshHz Display refresh rate (e.g., 60.0)
     */
    void init(double refreshHz);

    /**
     * Record a page flip event (called from DRM page flip handler)
     * @param sec Seconds from DRM event
     * @param usec Microseconds from DRM event
     * @param msc Vsync counter from DRM event
     */
    void recordFlip(unsigned int sec, unsigned int usec, unsigned int msc);

    /**
     * Record a successful drmModePageFlip submission. No-op when capture is
     * disabled (the production hot path). Pairs FIFO with the next recordFlip.
     */
    void recordSubmit();

    /**
     * Pop the front pending-submit timestamp. Called from waitForFlip's timeout
     * branch so an orphaned submit cannot mispair with a later flip event.
     */
    void discardPendingSubmit();

    /**
     * Enable submit→flip capture with a bounded sample window.
     * Idempotent; calling again preserves existing samples.
     */
    void enableLatencyCapture(size_t maxSamples = 256);

    /**
     * Disable capture and clear pending state. Already-collected samples remain
     * readable via getSwapChainLatencyStats() until reset().
     */
    void disableLatencyCapture();

    /**
     * Get current presentation timing info
     */
    PresentationEntry getInfo() const;

    /**
     * Get calculated vsync duration (actual, not expected)
     * @return Vsync duration in nanoseconds, or 0 if not yet calculated
     */
    int64_t getVsyncDuration() const { return current_.vsync_duration; }

    /**
     * Get expected vsync duration based on refresh rate
     */
    int64_t getExpectedVsyncDuration() const { return expectedVsyncNs_; }

    /**
     * Get number of skipped vsyncs in last flip
     */
    int64_t getSkippedVsyncs() const { return current_.skipped_vsyncs; }

    /**
     * Check if we're running behind (frames being dropped)
     */
    bool isRunningBehind() const { return current_.skipped_vsyncs > 0; }

    /**
     * Get aggregated submit→flip latency statistics over the captured window.
     */
    LatencyStats getSwapChainLatencyStats() const;

    /**
     * Reset statistics, including any captured submit→flip samples and pending
     * submits. Capture-enabled state is preserved.
     */
    void reset();

private:
    PresentationEntry current_;
    PresentationEntry previous_;

    int64_t expectedVsyncNs_ = 0;      // Expected vsync duration based on refresh rate
    int64_t totalUnexpectedDrops_ = 0; // Skipped vsyncs beyond jitter (actual problems)
    double displayHz_ = 0.0;           // Display refresh rate
    bool initialized_ = false;

    // Submit↔flip latency capture (off by default, no production overhead)
    mutable std::mutex mutex_;
    bool captureEnabled_ = false;
    size_t maxSamples_ = 0;
    std::deque<int64_t> pendingSubmits_;   // monotonic ns of unmatched submits
    std::vector<int64_t> latencySamples_;  // flip_ns - submit_ns

    // Convert DRM timestamp to nanoseconds
    static int64_t toNanoseconds(unsigned int sec, unsigned int usec);

    // Get current monotonic time in nanoseconds
    static int64_t getCurrentTimeNs();
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_PRESENTATIONTIMING_H

