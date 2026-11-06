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

#include "PresentTick.h"

#include <cstdlib>
#include <cstring>

namespace videocomposer {

namespace {
PresentTick g_tick;
VblankNow g_now;

bool envTrue(const char* name) {
    const char* v = std::getenv(name);
    return v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0);
}
} // namespace

PresentTick currentPresentTick() { return g_tick; }

void publishPresentTick(const PresentTick& tick) {
    g_tick = tick;
    g_tick.seq = static_cast<uint32_t>(g_tick.seq + 1);
}

VblankNow currentVblank() { return g_now; }

void publishVblankNow(const VblankNow& now) { g_now = now; }

void resetPresentTick() {
    g_tick = PresentTick{};
    g_now = VblankNow{};
}

bool cadenceTraceEnabled() {
    static const bool enabled = envTrue("VIDEOCOMPOSER_CADENCE_TRACE");
    return enabled;
}

bool cadenceLegacyForced() {
    static const bool forced = envTrue("VIDEOCOMPOSER_CADENCE_LEGACY");
    return forced;
}

} // namespace videocomposer
