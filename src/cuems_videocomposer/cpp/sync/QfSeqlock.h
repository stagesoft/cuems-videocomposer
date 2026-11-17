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

// The trusted-tick seqlock of plan 869ejxng3 + 1amfwm7e6cw, section 4.3a,
// the same rule as cuems-audioplayer's src/qfseqlock.h (D13: the whole node
// judges the quarter frames alike), copied so the two stay byte-comparable.
//
// The writer is mtcreceiver's tick callback (the MIDI thread; it fires only on
// a valid quarter frame). It keeps the anchored run: `run` counts consecutive
// ticks that are BOTH at most 50 ms apart and head-continuous (each head =
// the previous + 10 ms +- 1, the decoder's per-message increment at 25 fps;
// a snap breaks the run), and `anchored` = "a complete tick arrived with
// run >= 8", cleared whenever the run restarts. A tick is trusted only while
// anchored. The reader (here the render thread) retries at most 3 times and
// otherwise keeps its previous sample. `anchored` rides on every later tick,
// so a reader that misses a tick loses nothing.
#ifndef VIDEOCOMPOSER_QFSEQLOCK_H
#define VIDEOCOMPOSER_QFSEQLOCK_H

#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace videocomposer {

struct TickSample {
    int64_t monoNs = 0;     // CLOCK_MONOTONIC of the tick
    long headMs = 0;        // the head the tick reported
    bool anchored = false;  // inside an anchored run: trusted
    bool complete = false;  // the tick completed a frame (QF 8)
    uint32_t run = 0;
    uint64_t seq = 0;       // ticks published so far (0 = none)
};

class TickSeqlock {
public:
    static constexpr int64_t GAP_NS = 50000000;   // 50 ms (J1)
    static constexpr long STEP_MS = 10;            // 250 / 25 fps
    static constexpr long STEP_TOL_MS = 1;
    static constexpr uint32_t ANCHOR_RUN = 8;

    // Writer (one thread only).
    void onTick(long headMs, bool complete, int64_t monoNs)
    {
        const bool gapOk = have_ && (monoNs - lastMono_) <= GAP_NS;
        const bool contOk = have_ && std::labs(headMs - (lastHead_ + STEP_MS)) <= STEP_TOL_MS;
        if (gapOk && contOk) {
            run_++;
        } else {
            run_ = 1;
            anchored_ = false;
        }
        if (complete && run_ >= ANCHOR_RUN) {
            anchored_ = true;
        }
        have_ = true;
        lastMono_ = monoNs;
        lastHead_ = headMs;
        count_++;
        // publish: odd sequence word while writing
        const uint64_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        mono_.store(monoNs, std::memory_order_relaxed);
        head_.store(headMs, std::memory_order_relaxed);
        flags_.store((anchored_ ? 1u : 0u) | (complete ? 2u : 0u) | (run_ << 2), std::memory_order_relaxed);
        cnt_.store(count_, std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);
    }

    // Reader: true when `out` was refreshed.
    bool read(TickSample& out) const
    {
        for (int attempt = 0; attempt < 3; attempt++) {
            const uint64_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1) continue;
            TickSample t;
            t.monoNs = mono_.load(std::memory_order_relaxed);
            t.headMs = head_.load(std::memory_order_relaxed);
            const uint32_t f = flags_.load(std::memory_order_relaxed);
            t.seq = cnt_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) != s1) continue;
            t.anchored = (f & 1u) != 0;
            t.complete = (f & 2u) != 0;
            t.run = f >> 2;
            out = t;
            return true;
        }
        return false;
    }

    // Writer-side reset (a new process starts unanchored; tests).
    void reset()
    {
        have_ = false;
        run_ = 0;
        anchored_ = false;
    }

private:
    std::atomic<uint64_t> seq_{0};
    std::atomic<int64_t> mono_{0};
    std::atomic<long> head_{0};
    std::atomic<uint32_t> flags_{0};
    std::atomic<uint64_t> cnt_{0};
    // writer-only
    bool have_ = false;
    int64_t lastMono_ = 0;
    long lastHead_ = 0;
    uint32_t run_ = 0;
    bool anchored_ = false;
    uint64_t count_ = 0;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_QFSEQLOCK_H
