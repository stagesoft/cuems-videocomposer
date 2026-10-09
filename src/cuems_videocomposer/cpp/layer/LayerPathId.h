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

#ifndef VIDEOCOMPOSER_LAYERPATHID_H
#define VIDEOCOMPOSER_LAYERPATHID_H

#include <algorithm>
#include <cctype>
#include <string>

namespace videocomposer {

/**
 * True when a /videocomposer/layer/<id>/... path segment addresses a layer by
 * its integer id: non-empty, all digits, at most 9 of them (always fits an
 * int). A cue UUID is never one — std::atoi("2ac1fe93-...") is 2, and treating
 * it as an integer id applied commands for a removed cue layer to whichever
 * live layer had that id (869f8j1ja).
 */
inline bool isIntegerLayerId(const std::string& id) {
    return !id.empty() && id.size() <= 9 &&
           std::all_of(id.begin(), id.end(),
                       [](unsigned char c) { return std::isdigit(c) != 0; });
}

} // namespace videocomposer

#endif // VIDEOCOMPOSER_LAYERPATHID_H
