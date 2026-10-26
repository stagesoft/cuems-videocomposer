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

#ifndef VIDEOCOMPOSER_DAYALIGN_H
#define VIDEOCOMPOSER_DAYALIGN_H

// Day alignment of engine layer offsets (24h MTC wrap).
//
// The engine sends /videocomposer/layer/<id>/offset <int> = -start, in 25 fps
// MTC frames of ITS 24h-extended MTC domain (the wraps it has seen since its
// last reset). This process's MTC head is extended by the wraps THIS process
// has seen (mtcreceiver applyWrap). The two differ when the videocomposer is
// restarted after the engine passed 24 h, after a STOP at wire 23-24 h or
// 0-1 h after a real wrap, or when the node engine restarts past 24 h while
// the videocomposer keeps running: the layer then stays black or plays a
// phase-shifted loop, with no log. Every engine offset is now converted into
// this process's day with MtcReceiver::toLocalDay() before it is used.
//
// Pure helpers, testable without GL or decoders (test/TestDayAlign.cpp).
// Plan: cuems-RELATIONS Plans/2026-10-25-mtc-day-align-24h-wrap.md, step 6.

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace videocomposer {
namespace dayalign {

// CUEMS MTC is 25 fps by contract, and the engine counts offsets in 25 fps
// frames whatever the wire says (MtcReceiver::curFrameRate is an unsigned
// char, 29.97 reads as 29), so a day is always 2 160 000 offset frames.
constexpr int64_t MTC_FPS = 25;
constexpr int64_t MS_PER_MTC_FRAME = 1000 / MTC_FPS;                   // 40
constexpr int64_t DAY_MS = 86400000;
constexpr int64_t FRAMES_PER_DAY = DAY_MS / MS_PER_MTC_FRAME;           // 2 160 000
constexpr int64_t HOUR_MS = 3600000;
constexpr int64_t UNRESOLVED_WARN_MS = 2000;
// Offsets beyond +-1000 days are not engine times (and * 40 must not overflow):
// they are applied as received, as before.
constexpr int64_t MAX_ALIGN_FRAMES = 1000 * FRAMES_PER_DAY;
static_assert(FRAMES_PER_DAY == 2160000, "a day of 25 fps MTC frames");
static_assert(FRAMES_PER_DAY * MS_PER_MTC_FRAME == DAY_MS, "whole frames per day");

// The engine time an offset refers to: the cue start, ref ms = -offset * 40.
// Only for |offsetFrames| <= MAX_ALIGN_FRAMES (inAlignRange): beyond that the
// product can overflow (signed overflow is UB) and the value is no engine time.
inline bool inAlignRange(int64_t offsetFrames) {
    return offsetFrames <= MAX_ALIGN_FRAMES && offsetFrames >= -MAX_ALIGN_FRAMES;
}
inline int64_t engineRefMs(int64_t offsetFrames) { return -offsetFrames * MS_PER_MTC_FRAME; }

// The offset in this process's day for day shift k. The start moves by
// -k days (localStart = start - k * DAY), so the offset (= -start) moves by
// +k days. k = 0 returns the received value itself.
inline int64_t localOffsetFrames(int64_t rawFrames, long k) {
    if (k == 0) return rawFrames;
    return rawFrames + static_cast<int64_t>(k) * FRAMES_PER_DAY;
}

// Resolve one raw engine offset. toLocalDay is bool(long engineMs, long&
// localMs, long& k): MtcReceiver::toLocalDay in the player, a fake in the
// tests. False (outputs untouched) while it cannot place the value: the
// caller keeps it pending and tries again on its next update.
template <typename ToLocalDay>
bool resolve(int64_t rawFrames, ToLocalDay&& toLocalDay, int64_t& localFrames, long& k) {
    if (!inAlignRange(rawFrames)) {
        localFrames = rawFrames;
        k = 0;
        return true;
    }
    long localMs = 0;
    long kk = 0;
    if (!toLocalDay(static_cast<long>(engineRefMs(rawFrames)), localMs, kk)) return false;
    localFrames = localOffsetFrames(rawFrames, kk);
    k = kk;
    return true;
}

// A resolved media position more than an hour before the media start or more
// than an hour past its end, right after an engine offset, means a wrong day
// (or an engine time far from now): worth one WARNING.
inline bool implausiblePosition(int64_t positionFrames, int64_t totalFrames, double fps) {
    const int64_t hourFrames = static_cast<int64_t>((fps > 0.0 ? fps : static_cast<double>(MTC_FPS)) * 3600.0);
    return positionFrames < -hourFrames || (totalFrames > 0 && positionFrames > totalFrames + hourFrames);
}

// Phase 1 (the engine sends its extended domain). A process born after the
// engine passed n days lags it: k >= 1, expected, INFO. This compositor
// outlives node-engine restarts, so k < 0 is expected too once it has seen a
// wrap of its own (head in day >= 1); with the head still in day 0 nothing
// explains the engine being a day behind: WARNING.
inline bool realignIsWarning(long k, long headMs) { return k < 0 && headMs < DAY_MS; }

inline std::string realignLine(long k, long engineMs, long headMs, unsigned long conversions) {
    return "MTC day realign k=" + std::to_string(k) + " (engine " + std::to_string(engineMs)
           + " ms, local head " + std::to_string(headMs) + " ms, phase=ext); "
           + std::to_string(conversions) + " conversions";
}

// Start-up banner. Off when no mtcreceiver driver is open at start: offsets
// are then applied as received (see mtcReceiverDrivers()).
inline const char* banner(bool receiverActive = true) {
    return receiverActive ? "MTC day-align: on (12 h rule, ext)"
                          : "MTC day-align: off (no mtcreceiver driver)";
}

// Process-wide log decisions (main loop only; the videocomposer polls OSC on
// its main loop, so every offset is received, resolved and logged there).
class RealignLog {
public:
    // A resolution with day shift k: true when the k line is due (the first
    // time this k is seen; k = 0 never logs), with the line and its level.
    bool onResolved(long k, long engineMs, long headMs, std::string& line, bool& warning) {
        if (k == 0) return false;
        ++conversions_;
        for (long seen : loggedK_) {
            if (seen == k) return false;
        }
        loggedK_.push_back(k);
        line = realignLine(k, engineMs, headMs, conversions_);
        warning = realignIsWarning(k, headMs);
        return true;
    }

    // The MTC frame rate seen at a resolution: one WARNING per process when it
    // is not the 25 fps of the CUEMS contract.
    bool onFrameRate(unsigned fps, std::string& line) {
        if (fps == static_cast<unsigned>(MTC_FPS) || fpsWarned_) return false;
        fpsWarned_ = true;
        line = "MTC day-align: MTC frame rate is " + std::to_string(fps)
               + " fps, not the 25 fps of the CUEMS contract; engine offsets are still read as"
                 " 25 fps frames (2160000 per day)";
        return true;
    }

    // An engine offset applied as received because no mtcreceiver driver is
    // open: one line per process, so the fail-open path is never silent.
    bool onAsReceived(int64_t rawFrames, std::string& line) {
        if (asReceivedLogged_) return false;
        asReceivedLogged_ = true;
        line = "MTC day-align: off (no mtcreceiver driver): engine offset " + std::to_string(rawFrames)
               + " applied as received, no 24h day alignment in this process";
        return true;
    }

    unsigned long conversions() const { return conversions_; }

private:
    std::vector<long> loggedK_;
    unsigned long conversions_ = 0;     // resolutions with k != 0
    bool fpsWarned_ = false;
    bool asReceivedLogged_ = false;
};

inline RealignLog& processLog() {
    static RealignLog log;
    return log;
}

// Open MtcReceiver-backed MIDI drivers. With none (no mtcreceiver in the
// build, or MIDI fell back to another driver) nothing will ever make the
// mtcreceiver head authoritative: offsets are then applied as received.
inline std::atomic<int>& mtcReceiverDrivers() {
    static std::atomic<int> count{0};
    return count;
}

// One layer's engine offset waiting for an authoritative MTC head. Never used
// raw: until resolve() succeeds the layer behaves as "not started yet". Any
// other offset writer clears it (latest wins).
class EngineOffsetLatch {
public:
    enum class Status { None, Pending, Resolved };
    struct Outcome {
        Status status = Status::None;
        int64_t frames = 0;     // the offset in this process's day (Resolved)
        long k = 0;             // day shift used (Resolved)
        long engineMs = 0;      // the engine start it refers to
    };

    // A new engine value: replaces any pending one and re-arms the once-per-
    // value warnings (a loop pass re-sends the offset).
    void latch(int64_t rawFrames, int64_t nowMs, std::string tag) {
        pending_ = true;
        raw_ = rawFrames;
        sinceMs_ = nowMs;
        tag_ = std::move(tag);
        unresolvedWarned_ = false;
        checkPosition_ = false;
    }

    // Another writer set the offset: forget the engine value.
    void clear() {
        pending_ = false;
        checkPosition_ = false;
    }

    bool pending() const { return pending_; }
    int64_t raw() const { return raw_; }
    const std::string& tag() const { return tag_; }

    template <typename ToLocalDay>
    Outcome tryResolve(ToLocalDay&& toLocalDay) {
        Outcome o;
        if (!pending_) return o;
        // Out of range: no engine time, and raw * 40 could overflow (UB).
        o.engineMs = inAlignRange(raw_) ? static_cast<long>(engineRefMs(raw_)) : 0;
        if (!resolve(raw_, toLocalDay, o.frames, o.k)) {
            o.status = Status::Pending;
            return o;
        }
        o.status = Status::Resolved;
        pending_ = false;
        checkPosition_ = true;
        lastK_ = o.k;
        lastEngineMs_ = o.engineMs;
        return o;
    }

    // True once per latched value when it is still unresolved after
    // UNRESOLVED_WARN_MS with MTC running; ageMs says for how long.
    bool unresolvedWarningDue(int64_t nowMs, bool mtcRunning, int64_t& ageMs) {
        if (!pending_ || unresolvedWarned_ || !mtcRunning) return false;
        ageMs = nowMs - sinceMs_;
        if (ageMs <= UNRESOLVED_WARN_MS) return false;
        unresolvedWarned_ = true;
        return true;
    }

    // True exactly once after each resolution: the first media position
    // computed with the new offset is the one to check for plausibility.
    bool takePositionCheck() {
        const bool due = checkPosition_;
        checkPosition_ = false;
        return due;
    }

    bool positionCheckDue() const { return checkPosition_; }
    long lastK() const { return lastK_; }
    long lastEngineMs() const { return lastEngineMs_; }

private:
    bool pending_ = false;
    int64_t raw_ = 0;
    int64_t sinceMs_ = 0;
    std::string tag_;
    bool unresolvedWarned_ = false;
    bool checkPosition_ = false;
    long lastK_ = 0;
    long lastEngineMs_ = 0;
};

} // namespace dayalign
} // namespace videocomposer

#endif // VIDEOCOMPOSER_DAYALIGN_H
