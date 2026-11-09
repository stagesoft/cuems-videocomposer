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
 * @file TestCadenceAdvice.cpp
 * @brief The refresh rule said at layer load (plan 1amfwm7gq3y §4.2), on the fleet's real modes.
 *
 * Every period is the one a DRM mode really has: clock (integer kHz) × 1000 / (htotal × vtotal), rounded to ns the
 * way the backend publishes it. The cases are the ones the research and the rigs met: CEA 1080p60 (148.5 MHz),
 * CEA "59.94" (148.352 MHz), 1920x1200 CVT-RB "60" (154 MHz, 59.950 Hz), the Acer V193HQV's native 1366x768
 * (85.5 MHz, 59.790 Hz), 1080p50.
 */

#include "TestFramework.h"
#include "../sync/CadenceAdvice.h"

#include <cmath>
#include <string>

using namespace videocomposer;
using namespace videocomposer::test;
namespace cd = videocomposer::cadence;

namespace {

int64_t periodOf(int clockKHz, int htotal, int vtotal) {
    return static_cast<int64_t>(std::llround(1e9 * htotal * static_cast<double>(vtotal) / (clockKHz * 1000.0)));
}

const int64_t kP1080p60 = periodOf(148500, 2200, 1125);    // 60.000 Hz
const int64_t kP1080p5994 = periodOf(148352, 2200, 1125);  // 59.940 Hz (the integer-kHz "59.94")
const int64_t kP1080p50 = periodOf(148500, 2640, 1125);    // 50.000 Hz
const int64_t kP1200pRB = periodOf(154000, 2080, 1235);    // 59.950 Hz (CVT-RB, the projectors' "60")
const int64_t kPAcer = periodOf(85500, 1792, 798);         // 59.790 Hz (1366x768, the eye test's control)
const double k2997 = 30000.0 / 1001.0;

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

}  // namespace

REGISTER_TEST(CadenceAdvice_ExactHolds) {
    auto a = cd::advise(30.0, kP1080p60);
    TEST_ASSERT(a.kind == cd::Kind::Exact);
    TEST_ASSERT_EQ(a.n, 2);
    TEST_ASSERT_FALSE(a.warning);
    TEST_ASSERT(contains(a.text, "hold 2 (exact)"));
    a = cd::advise(25.0, kP1080p50);
    TEST_ASSERT(a.kind == cd::Kind::Exact);
    TEST_ASSERT_EQ(a.n, 2);
    a = cd::advise(60.0, kP1080p60);
    TEST_ASSERT(a.kind == cd::Kind::Exact);
    TEST_ASSERT_EQ(a.n, 1);
    // 29.97p on the 148.352 MHz mode: off by ~2 ppm (the integer-kHz clock), below the clocks' own drift: exact
    a = cd::advise(k2997, kP1080p5994);
    TEST_ASSERT(a.kind == cd::Kind::Exact);
    TEST_ASSERT(contains(a.text, "29.97 fps on 59.940 Hz"));
    return true;
}

REGISTER_TEST(CadenceAdvice_NearIntegerInterval) {
    // The Acer control: 30p on 59.790 Hz, one correction every ~4.8 s (eye-test row #0)
    auto a = cd::advise(30.0, kPAcer);
    TEST_ASSERT(a.kind == cd::Kind::NearInteger);
    TEST_ASSERT_EQ(a.n, 2);
    TEST_ASSERT(std::fabs(a.intervalS - 4.75) < 0.1);
    TEST_ASSERT_TRUE(a.warning);
    TEST_ASSERT(contains(a.text, "every ~4.8 s"));
    TEST_ASSERT(contains(a.text, "use a 60.000 Hz mode"));
    TEST_ASSERT_FALSE(contains(a.text, "or content at"));   // no standard rate helps on 59.790 Hz
    // 1920x1200 "60" = 59.950 Hz: 30p corrects every ~20 s (research §D.10)
    a = cd::advise(30.0, kP1200pRB);
    TEST_ASSERT(a.kind == cd::Kind::NearInteger);
    TEST_ASSERT(std::fabs(a.intervalS - 20.0) < 0.5);
    TEST_ASSERT_TRUE(a.warning);
    TEST_ASSERT(contains(a.text, "or content at 29.97 fps (a correction every ~99 s)"));   // a standard rate
    // 29.97p on a true 60.000: every ~17 s
    a = cd::advise(k2997, kP1080p60);
    TEST_ASSERT(a.kind == cd::Kind::NearInteger);
    TEST_ASSERT(std::fabs(a.intervalS - 16.7) < 0.3);
    // The level follows the threshold (D2: set from the eye test), the interval does not
    a = cd::advise(30.0, kP1200pRB, 10.0);
    TEST_ASSERT(a.kind == cd::Kind::NearInteger);
    TEST_ASSERT_FALSE(a.warning);
    TEST_ASSERT(std::fabs(a.intervalS - 20.0) < 0.5);
    return true;
}

REGISTER_TEST(CadenceAdvice_NonIntegerPulldown) {
    auto a = cd::advise(25.0, kP1080p60);    // the La Llotja case
    TEST_ASSERT(a.kind == cd::Kind::NonInteger);
    TEST_ASSERT_TRUE(a.warning);
    TEST_ASSERT(std::fabs(a.r - 2.4) < 1e-6);
    TEST_ASSERT(std::fabs(a.exactHz - 50.0) < 1e-9);
    TEST_ASSERT(contains(a.text, "-> 2,3 pulldown"));
    a = cd::advise(30.0, kP1080p50);         // 1,2 on 50 Hz: suggest 60, not 30
    TEST_ASSERT(a.kind == cd::Kind::NonInteger);
    TEST_ASSERT(std::fabs(a.exactHz - 60.0) < 1e-9);
    TEST_ASSERT(contains(a.text, "-> 1,2 pulldown"));
    a = cd::advise(24.0, kP1080p60);         // 2.5: halfway, suggest the lower multiple (48)
    TEST_ASSERT(a.kind == cd::Kind::NonInteger);
    TEST_ASSERT(std::fabs(a.exactHz - 48.0) < 1e-9);
    return true;
}

REGISTER_TEST(CadenceAdvice_DropRegime) {
    auto a = cd::advise(60.0, kP1080p50);
    TEST_ASSERT(a.kind == cd::Kind::Drop);
    TEST_ASSERT_TRUE(a.warning);
    TEST_ASSERT(std::fabs(a.dropEvery - 6.0) < 0.01);
    TEST_ASSERT(std::fabs(a.exactHz - 60.0) < 1e-9);
    return true;
}

REGISTER_TEST(CadenceAdvice_Unknown) {
    auto a = cd::advise(0.0, kP1080p60);
    TEST_ASSERT(a.kind == cd::Kind::Unknown);
    TEST_ASSERT_FALSE(a.warning);
    TEST_ASSERT(contains(a.text, "fps unknown"));
    a = cd::advise(25.0, 0);
    TEST_ASSERT(a.kind == cd::Kind::Unknown);
    TEST_ASSERT_FALSE(a.warning);
    TEST_ASSERT(contains(a.text, "no vsync period known yet"));
    return true;
}

REGISTER_TEST(CadenceAdvice_FormatFps) {
    TEST_ASSERT_EQ(cd::formatFps(30.0), std::string("30"));
    TEST_ASSERT_EQ(cd::formatFps(k2997), std::string("29.97"));
    TEST_ASSERT_EQ(cd::formatFps(24000.0 / 1001.0), std::string("23.976"));
    TEST_ASSERT_EQ(cd::formatFps(25.0), std::string("25"));
    return true;
}
