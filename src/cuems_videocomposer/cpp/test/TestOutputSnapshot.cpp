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
#include "../display/OutputSnapshot.h"

#include <string>
#include <vector>

using namespace videocomposer;
using namespace videocomposer::test;

bool test_OutputSnapshot_EncodePpmFlipsRowsAndDropsAlpha() {
    // 2x2 RGBA, GL order: bottom row first.
    const std::vector<uint8_t> rgba = {
        1, 2, 3, 255,    4, 5, 6, 255,     // bottom row (y = 0)
        7, 8, 9, 255,   10, 11, 12, 255,   // top row (y = 1)
    };
    const auto ppm = OutputSnapshot::encodePpm(rgba, 2, 2);
    const std::string header = "P6\n2 2\n255\n";
    if (ppm.size() != header.size() + 12) return false;
    if (std::string(ppm.begin(), ppm.begin() + header.size()) != header) return false;
    const std::vector<uint8_t> pixels(ppm.begin() + header.size(), ppm.end());
    // PPM is top-down: the GL top row comes first.
    return pixels == std::vector<uint8_t>{7, 8, 9, 10, 11, 12, 1, 2, 3, 4, 5, 6};
}

bool test_OutputSnapshot_RoundCoversEveryOutputOnce() {
    OutputSnapshot::requestForTest();
    if (!OutputSnapshot::wanted("A") || !OutputSnapshot::wanted("B")) return false;
    // capture() with an empty size marks the output done without GL.
    OutputSnapshot::capture("A", 0, 0);
    OutputSnapshot::endPass(2);
    if (OutputSnapshot::wanted("A")) return false;      // A already done this round
    if (!OutputSnapshot::wanted("B")) return false;     // round stays open for B
    OutputSnapshot::capture("B", 0, 0);
    OutputSnapshot::endPass(2);
    return !OutputSnapshot::wanted("A") && !OutputSnapshot::wanted("B");
}
