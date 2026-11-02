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

#ifndef VIDEOCOMPOSER_CAPSFILE_H
#define VIDEOCOMPOSER_CAPSFILE_H

// The capability file the node engine reads at every project load (plan
// 869ejxng3 + 1amfwm7e6cw, section 4.2): /run/cuems/videocomposer.caps, the
// writer's pid on the first line (`pid=<n>`), then one token per line. The
// engine ignores a file whose pid is gone or does not name a videocomposer,
// so a file left by a crash cannot make an old binary look capable (J14).
// /run/cuems is the unit's RuntimeDirectory, but rtpmidid declares it with
// Preserve=yes, so the file is unlinked at exit here, not by systemd.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace videocomposer {
namespace capsfile {

constexpr const char* DEFAULT_PATH = "/run/cuems/videocomposer.caps";

// The tokens of this build. `loop_period`: /layer/<id>/loop/period and the
// passes-aware wrap of section 4.4.
inline std::vector<std::string> tokens() { return {"loop_period"}; }

inline std::string render(long pid, const std::vector<std::string>& toks) {
    std::ostringstream o;
    o << "pid=" << pid << "\n";
    for (const std::string& t : toks) o << t << "\n";
    return o.str();
}

// Written atomically (temp + rename). False with `err` set on failure.
inline bool write(const std::string& path, long pid, const std::vector<std::string>& toks, std::string& err) {
    const std::string tmp = path + ".tmp." + std::to_string(pid);
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) {
            err = "cannot create " + tmp;
            return false;
        }
        f << render(pid, toks);
        if (!f.good()) {
            err = "cannot write " + tmp;
            std::remove(tmp.c_str());
            return false;
        }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        err = "cannot rename " + tmp + " to " + path;
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

// The pid a caps file names, or -1.
inline long readPid(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    if (!f || !std::getline(f, line) || line.rfind("pid=", 0) != 0) return -1;
    try {
        return std::stol(line.substr(4));
    } catch (...) {
        return -1;
    }
}

// Unlinked only when it is still this process's file (a newer instance may
// have replaced it).
inline bool removeIfOurs(const std::string& path, long pid) {
    if (readPid(path) != pid) return false;
    return std::remove(path.c_str()) == 0;
}

} // namespace capsfile
} // namespace videocomposer

#endif // VIDEOCOMPOSER_CAPSFILE_H
