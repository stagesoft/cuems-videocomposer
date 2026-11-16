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
#include "../display/DisplayConfigurationManager.h"
#include "../display/OutputInfo.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>
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

// --- the "refresh=<pin> not available" WARNING must agree with the selection (1amfwm7evfj merged with 1amfwm7gq3y)

bool test_ModeSelect_OfferedRatesAreProgressiveOnly() {
    // A bare refresh= never takes an interlaced mode, so its frame rate (1080i60 = 30) is not a rate on offer:
    // listing it would send the operator back to the very pin that was refused.
    const auto rates = offeredRefreshRates(vg248(), 1920, 1080);
    return rates == std::vector<double>{24.0, 50.0, 60.0};
}

bool test_ModeSelect_OfferedRatesSortedOnePerRate() {
    const std::vector<ModeView> modes = {
        {1920, 1080, 60.000, false, true},
        {1920, 1080, 59.940, false, false},
        {1920, 1080, 60.0002, false, false},   // the same rate from a second timing
        {1920, 1080, 50.000, false, false},
        {1280, 720, 75.000, false, false},     // another size
    };
    const auto rates = offeredRefreshRates(modes, 1920, 1080);
    return rates.size() == 3 && rates[0] == 50.0 && rates[1] == 59.94 && rates[2] == 60.0;
}

bool test_ModeSelect_PinNotHonouredWhenProgressiveRateDiffers() {
    const double tol = 0.005;  // kRefreshHonouredTolHz
    const ModeView p60{1920, 1080, 60.0, false, true};
    const ModeView p5995{1920, 1200, 59.95, false, true};
    const ModeView p5994{1920, 1080, 59.9402, false, false};
    return refreshPinNotHonoured(30.0, false, p60, tol)        // refresh=30 answered with the native 60
        && refreshPinNotHonoured(60.0, false, p5995, tol)      // the 1920x1200 "60"
        && !refreshPinNotHonoured(59.94, false, p5994, tol)    // an integer-kHz clock
        && !refreshPinNotHonoured(60.0, false, p60, tol)
        && !refreshPinNotHonoured(0.0, false, p60, tol);       // no pin
}

bool test_ModeSelect_PinNotHonouredFollowsTheInterlacedRules() {
    const double tol = 0.005;
    const ModeView i60{1920, 1080, 30.0, true, false};   // 1080i60: frame rate 30, field rate 60
    const ModeView p60{1920, 1080, 60.0, false, true};
    // An interlaced choice has its own line (asked for: INFO; the only kind at that size: WARNING), and
    // refresh=60i is matched against the field rate, not the frame rate.
    return !refreshPinNotHonoured(60.0, true, i60, tol)
        && !refreshPinNotHonoured(25.0, false, i60, tol)
        // refresh=60i on a sink without it ends on a progressive mode: not what was asked, even at 60.000 Hz.
        && refreshPinNotHonoured(60.0, true, p60, tol);
}

// --- rows from the change review of 2026-11-16 (cuems-RELATIONS baselines/1amfwm7evfj/reviews)

bool test_ModeSelect_OutputInfoFindModeRefusesInterlacedWhenProgressiveExists() {
    // The OSC display/mode pre-check: 1080 has a progressive mode, so refresh=30 must not be answered with 1080i60.
    OutputInfo info;
    OutputMode p60; p60.width = 1920; p60.height = 1080; p60.refreshRate = 60.0; p60.interlaced = false;
    OutputMode i60; i60.width = 1920; i60.height = 1080; i60.refreshRate = 30.0; i60.interlaced = true;
    info.modes = {p60, i60};
    const bool refused = info.findMode(1920, 1080, 30.0) == nullptr;
    const OutputMode* any = info.findMode(1920, 1080);          // no refresh asked: the progressive one
    return refused && any && !any->interlaced;
}

bool test_ModeSelect_EqualDistanceKeepsTheFirstMode() {
    // Two timings at the same rate (an EDID DTD and a CEA VIC): the first listed wins, as before this change
    // (the kernel lists the preferred one first).
    const std::vector<ModeView> prog = {
        {1920, 1080, 60.00, false, true},
        {1920, 1080, 60.00, false, false},
        {1920, 1080, 50.00, false, false},
    };
    const std::vector<ModeView> inter = {
        {1920, 1080, 30.00, true, false},
        {1920, 1080, 30.00, true, false},
    };
    const auto p = pickModeForRefresh(prog, 1920, 1080, 60.0, false);
    const auto i = pickModeForRefresh(inter, 1920, 1080, 60.0, true);
    return p.kind == ModePick::Kind::Exact && p.index == 0 && i.kind == ModePick::Kind::Exact && i.index == 0;
}

bool test_ModeSelect_OnlyInterlacedSizeUsesFieldRateWhenAskedInterlaced() {
    // A size with interlaced modes only, refresh=50i, no 50i: the nearest FIELD rate is 48 (1080i48), not the
    // mode whose frame rate (30) is nearest to the number 50.
    const std::vector<ModeView> modes = {
        {1920, 1080, 30.00, true, false},   // 1080i60
        {1920, 1080, 24.00, true, false},   // 1080i48
    };
    const auto p = pickModeForRefresh(modes, 1920, 1080, 50.0, true);
    return p.kind == ModePick::Kind::OnlyInterlaced && p.index == 1;
}

bool test_ModeSelect_ExplicitInterlacedWithoutSuchModeFallsToNativeProgressive() {
    const auto modes = lg4k();   // no interlaced mode at all
    const auto p = pickModeForRefresh(modes, 3840, 2160, 60.0, true);
    return p.kind == ModePick::Kind::NativeProgressive && p.index == 0;
}

bool test_ModeSelect_DisplayConfRefreshInterlacedSuffix() {
    // refresh=<n>i in display.conf: the "i" is found whatever follows it on the line (a CRLF file, a trailing
    // space), in either case, and survives save + load.
    char tmpl[] = "/tmp/vc-modeselect-XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd < 0) {
        return false;
    }
    ::close(fd);
    const std::string in = tmpl;
    const std::string out = in + ".out";
    {
        std::ofstream f(in);
        f << "[output:HDMI-A-1]\nresolution=1920x1080\nrefresh=60i\ncanvas_region=0,0,1920,1080\n"
          << "[output:HDMI-A-2]\nresolution=1920x1080\nrefresh=60I\r\ncanvas_region=1920,0,1920,1080\n"
          << "[output:HDMI-A-3]\nresolution=1920x1080\nrefresh=50i \ncanvas_region=3840,0,1920,1080\n"
          << "[output:HDMI-A-4]\nresolution=1920x1080\nrefresh=50\ncanvas_region=5760,0,1920,1080\n";
    }
    bool ok = false;
    {
        DisplayConfigurationManager m;
        ok = m.loadFromFile(in);
        const OutputConfiguration* a1 = m.getOutputConfig("HDMI-A-1");
        const OutputConfiguration* a2 = m.getOutputConfig("HDMI-A-2");
        const OutputConfiguration* a3 = m.getOutputConfig("HDMI-A-3");
        const OutputConfiguration* a4 = m.getOutputConfig("HDMI-A-4");
        ok = ok && a1 && a2 && a3 && a4
            && a1->refreshRate == 60.0 && a1->refreshInterlaced
            && a2->refreshRate == 60.0 && a2->refreshInterlaced
            && a3->refreshRate == 50.0 && a3->refreshInterlaced
            && a4->refreshRate == 50.0 && !a4->refreshInterlaced;
        ok = ok && m.saveToFile(out);
    }
    if (ok) {
        DisplayConfigurationManager again;
        ok = again.loadFromFile(out);
        const OutputConfiguration* a1 = again.getOutputConfig("HDMI-A-1");
        const OutputConfiguration* a4 = again.getOutputConfig("HDMI-A-4");
        ok = ok && a1 && a4 && a1->refreshRate == 60.0 && a1->refreshInterlaced
            && a4->refreshRate == 50.0 && !a4->refreshInterlaced;
    }
    ::unlink(in.c_str());
    ::unlink(out.c_str());
    return ok;
}
