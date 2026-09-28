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
 * TestLayerPathId.cpp - a cue UUID must never be taken for an integer layer id
 * (869f8j1ja).
 */

#include "remote/LayerPathId.h"
#include "TestFramework.h"

using namespace videocomposer;

bool test_LayerPathId_IntegerIds() {
    TEST_ASSERT_TRUE(isIntegerLayerId("1"));
    TEST_ASSERT_TRUE(isIntegerLayerId("29"));
    TEST_ASSERT_TRUE(isIntegerLayerId("0"));
    TEST_ASSERT_TRUE(isIntegerLayerId("123456789"));
    return true;
}

bool test_LayerPathId_UuidsAreNotIntegerIds() {
    // Real cue layer ids from medina cupula2: atoi() gives 2, 29, 615, 0.
    TEST_ASSERT_FALSE(isIntegerLayerId("2ac1fe93-b5c6-4b27-a937-fdf5c1af4f08_0"));
    TEST_ASSERT_FALSE(isIntegerLayerId("29a31353-5124-4bb8-aee0-1c4b2c29207b_0"));
    TEST_ASSERT_FALSE(isIntegerLayerId("615f2f3c-ca34-4b87-a7b8-033d1159c6e3_0"));
    TEST_ASSERT_FALSE(isIntegerLayerId("c2ac5b3a-14d3-4638-ab23-63bb18d8898f_0"));
    return true;
}

bool test_LayerPathId_Malformed() {
    TEST_ASSERT_FALSE(isIntegerLayerId(""));
    TEST_ASSERT_FALSE(isIntegerLayerId("-1"));
    TEST_ASSERT_FALSE(isIntegerLayerId("+1"));
    TEST_ASSERT_FALSE(isIntegerLayerId(" 1"));
    TEST_ASSERT_FALSE(isIntegerLayerId("1 "));
    TEST_ASSERT_FALSE(isIntegerLayerId("1234567890"));  // would overflow atoi's int
    return true;
}
