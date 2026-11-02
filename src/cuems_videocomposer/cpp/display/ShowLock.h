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

#ifndef VIDEOCOMPOSER_SHOWLOCK_H
#define VIDEOCOMPOSER_SHOWLOCK_H

// The engine's show lock as a gate for display commands (programme phase 3, plan 2.6 E2).
//
// The engines write /tmp/cuems.show.lock when a project is loaded and remove it on
// unload (cuems-engine >= 0.1.0rc6-6; older engines remove it only when they stop).
// Until the VC has an edit mode, every display command that rebuilds the renderer,
// modesets or resets the configuration manager is refused while the file exists:
// display/color (except dump), display/blend, display/region, display/mode,
// display/resolution_mode and display/load. The gate is called by the handlers, as
// their first real statement, never inside the backend methods: a backend `false`
// is logged as "Unknown output" by the handlers, which a refusal must never be.

#include "../utils/Logger.h"

#include <cerrno>
#include <string>
#include <sys/stat.h>

namespace videocomposer {

inline constexpr const char* kShowLockPath = "/tmp/cuems.show.lock";

namespace showlock_detail {
// Test-only override of the lock path (empty = kShowLockPath). One instance for
// the whole program: an inline function's static is shared by every TU.
inline std::string& pathOverride() {
    static std::string path;
    return path;
}
}  // namespace showlock_detail

/** Tests only: point the gate at another file; an empty path restores kShowLockPath. */
inline void setShowLockPathForTesting(const std::string& path) {
    showlock_detail::pathOverride() = path;
}

/** The lock file the gate checks: kShowLockPath unless a test changed it. */
inline const char* showLockPath() {
    const std::string& path = showlock_detail::pathOverride();
    return path.empty() ? kShowLockPath : path.c_str();
}

/**
 * True when the lock exists. Anything at the path counts (lstat, so a dangling
 * symlink too). Only "does not exist" reads as absent: any other stat error means
 * the VC cannot tell, and a display command under a show is the worse mistake.
 */
inline bool showLockPresent(const char* path) {
    struct stat st;
    if (::lstat(path, &st) == 0) {
        return true;
    }
    return errno != ENOENT && errno != ENOTDIR;
}

inline bool showLockPresent() {
    return showLockPresent(showLockPath());
}

/** The refusal line: names the path, both causes, and what to do. */
inline std::string showLockRefusalLine(const std::string& command, const char* path) {
    return command + " REFUSED: " + path +
           " exists — a project is loaded, or the lock was left by an engine older than"
           " cuems-engine 0.1.0rc6-6 or by a crashed engine; unload the project (engine"
           " ≥ rc6-6) or stop the engines, then retry; color is also applied from"
           " display.conf at the next start";
}

/**
 * The gate: when the lock exists, log the refusal as a WARNING and return true
 * (the caller returns without changing anything). `command` is the handler's own
 * literal ("display/blend"), never text from the wire.
 */
inline bool refuseUnderShowLock(const std::string& command) {
    const char* path = showLockPath();
    if (!showLockPresent(path)) {
        return false;
    }
    LOG_WARNING << showLockRefusalLine(command, path);
    return true;
}

}  // namespace videocomposer

#endif  // VIDEOCOMPOSER_SHOWLOCK_H
