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

#include "ContractTypes.h"

#include <cstdio>
#include <map>

namespace videocomposer {

namespace {
constexpr uint32_t kFlagInterlace = 1u << 4;  // DRM_MODE_FLAG_INTERLACE
constexpr uint32_t kTypePreferred = 1u << 3;  // DRM_MODE_TYPE_PREFERRED
}  // namespace

ContractMode ContractMode::fromTiming(const ContractModeTiming& t) {
    ContractMode m;
    m.timing = t;
    m.width = static_cast<int>(t.hdisplay);
    m.height = static_cast<int>(t.vdisplay);
    if (t.htotal && t.vtotal) {
        m.refreshHz = static_cast<double>(t.clockKhz) * 1000.0
                      / (static_cast<double>(t.htotal) * static_cast<double>(t.vtotal));
    }
    m.interlaced = (t.flags & kFlagInterlace) != 0;
    m.fieldRateHz = m.interlaced ? m.refreshHz * 2.0 : m.refreshHz;
    m.preferred = (t.type & kTypePreferred) != 0;
    return m;
}

std::string baseModeId(const ContractMode& m) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%dx%d@%.2f%c", m.width, m.height, m.refreshHz,
                  m.interlaced ? 'i' : 'p');
    return buf;
}

std::string contractModeId(const ContractMode& m) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%dx%d@%.2f%c", m.width, m.height, m.fieldRateHz,
                  m.interlaced ? 'i' : 'p');
    return buf;
}

void assignModeIds(std::vector<ContractMode>& modes) {
    std::map<std::string, int> baseCount;
    for (const auto& m : modes) {
        baseCount[baseModeId(m)]++;
    }
    std::map<std::string, int> withClockCount;
    for (auto& m : modes) {
        std::string id = baseModeId(m);
        if (baseCount[id] > 1) {
            id += "/" + std::to_string(m.timing.clockKhz);
        }
        m.modeId = id;
        withClockCount[id]++;
    }
    std::map<std::string, int> seen;
    for (auto& m : modes) {
        if (withClockCount[m.modeId] > 1) {
            const int n = ++seen[m.modeId];
            m.modeId += "#" + std::to_string(n);
        }
    }
}

} // namespace videocomposer
