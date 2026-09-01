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

#include "SaturationSignals.h"

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>

namespace videocomposer {
namespace signals {

namespace {

std::atomic<long> g_framesMissed{0};
std::atomic<long> g_framesHeldLong{0};

/**
 * Recent per-layer decode errors.
 *
 * A tiny fixed ring, never grown and never allocating, because a decode-error
 * burst is exactly the moment when the process is under memory pressure. Only
 * the distinct-layer count within a window is ever asked for, so 32 slots is
 * ample: the question is "one sick file or the whole GPU", and the answer
 * saturates at two.
 */
constexpr size_t ERROR_RING = 32;

struct ErrorEvent {
    uint64_t layerKey = 0;
    long long atMs = 0;
};

std::mutex g_errorMutex;
std::array<ErrorEvent, ERROR_RING> g_errors{};
size_t g_errorNext = 0;

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

void frameMissed() {
    g_framesMissed.fetch_add(1, std::memory_order_relaxed);
}

void frameHeldLong() {
    g_framesHeldLong.fetch_add(1, std::memory_order_relaxed);
}

void decodeErrorOnLayer(uint64_t layerKey) {
    std::lock_guard<std::mutex> lock(g_errorMutex);
    g_errors[g_errorNext] = ErrorEvent{layerKey, nowMs()};
    g_errorNext = (g_errorNext + 1) % ERROR_RING;
}

long framesMissed() {
    return g_framesMissed.load(std::memory_order_relaxed);
}

long framesHeldLong() {
    return g_framesHeldLong.load(std::memory_order_relaxed);
}

int layersErroringWithin(long windowMs) {
    const long long cutoff = nowMs() - windowMs;

    std::array<uint64_t, ERROR_RING> seen{};
    size_t seenCount = 0;

    std::lock_guard<std::mutex> lock(g_errorMutex);
    for (const ErrorEvent& e : g_errors) {
        if (e.atMs == 0 || e.atMs < cutoff) continue;
        bool already = false;
        for (size_t i = 0; i < seenCount; ++i) {
            if (seen[i] == e.layerKey) { already = true; break; }
        }
        if (!already) {
            seen[seenCount++] = e.layerKey;
        }
    }
    return static_cast<int>(seenCount);
}

void resetForTest() {
    g_framesMissed.store(0, std::memory_order_relaxed);
    g_framesHeldLong.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_errorMutex);
    g_errors.fill(ErrorEvent{});
    g_errorNext = 0;
}

} // namespace signals
} // namespace videocomposer
