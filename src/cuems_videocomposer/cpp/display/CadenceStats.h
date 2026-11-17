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

#ifndef VIDEOCOMPOSER_CADENCESTATS_H
#define VIDEOCOMPOSER_CADENCESTATS_H

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace videocomposer {

/**
 * CadenceStats - per (output, layer) presented-frame cadence, counted in the
 * output's own vsyncs (msc of the flip event that put each frame on screen).
 *
 * Enabled by VIDEOCOMPOSER_CADENCE_STATS=1; off, the backend never builds a
 * sample and this class costs nothing. Pure and thread-free: the backend
 * feeds it from the render thread, the unit tests feed it synthetic flips.
 *
 * A "hold" is how many vsyncs a frame stayed on screen. For content at fps
 * on a display at hz, r = hz / fps and the allowed set is
 * A = {floor(r), ceil(r)} (just {r} when r is an integer): 25@60 gives
 * holds of 2 and 3 (3:2 pulldown), 29.97@30 gives 1 and the occasional 2
 * (the inherent drift), 30@60 gives only 2. A hold outside A is a visible
 * irregularity ("3 then 1"). The minority-hold count is reported next to the
 * count the rate alone predicts, so the inherent drift reads as inherent.
 *
 * Step 1 of plan R9 (cuems-RELATIONS Plans/2026-10-20-vc-per-output-cadence-r9.md
 * §2.8, S1): counters only, no behaviour change.
 */
class CadenceStats {
public:
    struct Sample {
        int layerId = 0;
        int64_t frame = 0;        // frame the layer showed in this composite
        double contentFps = 0.0;  // layer's own frame rate (0 = unknown)
    };

    struct Counters {
        double contentFps = 0.0;
        double refreshHz = 0.0;
        int64_t presents = 0;       // flips stamped for this (output, layer)
        int64_t changes = 0;        // frame changes measured as a hold
        std::map<int64_t, int64_t> holds;  // hold length in vsyncs -> count
        int64_t outsideA = 0;       // holds not in {floor(r), ceil(r)}
        int64_t minority = 0;       // holds equal to the less common member of A
        double minorityExpected = 0.0;  // what r alone predicts for 'changes'
        int64_t skippedFrames = 0;  // frames jumped over by forward steps of 2
        int64_t stepsBack = 0;      // frame went backwards (not a wrap)
        int64_t wraps = 0;          // frame went back to the start (loop)
        int64_t discontinuities = 0;  // jumps of 3+ frames (seek/resync): hold not counted
    };

    static bool enabledFromEnv();

    /// The allowed hold set for a ratio r = hz / fps: {lo, hi}, lo == hi when r is an integer.
    static std::pair<int64_t, int64_t> allowedHolds(double refreshHz, double contentFps);

    /// One flip completed on 'output' at vsync counter 'msc'. 'samples' are the
    /// visible layers of the composite that flip presented. nowNs (monotonic)
    /// drives the periodic log.
    void onPresented(const std::string& output, int64_t msc, double refreshHz,
                     const std::vector<Sample>& samples, int64_t nowNs);

    /// Summary of every (output, layer) still tracked; call at stop.
    void logFinal();

    const Counters* counters(const std::string& output, int layerId) const;

    void setLogIntervalNs(int64_t ns) { logIntervalNs_ = ns; }

private:
    struct Track {
        Counters c;
        bool have = false;
        int64_t lastFrame = 0;
        int64_t lastChangeMsc = 0;
        int64_t lastLogNs = 0;
        bool seen = false;          // present in the latest flip of its output
    };

    using Key = std::pair<std::string, int>;
    std::map<Key, Track> tracks_;
    int64_t logIntervalNs_ = 60LL * 1000000000LL;

    static std::string format(const std::string& output, int layerId, const Counters& c);
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_CADENCESTATS_H
