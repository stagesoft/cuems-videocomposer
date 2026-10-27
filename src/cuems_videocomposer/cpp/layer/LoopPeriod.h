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
// num den [frames]. The period P = num / den seconds is the one the audio
// player wraps on (the decoded audio length of an A/V file, D2), so the
// layer wraps on it too and both ends turn at the same MTC instant.
//
// Pure arithmetic, no state of its own: everything in integer rationals,
// cross-multiplied, in __int128 (K14, L12d). With Pf = P x fps frames not
// representable, floor(F / Pf) in doubles is one frame late exactly when
// k x Pf is an integer, and band 2's F x realFrames overflows int64 past
// ~2 h at 1001-denominator rates.

#include <cstdint>
#include <cstdlib>

namespace videocomposer {
namespace loopperiod {

// The three bands of D2/D8 (how the period relates to the video's own
// frame count realFrames).
enum class Band {
    Hold,      // |Pf - realFrames| <= 2 frames: hold (or cut) at the loop cut
    Stretch,   // > 2 frames and <= 1 %: stretch the pass over Pf (long files)
    Beyond     // > 2 frames and > 1 %: applied as Hold, ERROR once (H9, I4)
};

inline const char* bandName(Band b) {
    switch (b) {
    case Band::Hold: return "1";
    case Band::Stretch: return "2";
    case Band::Beyond: return "beyond";
    }
    return "?";
}

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

inline Band band(const Period& p, const Rate& r, int64_t realFrames) {
    // |Pf - R| <= 2  <=>  |num fps_num - R den fps_den| <= 2 den fps_den
    const i128 a = pfNum(p, r);
    const i128 b = pfDen(p, r);
    i128 diff = a - (i128)realFrames * b;
    if (diff < 0) diff = -diff;
    if (diff <= 2 * b) return Band::Hold;
    // stretch > 1 %  <=>  100 |Pf - R| > R
    if (100 * diff > (i128)realFrames * b) return Band::Beyond;
    return Band::Stretch;
}

// Stretch of the period against realFrames, in percent (for logs only).
inline double stretchPercent(const Period& p, const Rate& r, int64_t realFrames) {
    if (realFrames <= 0) return 0.0;
    const double pf = pfFrames(p, r);
    return 100.0 * (pf - (double)realFrames) / (double)realFrames;
}

struct Shown {
    int64_t frame = 0;   // the frame to show
    int64_t pass = 0;    // k: the pass index (0 = first); -1 before the start
    bool ended = false;  // a finite loop is past its last pass
};

// The frame shown for the layer position F (video frames since the cue's
// start, offset applied) under period p at rate r with `realFrames` frames
// and `passes` passes (<= 0: forever). Section 4.4:
//   F < 0                    -> frame 0 (before the start, G21)
//   k = floor(F / Pf)
//   passes > 0 and k >= passes -> realFrames - 1 (the engine ends the cue)
//   band 1 / beyond          -> min(floor(F - k Pf), realFrames - 1)
//   band 2                   -> min(floor((F - k Pf) x realFrames / Pf), realFrames - 1)
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
    if (passes > 0 && k >= passes) {
        s.frame = last;
        s.ended = true;
        return s;
    }
    // f_cont x b = F b - k a  (in [0, a))
    const i128 rem = (i128)F * b - k * a;
    i128 f;
    if (band(p, r, realFrames) == Band::Stretch) {
        f = floorDiv(rem * realFrames, a);
    } else {
        f = floorDiv(rem, b);
    }
    s.frame = f > last ? last : (int64_t)f;
    return s;
}

} // namespace loopperiod
} // namespace videocomposer

#endif // VIDEOCOMPOSER_LOOPPERIOD_H
