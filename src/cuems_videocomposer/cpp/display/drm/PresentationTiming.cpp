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
 * PresentationTiming.cpp - Frame pacing implementation
 */

#include "PresentationTiming.h"
#include "../../utils/Logger.h"
#include <algorithm>
#include <ctime>

namespace videocomposer {

PresentationTiming::PresentationTiming() {
    reset();
}

std::string PresentationTiming::tag() const {
    if (outputName_.empty()) {
        return "PresentationTiming";
    }
    return "PresentationTiming[" + outputName_ + "]";
}

void PresentationTiming::init(double refreshHz, std::string outputName) {
    reset();
    // After reset(): reset() deliberately leaves identity alone, but setting
    // it here keeps init() the single place that establishes both.
    outputName_ = std::move(outputName);

    if (refreshHz > 0) {
        // Calculate expected vsync duration in nanoseconds
        // e.g., 60Hz -> 16,666,667 ns
        expectedVsyncNs_ = static_cast<int64_t>(1e9 / refreshHz);
        displayHz_ = refreshHz;
        initialized_ = true;

        LOG_INFO << tag() << ": Initialized for " << refreshHz
                 << "Hz (vsync=" << (expectedVsyncNs_ / 1000000.0) << "ms)";
    }
}

void PresentationTiming::recordFlip(unsigned int sec, unsigned int usec, unsigned int msc) {
    // Store previous entry
    previous_ = current_;

    // Record new timing
    current_.ust = toNanoseconds(sec, usec);
    current_.msc = static_cast<int64_t>(msc);
    current_.display_time = getCurrentTimeNs();
    current_.valid = true;

    // Calculate vsync duration and skipped frames if we have a previous entry
    if (previous_.valid && previous_.ust > 0 && current_.ust > previous_.ust) {
        int64_t ust_delta = current_.ust - previous_.ust;
        int64_t msc_delta = current_.msc - previous_.msc;

        // Avoid division by zero
        if (msc_delta > 0) {
            // Calculate actual vsync duration
            current_.vsync_duration = ust_delta / msc_delta;

            // Detect skipped vsyncs (msc_delta > 1 means we missed frames)
            // msc_delta of 1 = perfect, 2 = 1 skipped, etc.
            current_.skipped_vsyncs = msc_delta - 1;

            // One skipped vsync is timing jitter; more than one is a real drop.
            // This surface's own count -- every DRMSurface owns its
            // PresentationTiming, so the number below is NOT a fleet-wide total.
            if (current_.skipped_vsyncs > 1) {
                totalUnexpectedDrops_ += current_.skipped_vsyncs;
                if (totalUnexpectedDrops_ <= 5 || totalUnexpectedDrops_ % 60 == 0) {
                    LOG_WARNING << tag() << ": Dropped " << current_.skipped_vsyncs
                               << " frame(s) beyond expected (total unexpected: "
                               << totalUnexpectedDrops_ << ")";
                }
            }
        } else if (msc_delta == 0) {
            // Same vsync - this shouldn't happen with proper page flipping
            // Keep previous vsync duration
            current_.vsync_duration = previous_.vsync_duration;
            current_.skipped_vsyncs = 0;
        } else {
            // msc went backwards (counter wrapped or reset) - reset timing
            current_.vsync_duration = expectedVsyncNs_;
            current_.skipped_vsyncs = 0;
        }
    } else if (expectedVsyncNs_ > 0) {
        // First frame or invalid previous - use expected duration
        current_.vsync_duration = expectedVsyncNs_;
        current_.skipped_vsyncs = 0;
    }

    // Pair with the front pending submit (FIFO per surface) and record the
    // submit→flip latency sample. Capture-disabled = pendingSubmits_ stays
    // empty, so this branch is also a no-op on the production hot path.
    //
    // The flip side of the pair uses current_.ust — the kernel's hardware
    // vsync timestamp (CLOCK_MONOTONIC, same domain as recordSubmit's
    // getCurrentTimeNs). This measures userspace-submit → actual hardware
    // flip; using current_.display_time instead would add the kernel-to-
    // userspace event-delivery latency (~100-500 µs of jitter).
    //
    // Skip the sample if the kernel didn't supply a usable timestamp
    // (current_.ust == 0) — rare, but seen on some atomic-only drivers.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (captureEnabled_ && !pendingSubmits_.empty()) {
            int64_t submit_ns = pendingSubmits_.front();
            pendingSubmits_.pop_front();
            if (current_.ust > 0) {
                int64_t delta = current_.ust - submit_ns;
                if (delta > 0) {
                    if (latencySamples_.size() >= maxSamples_ && maxSamples_ > 0) {
                        latencySamples_.erase(latencySamples_.begin());
                    }
                    latencySamples_.push_back(delta);
                }
            }
        }
    }
}

void PresentationTiming::recordSubmit() {
    // Hot-path early-return when capture is off — no allocation, no lock
    // contention with the render loop.
    if (!captureEnabled_) {
        return;
    }
    int64_t now = getCurrentTimeNs();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!captureEnabled_) {
        return;
    }
    pendingSubmits_.push_back(now);
}

void PresentationTiming::discardPendingSubmit() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pendingSubmits_.empty()) {
        pendingSubmits_.pop_front();
    }
}

void PresentationTiming::enableLatencyCapture(size_t maxSamples) {
    std::lock_guard<std::mutex> lock(mutex_);
    captureEnabled_ = true;
    maxSamples_ = (maxSamples == 0) ? 256 : maxSamples;
}

void PresentationTiming::disableLatencyCapture() {
    std::lock_guard<std::mutex> lock(mutex_);
    captureEnabled_ = false;
    pendingSubmits_.clear();
}

LatencyStats PresentationTiming::getSwapChainLatencyStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    LatencyStats stats;
    stats.expectedVsyncNs = expectedVsyncNs_;
    stats.sampleCount = latencySamples_.size();
    if (latencySamples_.empty()) {
        return stats;
    }
    std::vector<int64_t> sorted(latencySamples_);
    std::sort(sorted.begin(), sorted.end());
    stats.medianNs = sorted[sorted.size() / 2];
    size_t p95idx = static_cast<size_t>(0.95 * (sorted.size() - 1));
    stats.p95Ns = sorted[p95idx];
    stats.valid = true;
    return stats;
}

PresentationEntry PresentationTiming::getInfo() const {
    return current_;
}

void PresentationTiming::reset() {
    current_ = PresentationEntry();
    previous_ = PresentationEntry();
    totalUnexpectedDrops_ = 0;
    // Keep expectedVsyncNs_, displayHz_, initialized_ - they're set by init()

    std::lock_guard<std::mutex> lock(mutex_);
    pendingSubmits_.clear();
    latencySamples_.clear();
    // captureEnabled_ / maxSamples_ preserved across reset() — caller controls them via enable/disable.
}

int64_t PresentationTiming::toNanoseconds(unsigned int sec, unsigned int usec) {
    // Convert seconds + microseconds to nanoseconds
    return static_cast<int64_t>(sec) * 1000000000LL +
           static_cast<int64_t>(usec) * 1000LL;
}

int64_t PresentationTiming::getCurrentTimeNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

} // namespace videocomposer

