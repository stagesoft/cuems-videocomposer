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

#ifndef VIDEOCOMPOSER_LOOPPERIOD_H
#define VIDEOCOMPOSER_LOOPPERIOD_H

// The engine's loop period for a looping VideoCue (plan 869ejxng3 +
// 1amfwm7e6cw, section 4.4): /videocomposer/layer/<id>/loop/period
// num den [frames]. P = num / den seconds is the length the engine chose for
// this cue (D15, rev 3.17, section 4.1a): its own track (P = V, Pf =
// realFrames exactly), the audio length of a file whose tracks are within
// the slack threshold, or a same-file pair's choice (the audio length within
// 0.5 s, the longest track beyond). The audio player of a pair wraps on the
// same P, so both ends turn at the same MTC instant.
//
// What the layer does with the tail, P against the video's realFrames:
//   |Pf - realFrames| <= T = max(2 frames, 80 ms)  the existing clamp: held or
//                                                   cut, invisible at the jump
//   Pf > realFrames, tail <= HOLD_TAIL_MAX_MS        the last frame held
//   Pf > realFrames, tail >  HOLD_TAIL_MAX_MS        nothing drawn (Shown::blank)
//   Pf < realFrames by <= HOLD_TAIL_MAX_MS           cut at Pf, INFO once (R23)
//   Pf < realFrames by >  HOLD_TAIL_MAX_MS           cut at Pf, ERROR once (a
//                                                   bad sidecar or the test knob)
// Nothing is ever stretched (D8's bands 2-3 are gone).
//
// Pure arithmetic, no state of its own: everything in integer rationals,
// cross-multiplied, in __int128 (K14, L12d). With Pf = P x fps frames not
// representable, floor(F / Pf) in doubles is one frame late exactly when
// k x Pf is an integer.

#include <cstdint>
#include <cstdlib>

namespace videocomposer {
namespace loopperiod {

// S, the content threshold of D15 (rev 3.17): a tail up to this is slack
// (held, or cut), beyond it is content (not drawn). The engine's
// PAIR_SLACK_MS (cuemsengine/cues/loop_mode.py) is the same value: it decides
// P, this decides the picture; a mismatch could never touch sync.
constexpr int64_t HOLD_TAIL_MAX_MS = 500;
// T, the slack threshold (R5): max(2 frames, 80 ms).
constexpr int64_t SLACK_FRAMES = 2;
constexpr int64_t SLACK_FLOOR_MS = 80;

struct Period {
    int64_t num = 0;         // P = num / den seconds
    int64_t den = 0;
    int64_t realFrames = 0;  // the engine's frame count (0 = not sent: J12)
    bool valid() const { return num > 0 && den > 0; }
    bool operator==(const Period& o) const {
        return num == o.num && den == o.den && realFrames == o.realFrames;
    }
    bool operator!=(const Period& o) const { return !(*this == o); }
};

// The layer's frame rate as a rational (r_frame_rate, num / den fps).
struct Rate {
    int64_t num = 0;
    int64_t den = 0;
    bool valid() const { return num > 0 && den > 0; }
};

using i128 = __int128;

// Pf as a fraction of frames: Pf = (num x fps_num) / (den x fps_den).
inline i128 pfNum(const Period& p, const Rate& r) { return (i128)p.num * r.num; }
inline i128 pfDen(const Period& p, const Rate& r) { return (i128)p.den * r.den; }

// floor(a / b) for b > 0, any sign of a.
inline i128 floorDiv(i128 a, i128 b) {
    i128 q = a / b;
    if ((a % b != 0) && (a < 0)) --q;
    return q;
}

// Pf in frames, rounded (for logs only).
inline double pfFrames(const Period& p, const Rate& r) {
    if (!p.valid() || !r.valid()) return 0.0;
    return (double)p.num * (double)r.num / ((double)p.den * (double)r.den);
}

// The tail, Pf - realFrames, as a numerator over pfDen(): > 0 the period runs
// past the video's last frame, < 0 the video is longer than the period.
inline i128 tailNum(const Period& p, const Rate& r, int64_t realFrames) {
    return pfNum(p, r) - (i128)realFrames * pfDen(p, r);
}

inline i128 absI(i128 x) { return x < 0 ? -x : x; }

// |tail| > boundMs:  |t| / b frames x 1000 fps_den / fps_num > boundMs
//               <=>  |t| x 1000 x fps_den > boundMs x b x fps_num
inline bool tailBeyondMs(i128 t, const Period& p, const Rate& r, int64_t boundMs) {
    return absI(t) * 1000 * r.den > (i128)boundMs * pfDen(p, r) * r.num;
}

// The tail in ms, signed (for logs only).
inline double tailMs(const Period& p, const Rate& r, int64_t realFrames) {
    if (!p.valid() || !r.valid()) return 0.0;
    return (pfFrames(p, r) - (double)realFrames) * 1000.0 * (double)r.den / (double)r.num;
}

// |tail| <= T = max(2 frames, 80 ms).
inline bool withinSlack(i128 t, const Period& p, const Rate& r) {
    return absI(t) <= (i128)SLACK_FRAMES * pfDen(p, r) || !tailBeyondMs(t, p, r, SLACK_FLOOR_MS);
}

// A tail the layer does not draw: the period runs past the video's end by
// more than HOLD_TAIL_MAX_MS.
inline bool darkTail(const Period& p, const Rate& r, int64_t realFrames) {
    const i128 t = tailNum(p, r, realFrames);
    return t > 0 && tailBeyondMs(t, p, r, HOLD_TAIL_MAX_MS);
}

enum class TailKind {
    Exact,      // Pf == realFrames
    Slack,      // |tail| <= T: held or cut, the existing clamp
    Held,       // T < tail <= HOLD_TAIL_MAX_MS: the last frame held
    NotDrawn,   // tail > HOLD_TAIL_MAX_MS: nothing drawn until the loop point
    Cut,        // T < -tail <= HOLD_TAIL_MAX_MS: cut at Pf (a pair, R23), INFO
    CutBeyond   // -tail > HOLD_TAIL_MAX_MS: cut at Pf, ERROR (cannot come from D15)
};

inline TailKind tailKind(const Period& p, const Rate& r, int64_t realFrames) {
    const i128 t = tailNum(p, r, realFrames);
    if (t == 0) return TailKind::Exact;
    if (withinSlack(t, p, r)) return TailKind::Slack;
    const bool beyond = tailBeyondMs(t, p, r, HOLD_TAIL_MAX_MS);
    if (t > 0) return beyond ? TailKind::NotDrawn : TailKind::Held;
    return beyond ? TailKind::CutBeyond : TailKind::Cut;
}

inline const char* tailKindName(TailKind k) {
    switch (k) {
    case TailKind::Exact: return "none";
    case TailKind::Slack: return "within the slack threshold";
    case TailKind::Held: return "held";
    case TailKind::NotDrawn: return "not drawn";
    case TailKind::Cut: return "cut";
    case TailKind::CutBeyond: return "cut beyond the bound";
    }
    return "?";
}

struct Shown {
    int64_t frame = 0;   // the frame to show (the last one while blank)
    int64_t pass = 0;    // k: the pass index (0 = first); -1 before the start
    bool ended = false;  // a finite loop is past its last pass
    bool blank = false;  // draw nothing: a dark tail, or the end after one (R2)
};

// The frame shown for the layer position F (video frames since the cue's
// start, offset applied) under period p at rate r with `realFrames` frames
// and `passes` passes (<= 0: forever). Section 4.4:
//   F < 0                       -> frame 0 (before the start, G21)
//   k = floor(F / Pf)
//   passes > 0 and k >= passes  -> ended: the last frame, blank after a dark tail
//   f = floor(F - k Pf)
//   f < realFrames              -> f
//   otherwise                   -> the last frame; blank when the tail is dark
// A period shorter than the video simply wraps before its end (cut).
inline Shown shownFrame(int64_t F, const Period& p, const Rate& r, int64_t realFrames, int passes) {
    Shown s;
    if (F < 0) {
        s.frame = 0;
        s.pass = -1;
        return s;
    }
    const i128 a = pfNum(p, r);   // Pf = a / b
    const i128 b = pfDen(p, r);
    const i128 k = floorDiv((i128)F * b, a);
    s.pass = (int64_t)k;
    const int64_t last = realFrames > 0 ? realFrames - 1 : 0;
    const bool dark = realFrames > 0 && darkTail(p, r, realFrames);
    if (passes > 0 && k >= passes) {
        s.frame = last;
        s.ended = true;
        s.blank = dark;
        return s;
    }
    // f_cont x b = F b - k a  (in [0, a))
    const i128 f = floorDiv((i128)F * b - k * a, b);
    if (f > last) {
        s.frame = last;
        s.blank = dark;
    } else {
        s.frame = (int64_t)f;
    }
    return s;
}

} // namespace loopperiod
} // namespace videocomposer

#endif // VIDEOCOMPOSER_LOOPPERIOD_H
