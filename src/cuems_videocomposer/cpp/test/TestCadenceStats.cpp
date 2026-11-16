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
#include "../display/CadenceStats.h"

#include <cmath>
#include <vector>

using namespace videocomposer;
using namespace videocomposer::test;

namespace {

// Ideal presenter: on every vsync shows the frame due at that vsync,
// frame = floor(vsync * fps / hz). Drives CadenceStats one flip per vsync.
CadenceStats::Counters runIdeal(double hz, double fps, int vsyncs, int64_t startFrame = 0) {
    CadenceStats st;
    st.setLogIntervalNs(INT64_MAX);
    for (int v = 0; v < vsyncs; ++v) {
        const auto frame = startFrame + static_cast<int64_t>(std::floor(v * fps / hz + 1e-9));
        st.onPresented("OUT", 1000 + v, hz, {{7, frame, fps}}, v);
    }
    const auto* c = st.counters("OUT", 7);
    return c ? *c : CadenceStats::Counters{};
}

}  // namespace

bool test_CadenceStats_AllowedHolds() {
    auto a = CadenceStats::allowedHolds(60.0, 25.0);
    if (a.first != 2 || a.second != 3) return false;
    a = CadenceStats::allowedHolds(60.0, 30.0);
    if (a.first != 2 || a.second != 2) return false;
    a = CadenceStats::allowedHolds(30.0, 29.97);
    if (a.first != 1 || a.second != 2) return false;
    a = CadenceStats::allowedHolds(50.0, 25.0);
    if (a.first != 2 || a.second != 2) return false;
    a = CadenceStats::allowedHolds(0.0, 25.0);
    return a.first == 0;
}

bool test_CadenceStats_Pulldown25at60_Clean() {
    auto c = runIdeal(60.0, 25.0, 6000);
    // 3:2 pulldown: only 2s and 3s, 3s = 40 % of changes.
    if (c.outsideA != 0 || c.skippedFrames != 0 || c.stepsBack != 0) return false;
    if (c.holds.size() != 2 || !c.holds.count(2) || !c.holds.count(3)) return false;
    return std::abs(static_cast<double>(c.minority) - c.minorityExpected) <= 2.0;
}

bool test_CadenceStats_2997at30_InherentDoublesAreMinority() {
    auto c = runIdeal(30.0, 29.97, 30000);
    if (c.outsideA != 0) return false;
    // ~30 two-vsync holds over 30000 vsyncs, all predicted by the rate.
    return c.holds.count(2) && c.minority == c.holds.at(2)
           && std::abs(static_cast<double>(c.minority) - c.minorityExpected) <= 2.0;
}

bool test_CadenceStats_IrregularPairCountedOutsideA() {
    // 30 fps on 60 Hz should be all 2s; one frame shown for 3 vsyncs and the
    // next for 1 ("3 then 1") must count twice outside A.
    CadenceStats st;
    st.setLogIntervalNs(INT64_MAX);
    std::vector<int64_t> frames = {0,0, 1,1, 2,2,2, 3, 4,4, 5,5};
    for (size_t v = 0; v < frames.size(); ++v) {
        st.onPresented("OUT", static_cast<int64_t>(v), 60.0, {{1, frames[v], 30.0}}, 0);
    }
    const auto* c = st.counters("OUT", 1);
    return c && c->outsideA == 2 && c->holds.at(3) == 1 && c->holds.at(1) == 1;
}

bool test_CadenceStats_MissedVsyncFromMscGap() {
    // The flip event of frame 2 arrives one vsync late (msc jumps by 2): the
    // hold of frame 1 is 3 vsyncs even though only two flips were stamped.
    CadenceStats st;
    st.setLogIntervalNs(INT64_MAX);
    st.onPresented("OUT", 10, 60.0, {{1, 0, 30.0}}, 0);
    st.onPresented("OUT", 12, 60.0, {{1, 1, 30.0}}, 0);
    st.onPresented("OUT", 15, 60.0, {{1, 2, 30.0}}, 0);
    const auto* c = st.counters("OUT", 1);
    return c && c->holds.at(2) == 1 && c->holds.at(3) == 1 && c->outsideA == 1;
}

bool test_CadenceStats_WrapSkipBackDiscontinuity() {
    CadenceStats st;
    st.setLogIntervalNs(INT64_MAX);
    int64_t msc = 0;
    auto show = [&](int64_t f) { st.onPresented("OUT", msc, 50.0, {{2, f, 25.0}}, 0); msc += 2; };
    show(10); show(11); show(13);   // skip of one frame
    show(12);                       // step back
    show(0);                        // wrap to the start
    show(1); show(40);              // seek: discontinuity, hold not counted
    show(41);
    const auto* c = st.counters("OUT", 2);
    return c && c->skippedFrames == 1 && c->stepsBack == 1 && c->wraps == 1
           && c->discontinuities == 1 && c->outsideA == 0 && c->changes == 6;
}

bool test_CadenceStats_PerOutputAndLayerRemoval() {
    CadenceStats st;
    st.setLogIntervalNs(INT64_MAX);
    st.onPresented("A", 1, 60.0, {{1, 0, 30.0}, {2, 0, 25.0}}, 0);
    st.onPresented("B", 1, 50.0, {{1, 0, 30.0}}, 0);
    if (!st.counters("A", 2) || !st.counters("B", 1) || st.counters("B", 2)) return false;
    // Layer 2 leaves output A's composite: summarised and dropped; B untouched.
    st.onPresented("A", 2, 60.0, {{1, 0, 30.0}}, 0);
    if (st.counters("A", 2) || !st.counters("A", 1) || !st.counters("B", 1)) return false;
    st.logFinal();
    return st.counters("A", 1) == nullptr;
}
