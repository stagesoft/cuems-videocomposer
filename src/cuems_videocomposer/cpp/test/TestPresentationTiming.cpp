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
#include <thread>

using namespace videocomposer;
using namespace videocomposer::test;

namespace {

// Build a (sec, usec, msc) tuple T_ms after some base instant, then call
// recordFlip. usec wraparound is handled by passing the absolute time in ms
// folded into sec/usec the way DRM events do.
void flipAtMs(PresentationTiming& pt, uint64_t total_ms, unsigned int msc) {
    unsigned int sec = static_cast<unsigned int>(total_ms / 1000ULL);
    unsigned int usec = static_cast<unsigned int>((total_ms % 1000ULL) * 1000ULL);
    pt.recordFlip(sec, usec, msc);
}

}  // namespace

bool test_PresentationTiming_CaptureDisabled_NoOp() {
    PresentationTiming pt;
    pt.init(60.0);
    // Capture starts disabled; recordSubmit must not collect anything.
    for (int i = 0; i < 50; ++i) {
        pt.recordSubmit();
        flipAtMs(pt, static_cast<uint64_t>(1000 + i * 16), static_cast<unsigned int>(i + 1));
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
    flipAtMs(pt, 1000, 1);
    flipAtMs(pt, 1008, 2);
    flipAtMs(pt, 1016, 3);

    auto stats = pt.getSwapChainLatencyStats();
    TEST_ASSERT_TRUE(stats.valid);
    TEST_ASSERT_EQ(stats.sampleCount, static_cast<size_t>(3));
    TEST_ASSERT_TRUE(stats.medianNs > 30 * 1000000LL);
    TEST_ASSERT_TRUE(stats.medianNs < 200 * 1000000LL);
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
    flipAtMs(pt, 1000, 1);

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
        flipAtMs(pt, static_cast<uint64_t>(i * 16), static_cast<unsigned int>(i + 1));
    }
    pt.recordSubmit();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    flipAtMs(pt, 9000, 99);

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
        flipAtMs(pt, static_cast<uint64_t>(i * 16), static_cast<unsigned int>(i + 1));
    }
    auto before = pt.getSwapChainLatencyStats();
    TEST_ASSERT_TRUE(before.sampleCount > 0);

    pt.reset();
    auto after = pt.getSwapChainLatencyStats();
    TEST_ASSERT_FALSE(after.valid);
    TEST_ASSERT_EQ(after.sampleCount, static_cast<size_t>(0));

    pt.recordSubmit();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    flipAtMs(pt, 100, 1);
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
        flipAtMs(pt, static_cast<uint64_t>(msc) * 16, ++msc);
        std::this_thread::sleep_for(std::chrono::microseconds(800));
    }
    stop.store(true);
    submitter.join();

    auto stats = pt.getSwapChainLatencyStats();
    TEST_ASSERT_TRUE(stats.sampleCount > 0);
    return true;
}
