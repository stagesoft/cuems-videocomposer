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

/**
 * TestSMPTEUtils.cpp - Unit test for SMPTEUtils::toFrame >24h overflow handling
 *
 * Guards the Plan 3a hardening: a SMPTE string at or beyond 24h must convert to
 * the full frame count, not truncate mod-24h. FIX_SMPTE_OVERFLOW folds whole
 * days into v[SMPTE_OVERFLOW]; toFrame() must add that day term back.
 */

#include "utils/SMPTEUtils.h"
#include "TestFramework.h"
#include <cstdint>

using namespace videocomposer;

// 25 fps, non-dropframe: frame = totalSeconds*25 + frameField.
bool test_SMPTEUtils_Overflow24h() {
    // Sub-24h: unchanged baseline behaviour.
    TEST_ASSERT_EQ(SMPTEUtils::smpteStringToFrame("00:00:00:00", 25.0), (int64_t)0);
    TEST_ASSERT_EQ(SMPTEUtils::smpteStringToFrame("23:59:59:24", 25.0), (int64_t)86399 * 25 + 24); // 2159999

    // Exactly 24h: overflow=1, hour=0 → must be 24h of frames, not 0.
    TEST_ASSERT_EQ(SMPTEUtils::smpteStringToFrame("24:00:00:00", 25.0), (int64_t)86400 * 25);       // 2160000

    // >24h: the regression target. Pre-fix these truncated mod-24h.
    TEST_ASSERT_EQ(SMPTEUtils::smpteStringToFrame("25:00:00:00", 25.0), (int64_t)90000 * 25);        // 2250000
    TEST_ASSERT_EQ(SMPTEUtils::smpteStringToFrame("48:00:00:00", 25.0), (int64_t)172800 * 25);       // 4320000 (2-day multiplier)

    return true;
}
