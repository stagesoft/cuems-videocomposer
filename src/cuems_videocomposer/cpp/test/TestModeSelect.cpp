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
#include "../display/ModeSelect.h"
#include "../display/OutputInfo.h"

#include <vector>

using namespace videocomposer;
using namespace videocomposer::test;

namespace {

// Shape of the Asus VG248 on test2 node01 (modetest, 2026-10-21): 1080 progressive
// at 60/50/24, interlaced only at 60/50 field rate (30/25 frame rate).
std::vector<ModeView> vg248() {
    return {
        {1920, 1080, 60.00, false, true},
        {1920, 1080, 50.00, false, false},
        {1920, 1080, 30.00, true, false},   // 1080i60
        {1920, 1080, 25.00, true, false},   // 1080i50
        {1920, 1080, 24.00, false, false},
        {1280, 720, 60.00, false, false},
    };
}

// LG UltraFine-like 4K list.
std::vector<ModeView> lg4k() {
    return {
        {3840, 2160, 60.00, false, true},
        {3840, 2160, 50.00, false, false},
        {3840, 2160, 30.00, false, false},
        {3840, 2160, 29.97, false, false},
        {3840, 2160, 24.00, false, false},
    };
}

}  // namespace

bool test_ModeSelect_Refresh30On1080PicksNativeProgressiveNotInterlaced() {
    // The .111 case: refresh=30 used to land on 1080i60.
    const auto modes = vg248();
    const auto p = pickModeForRefresh(modes, 1920, 1080, 30.0, false);
    return p.kind == ModePick::Kind::NativeProgressive && !modes[p.index].interlaced &&
           modes[p.index].frameHz == 60.0;
}

bool test_ModeSelect_ExplicitInterlacedMatchesFieldRate() {
    const auto modes = vg248();
    const auto p60i = pickModeForRefresh(modes, 1920, 1080, 60.0, true);
    const auto p50i = pickModeForRefresh(modes, 1920, 1080, 50.0, true);
    return p60i.kind == ModePick::Kind::Exact && modes[p60i.index].interlaced && modes[p60i.index].frameHz == 30.0 &&
           p50i.kind == ModePick::Kind::Exact && modes[p50i.index].interlaced && modes[p50i.index].frameHz == 25.0;
}

bool test_ModeSelect_ProgressiveWithinToleranceWinsClosest() {
    const auto modes = lg4k();
    const auto a = pickModeForRefresh(modes, 3840, 2160, 29.97, false);
    const auto b = pickModeForRefresh(modes, 3840, 2160, 30.0, false);
    const auto c = pickModeForRefresh(modes, 1920, 1080, 50.0, false);  // size absent
    return a.kind == ModePick::Kind::Exact && modes[a.index].frameHz == 29.97 &&
           b.kind == ModePick::Kind::Exact && modes[b.index].frameHz == 30.0 &&
           c.index == -1;
}

bool test_ModeSelect_NoProgressiveSizeFallsBackToInterlacedWithKind() {
    std::vector<ModeView> modes = {{1920, 1080, 30.0, true, false}, {1920, 1080, 25.0, true, false},
                                   {1280, 720, 60.0, false, true}};
    const auto p = pickModeForRefresh(modes, 1920, 1080, 25.0, false);
    return p.kind == ModePick::Kind::OnlyInterlaced && modes[p.index].frameHz == 25.0;
}

bool test_ModeSelect_NoRefreshPrefersPreferredProgressive() {
    auto modes = vg248();
    modes[2].preferred = true;  // even a "preferred" interlaced mode loses
    modes[0].preferred = false;
    const auto p = pickModeForRefresh(modes, 1920, 1080, 0.0, false);
    return p.kind == ModePick::Kind::Exact && !modes[p.index].interlaced && modes[p.index].frameHz == 60.0;
}

bool test_ModeSelect_OutputInfoFindModePrefersProgressive() {
    OutputInfo info;
    OutputMode i60; i60.width = 1920; i60.height = 1080; i60.refreshRate = 30.0; i60.interlaced = true;
    OutputMode p30; p30.width = 1920; p30.height = 1080; p30.refreshRate = 30.0; p30.interlaced = false;
    info.modes = {i60, p30};
    const OutputMode* m = info.findMode(1920, 1080, 30.0);
    info.modes = {i60};
    const OutputMode* onlyI = info.findMode(1920, 1080, 30.0);
    return m && !m->interlaced && onlyI && onlyI->interlaced;
}
