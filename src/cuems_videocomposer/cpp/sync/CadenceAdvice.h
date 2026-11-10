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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file CadenceAdvice.h
 * @brief The refresh rule per content fps, said at layer load (plan 1amfwm7gq3y §4.2).
 *
 * cuems-RELATIONS Plans/2026-11-06-refresh-rule-and-warning-1amfwm7gq3y.md. A pure function: from the content's fps
 * and the vsync period the layer will be paced on, which cadence the layer gets (exact hold, near-integer with a
 * correction every ~s seconds, non-integer pulldown, drop regime) and the journal line that says so. No behaviour
 * changes: the cadence engine decides on its own at reveal; this only tells the operator before the GO.
 */

#pragma once

#include <cstdint>
#include <string>

namespace videocomposer {
namespace cadence {

/// |r − round r| at or below this = an integer hold pattern (the cadence engine's kExactTol).
constexpr double kNearIntegerTol = 0.02;
/// r below this, not near-integer = the drop regime (the cadence engine's kDropTol).
constexpr double kDropTol = 0.98;
/// Relative mismatch at or below this = exact: below the clocks' own drift (26–80 ppm measured, research §D.11), so
/// the correction it would cause is the crystal's, not the mode's (e.g. 29.97p on a 148.352 MHz "59.94" mode).
constexpr double kExactRelTol = 10e-6;
/// A near-integer ratio is a WARNING when its correction interval is shorter than this, INFO otherwise.
/// D2, decided by Ion 2026-11-10 from the eye test: 60 s. A single-vsync correction was seen on panels every 4.8 s
/// (Acer 59.79 Hz) and every 16.2 s (LG at 59.94 Hz, 30p), not at 60.000 Hz (one every few minutes). A projector at
/// 59.95 Hz hid it (it re-times the signal), so the 59.94/59.95 class stays a WARNING.
constexpr double kNearIntegerWarnIntervalS = 60.0;

enum class Kind {
    Unknown,      ///< fps or period not known: no advice
    Exact,        ///< hold N, every frame the same number of vsyncs
    NearInteger,  ///< hold N, plus one single-vsync correction every intervalS
    NonInteger,   ///< a regular pulldown pattern (Bresenham), visible judder by construction
    Drop          ///< more content frames than vsyncs: 1 frame in dropEvery is never shown
};

struct Advice {
    Kind kind = Kind::Unknown;
    double r = 0.0;            ///< (1/fps) / period
    int n = 0;                 ///< the hold (Exact, NearInteger)
    double hz = 0.0;           ///< the pacing refresh, 1e9 / periodNs
    double intervalS = 0.0;    ///< NearInteger: seconds between single-vsync corrections
    double dropEvery = 0.0;    ///< Drop: 1 frame in this many is dropped
    double exactHz = 0.0;      ///< NonInteger/Drop/NearInteger: the refresh that would make it exact
    bool warning = false;      ///< journal level: WARNING (true) or INFO
    std::string text;          ///< the advice, after the "cadence: cue=… file=…" prefix
};

/// The advice for content at `fps` paced on a vsync of `periodNs`; `warnBelowS` is the near-integer threshold.
Advice advise(double fps, int64_t periodNs, double warnBelowS = kNearIntegerWarnIntervalS);

/// Shortest decimal form up to 3 places ("30", "29.97", "23.976").
std::string formatFps(double fps);

}  // namespace cadence
}  // namespace videocomposer
