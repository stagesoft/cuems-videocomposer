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

#ifndef VIDEOCOMPOSER_ENGINEOFFSETSPLIT_H
#define VIDEOCOMPOSER_ENGINEOFFSETSPLIT_H

#include <cmath>
#include <cstdint>

namespace videocomposer {

/**
 * The engine's layer offset in content frames, exactly (ClickUp 1amfwm7h924).
 *
 * The engine sends a cue's start in MTC frames (always 25 fps, 40 ms). Content at another rate has frames that do
 * not divide 40 ms (30p: 33.3 ms), so the offset is a whole number of content frames plus a remainder. Flooring the
 * offset on its own and adding it to the already-floored clock frame made non-25p video start up to one content
 * frame late against audio (0 / 6.7 / 13.3 / 20 / 26.7 ms at 30p, by the GO's position modulo 200 ms).
 *
 * `frames` is what LayerPlayback adds to the clock frame; `phaseMs` (in [0, one content frame)) is what the converter
 * adds to the clock before it turns it into content frames. Then
 *     floor((clockMs + phaseMs) * fps / 1000) + frames == floor((clockMs + offset * 1000 / mtcFps) * fps / 1000),
 * the frame the cue's start puts there. Equal rates (25p on 25 fps MTC) and unknown rates keep the offset as it is,
 * with no phase.
 */
struct EngineOffsetSplit {
    int64_t frames = 0;
    double phaseMs = 0.0;
};

inline EngineOffsetSplit splitEngineOffset(int64_t offsetMtcFrames, double mtcFps, double contentFps) {
    EngineOffsetSplit s;
    s.frames = offsetMtcFrames;
    if (offsetMtcFrames == 0 || mtcFps <= 0.0 || contentFps <= 0.0 || std::fabs(mtcFps - contentFps) <= 0.01) {
        return s;
    }
    const double x = static_cast<double>(offsetMtcFrames) * contentFps / mtcFps;
    // 1e-9: an offset that is a whole number of content frames (e.g. 5 MTC frames = 6 frames at 30p) must not
    // become 5.999999 frames + almost a whole frame of phase through the division's rounding.
    const double whole = std::floor(x + 1e-9);
    double frac = x - whole;
    if (frac < 0.0) {
        frac = 0.0;
    }
    s.frames = static_cast<int64_t>(whole);
    s.phaseMs = frac * 1000.0 / contentFps;
    return s;
}

} // namespace videocomposer

#endif // VIDEOCOMPOSER_ENGINEOFFSETSPLIT_H
