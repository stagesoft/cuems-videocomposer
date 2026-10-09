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

#ifndef VIDEOCOMPOSER_NDITRANSPORT_H
#define VIDEOCOMPOSER_NDITRANSPORT_H

// Which NDI receive transport this process uses (plan rev 9, D17).
//
// The shipped ndi-config.v1.json forces NDI's base TCP connection, the only
// transport a plain TCP relay can carry. The NDI SDK reads it from
// NDI_CONFIG_DIR when the library initialises, per process, so main() calls
// configureNdiTransport() before anything can touch NDI (three call sites
// initialise the SDK). An operator who sets NDI_CONFIG_DIR keeps it; the
// startup line then says the relay is unavailable, and the bridge reads that
// line before relaying to this videocomposer.

#include <cstdlib>
#include <string>
#include <sys/stat.h>

namespace videocomposer {

struct NdiTransportState {
    bool baseTcp = false;
    std::string detail;  // the config dir, or why it is not base TCP
};

// Pure decision, testable: envValue is NDI_CONFIG_DIR as found (nullptr or
// empty = unset).
inline NdiTransportState decideNdiTransport(const char* envValue, const std::string& shippedDir,
                                            bool shippedConfigExists) {
    NdiTransportState s;
    if (envValue && *envValue) {
        if (shippedDir == envValue && shippedConfigExists) {
            s.baseTcp = true;
            s.detail = shippedDir;
        } else {
            s.detail = std::string("NDI_CONFIG_DIR=") + envValue;
        }
    } else if (shippedConfigExists) {
        s.baseTcp = true;
        s.detail = shippedDir;
    } else {
        s.detail = "no config at " + shippedDir + "/ndi-config.v1.json";
    }
    return s;
}

inline NdiTransportState& ndiTransportState() {
    static NdiTransportState state;
    return state;
}

// Call first thing in main().
inline void configureNdiTransport(const std::string& shippedDir) {
    struct stat st;
    const bool exists = ::stat((shippedDir + "/ndi-config.v1.json").c_str(), &st) == 0;
    const char* env = std::getenv("NDI_CONFIG_DIR");
    ndiTransportState() = decideNdiTransport(env, shippedDir, exists);
    if ((!env || !*env) && exists) {
        ::setenv("NDI_CONFIG_DIR", shippedDir.c_str(), 0);
    }
}

}  // namespace videocomposer

#endif  // VIDEOCOMPOSER_NDITRANSPORT_H
