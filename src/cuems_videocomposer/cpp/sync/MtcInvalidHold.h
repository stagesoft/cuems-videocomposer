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

#ifndef VIDEOCOMPOSER_MTCINVALIDHOLD_H
#define VIDEOCOMPOSER_MTCINVALIDHOLD_H

// Undecodable timecode (plan 869ejxng3 + 1amfwm7e6cw, D13, J5): the audio
// player's `invalid` rule (section 4.3a), for the videocomposer's MIDI sync.
//
// On doubled quarter frames (a second rtpmidid subscription) the atomic MTC
// head sawtooths: it moves on every quarter-frame message, valid or not, but
// no complete frame is ever decoded inside an anchored run. Followed raw, the
// layers hunt and every periodic full frame forces a seek. The rule:
//  - the head moves (it changed within the last 150 ms) and 800 ms have
//    passed without an anchored tick, counted from the later of the last
//    anchored tick and the moment the head resumed moving after a freeze (or
//    the start) -> HOLD: the sync source keeps returning the frame and time
//    it had at that instant, ERROR once;
//  - the hold is left on the first fresh anchored tick while the head moves.
// A frozen head (dropout, STOP) never enters the hold: today's path. A hold
// already in place stays through a freeze (the picture is frozen either way)
// and is left only by an anchored tick, so a doubled feed that resumes after
// a STOP does not hunt for 800 ms first.
//
// Pure: time, the head and the last tick sample are fed in (tests drive it
// with simulated time). Single caller thread (the render loop).

#include "QfSeqlock.h"
#include <algorithm>
#include <cstdint>

namespace videocomposer {

class MtcInvalidHold {
public:
    static constexpr int64_t FROZEN_NS = 150000000;    // 150 ms: the head stood still
    static constexpr int64_t INVALID_NS = 800000000;   // 800 ms without an anchor (J2)

    struct Edges {
        bool entered = false;     // HOLD begins: ERROR once
        bool left = false;        // an anchored tick ended it
        int64_t invalidForMs = 0; // on entered: ms without an anchor; on left: ms held
    };

    void start(int64_t nowNs, long head) {
        lastHead_ = head;
        lastMotionNs_ = nowNs - FROZEN_NS;   // not moving until it moves
        resumedNs_ = nowNs;
        lastAnchoredNs_ = INT64_MIN / 2;
        seenSeq_ = 0;
        held_ = false;
        heldSinceNs_ = 0;
        started_ = true;
    }

    Edges update(int64_t nowNs, long head, const TickSample& t) {
        if (!started_) start(nowNs, head);
        Edges e;
        const bool frozenBefore = (nowNs - lastMotionNs_) >= FROZEN_NS;
        if (head != lastHead_) {
            if (frozenBefore) resumedNs_ = nowNs;   // resumed after a freeze
            lastMotionNs_ = nowNs;
            lastHead_ = head;
        }
        const bool frozen = (nowNs - lastMotionNs_) >= FROZEN_NS;
        bool freshAnchor = false;
        if (t.seq != seenSeq_) {
            seenSeq_ = t.seq;
            if (t.anchored) {
                lastAnchoredNs_ = t.monoNs;
                freshAnchor = true;
            }
        }
        if (held_) {
            if (freshAnchor && !frozen) {
                held_ = false;
                e.left = true;
                e.invalidForMs = (nowNs - heldSinceNs_) / 1000000;
            }
            return e;
        }
        if (frozen) {
            return e;
        }
        const int64_t since = std::max(lastAnchoredNs_, resumedNs_);
        if (nowNs - since >= INVALID_NS) {
            held_ = true;
            heldSinceNs_ = nowNs;
            e.entered = true;
            e.invalidForMs = (nowNs - since) / 1000000;
        }
        return e;
    }

    bool held() const { return held_; }

private:
    bool started_ = false;
    bool held_ = false;
    long lastHead_ = 0;
    int64_t lastMotionNs_ = 0;
    int64_t resumedNs_ = 0;
    int64_t lastAnchoredNs_ = 0;
    int64_t heldSinceNs_ = 0;
    uint64_t seenSeq_ = 0;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_MTCINVALIDHOLD_H
