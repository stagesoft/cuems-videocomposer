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


#include "DisplayBackendChoice.h"
#include "../utils/Logger.h"

#include <algorithm>
#include <cctype>

namespace videocomposer {
namespace displaybackend {

const char* name(Kind kind) {
    switch (kind) {
        case Kind::Drm: return "drm";
        case Kind::X11: return "x11";
        case Kind::Wayland: return "wayland";
    }
    return "drm";
}

const char* acceptedValues() { return "drm, x11, wayland"; }

bool parse(const std::string& value, Kind& out) {
    std::string v = value;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
    if (v == "drm") { out = Kind::Drm; return true; }
    if (v == "x11") { out = Kind::X11; return true; }
    if (v == "wayland") { out = Kind::Wayland; return true; }
    return false;
}

bool select(const std::string& option, Kind& out) {
    if (option.empty()) {
        out = Kind::Drm;
        return true;
    }
    return parse(option, out);
}

bool openExplicitDesktop(Kind kind, const std::function<bool(Kind)>& open) {
    if (open(kind)) return true;
    LOG_ERROR << "display backend " << name(kind) << " (--display-backend=" << name(kind)
              << ") failed to initialize - no other backend is tried; drop the flag to run on DRM";
    return false;
}

} // namespace displaybackend
} // namespace videocomposer
