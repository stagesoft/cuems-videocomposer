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

#ifndef VIDEOCOMPOSER_CONTRACTPROTOCOL_H
#define VIDEOCOMPOSER_CONTRACTPROTOCOL_H

#include "ContractTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace videocomposer {

/**
 * The JSON side of the hardware-configuration contract, schema v1
 * (docs/contract-v1.md). Pure: no sockets, no globals, so the unit tests
 * drive it directly and ContractServer only moves bytes.
 */
class ContractProtocol {
public:
    static constexpr int kVersion = 1;

    struct Identity {
        std::string vcVersion;
        std::string hostname;
        int64_t pid = 0;
        std::vector<std::string> caps;
        std::string showLockPath = "/tmp/cuems.show.lock";
    };

    /// One request in, one reply out. Never throws.
    static std::string handle(const std::string& request, const ContractSnapshot& snapshot,
                              const Identity& identity);

    /// Event documents published on the Pub socket (topic prefix included).
    static std::string eventStateChanged(uint64_t seq, int64_t pid);
    static std::string eventModeWarning(uint64_t seq, int64_t pid, const std::string& kind,
                                        const std::string& detail);

    static std::string hardwareJson(const ContractSnapshot& snapshot);  // the "result" of get_hardware
    static std::string stateJson(const ContractSnapshot& snapshot, bool showLoaded);
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_CONTRACTPROTOCOL_H
