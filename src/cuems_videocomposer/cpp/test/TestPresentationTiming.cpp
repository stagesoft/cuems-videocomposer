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

#include "TestFramework.h"
#include "../display/drm/PresentationTiming.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <thread>

using namespace videocomposer;
using namespace videocomposer::test;

namespace {

// Synthetic flip "right now" in CLOCK_MONOTONIC, encoded into the (sec, usec)
// DRM event payload. recordFlip pairs against current_.ust (kernel hardware
// timestamp) for the swap-chain delta, so test flip timestamps must live in
// the same clock domain as recordSubmit's getCurrentTimeNs().
void flipNow(PresentationTiming& pt, unsigned int msc) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t total_us = static_cast<int64_t>(ts.tv_sec) * 1000000LL + ts.tv_nsec / 1000LL;
    unsigned int sec = static_cast<unsigned int>(total_us / 1000000LL);
    unsigned int usec = static_cast<unsigned int>(total_us % 1000000LL);
    pt.recordFlip(sec, usec, msc);
}

// Synthetic flip with a deliberately-zero ust (sec=0, usec=0). Used to
// verify the "kernel didn't supply a timestamp" guard added in Commit 9.
void flipWithZeroUst(PresentationTiming& pt, unsigned int msc) {
    pt.recordFlip(0u, 0u, msc);
}

}  // namespace

bool test_PresentationTiming_CaptureDisabled_NoOp() {
    PresentationTiming pt;
    pt.init(60.0);
    // Capture starts disabled; recordSubmit must not collect anything.
    for (int i = 0; i < 50; ++i) {
        pt.recordSubmit();
        flipNow(pt, static_cast<unsigned int>(i + 1));
    }
    auto stats = pt.getSwapChainLatencyStats();
    TEST_ASSERT_FALSE(stats.valid);
    TEST_ASSERT_EQ(stats.sampleCount, static_cast<size_t>(0));
    return true;
}

bool test_PresentationTiming_FifoPairing() {
    PresentationTiming pt;
    pt.init(60.0);
    pt.enableLatencyCapture(64);

    pt.recordSubmit();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    pt.recordSubmit();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    pt.recordSubmit();

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    flipNow(pt, 1);
    flipNow(pt, 2);
    flipNow(pt, 3);

    auto stats = pt.getSwapChainLatencyStats();
    TEST_ASSERT_TRUE(stats.valid);
    TEST_ASSERT_EQ(stats.sampleCount, static_cast<size_t>(3));
    // Submits at t0, t0+5, t0+10; flips around t0+50.
    // Pair 1: ~50 ms; pair 2: ~45 ms; pair 3: ~40 ms.
    TEST_ASSERT_TRUE(stats.medianNs > 30 * 1000000LL);
    TEST_ASSERT_TRUE(stats.medianNs < 200 * 1000000LL);
    return true;
}

bool test_PresentationTiming_FifoPairing_UsesKernelUst() {
    // Commit 9 specific: confirm recordFlip pairs against current_.ust
    // (kernel timestamp from sec/usec) and NOT against display_time
    // (userspace receipt time). With ust == 0 the sample MUST be
    // dropped, even though display_time would have produced a
    // legitimate-looking positive delta.
    PresentationTiming pt;
    pt.init(60.0);
    pt.enableLatencyCapture(64);

    pt.recordSubmit();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    flipWithZeroUst(pt, 1);

    auto stats = pt.getSwapChainLatencyStats();
    TEST_ASSERT_EQ(stats.sampleCount, static_cast<size_t>(0));

    // Now a legitimate flipNow should pair against the second submit.
    pt.recordSubmit();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    flipNow(pt, 2);
    auto stats2 = pt.getSwapChainLatencyStats();
    TEST_ASSERT_EQ(stats2.sampleCount, static_cast<size_t>(1));
    TEST_ASSERT_TRUE(stats2.medianNs > 0);
    return true;
}

bool test_PresentationTiming_DiscardPendingSubmit() {
    PresentationTiming pt;
    pt.init(60.0);
    pt.enableLatencyCapture(64);

    pt.recordSubmit();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    pt.recordSubmit();
    pt.discardPendingSubmit();

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    flipNow(pt, 1);

    auto stats = pt.getSwapChainLatencyStats();
    TEST_ASSERT_EQ(stats.sampleCount, static_cast<size_t>(1));
    TEST_ASSERT_TRUE(stats.medianNs > 5 * 1000000LL);
    TEST_ASSERT_TRUE(stats.medianNs < 50 * 1000000LL);
    return true;
}

bool test_PresentationTiming_StatisticsMedianAndP95() {
    PresentationTiming pt;
    pt.init(60.0);
    pt.enableLatencyCapture(64);

    for (int i = 0; i < 20; ++i) {
        pt.recordSubmit();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        flipNow(pt, static_cast<unsigned int>(i + 1));
    }
    pt.recordSubmit();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    flipNow(pt, 99);

    auto stats = pt.getSwapChainLatencyStats();
    TEST_ASSERT_TRUE(stats.valid);
    TEST_ASSERT_EQ(stats.sampleCount, static_cast<size_t>(21));
    TEST_ASSERT_TRUE(stats.medianNs > 0);
    TEST_ASSERT_TRUE(stats.p95Ns >= stats.medianNs);
    TEST_ASSERT_TRUE(stats.expectedVsyncNs > 0);
    return true;
}

bool test_PresentationTiming_ResetClearsState() {
    PresentationTiming pt;
    pt.init(60.0);
    pt.enableLatencyCapture(64);
    for (int i = 0; i < 5; ++i) {
        pt.recordSubmit();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        flipNow(pt, static_cast<unsigned int>(i + 1));
    }
    auto before = pt.getSwapChainLatencyStats();
    TEST_ASSERT_TRUE(before.sampleCount > 0);

    pt.reset();
    auto after = pt.getSwapChainLatencyStats();
    TEST_ASSERT_FALSE(after.valid);
    TEST_ASSERT_EQ(after.sampleCount, static_cast<size_t>(0));

    pt.recordSubmit();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    flipNow(pt, 1);
    auto resumed = pt.getSwapChainLatencyStats();
    TEST_ASSERT_EQ(resumed.sampleCount, static_cast<size_t>(1));
    return true;
}

bool test_PresentationTiming_ConcurrentSubmitFlip() {
    PresentationTiming pt;
    pt.init(60.0);
    pt.enableLatencyCapture(1024);
    std::atomic<bool> stop{false};

    std::thread submitter([&]() {
        while (!stop.load()) {
            pt.recordSubmit();
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });

    unsigned int msc = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    while (std::chrono::steady_clock::now() < deadline) {
        flipNow(pt, ++msc);
        std::this_thread::sleep_for(std::chrono::microseconds(800));
    }
    stop.store(true);
    submitter.join();

    auto stats = pt.getSwapChainLatencyStats();
    TEST_ASSERT_TRUE(stats.sampleCount > 0);
    return true;
}
