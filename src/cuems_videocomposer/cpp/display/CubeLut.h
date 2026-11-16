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

#ifndef VIDEOCOMPOSER_CUBELUT_H
#define VIDEOCOMPOSER_CUBELUT_H

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace videocomposer {

/**
 * CubeLut - a parsed 3D `.cube` LUT (programme phase 3, plan §2.4). GL-free.
 *
 * Accepted: optional UTF-8 BOM, TITLE, LUT_3D_SIZE 2..65, DOMAIN_MIN/DOMAIN_MAX,
 * LUT_3D_INPUT_RANGE (mapped to the domain), '#' comment lines, blank lines, CRLF,
 * tabs, fixed or scientific notation, then exactly N^3 data lines of three numbers,
 * red varying fastest. Anything else is refused with a reason naming the line, and
 * nothing is applied: a LUT whose meaning depends on a keyword we do not implement
 * must never be applied with a wrong domain.
 */
struct CubeLut {
    static constexpr int kMinSize = 2;
    static constexpr int kMaxSize = 65;
    static constexpr std::size_t kMaxFileBytes = 16u * 1024u * 1024u;

    std::string title;
    int size = 0;
    float domainMin[3] = {0.0f, 0.0f, 0.0f};
    float domainMax[3] = {1.0f, 1.0f, 1.0f};
    /// size^3 RGB triplets, index (r + size * (g + size * b)) * 3, red fastest (the .cube order)
    std::vector<float> rgb;
    /// data values outside [0, 1] that were clamped (HDR-authored LUTs; the outputs are SDR scanout)
    std::size_t clampedValues = 0;

    std::size_t entryCount() const { return static_cast<std::size_t>(size) * size * size; }
};

/** Parse .cube text. On failure returns false, leaves `out` unspecified and sets `error`. */
bool parseCubeLut(std::string_view text, CubeLut& out, std::string& error);

/** Result of loading a .cube file from disk. */
struct CubeLoadResult {
    bool ok = false;
    bool notFound = false;  ///< open() failed with ENOENT/ENOTDIR: reported as "missing"
    std::string error;      ///< reason, empty when ok
    double parseMs = 0.0;   ///< read + parse time
};

/**
 * Load a .cube file safely: open(O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY), fstat the
 * descriptor, require a regular file (a FIFO, device or socket is refused before a byte is
 * read), refuse anything larger than CubeLut::kMaxFileBytes, read to EOF within that bound,
 * then parse.
 */
CubeLoadResult loadCubeLutFile(const std::string& path, CubeLut& out);

/** Where display.conf relative names and OSC bare names are resolved. */
inline constexpr const char* kLutDirectory = "/etc/cuems/luts";

/**
 * Resolve a display.conf `color_lut=` value: an absolute path is used as given; a relative
 * path is resolved under `baseDir`, and any ".." component is refused.
 */
bool resolveConfigLutPath(std::string_view value, std::string& resolved, std::string& error,
                          std::string_view baseDir = kLutDirectory);

/**
 * Resolve an OSC `lut` argument: bare names only (no '/', no "..", no control characters,
 * ending in ".cube"), resolved under `baseDir`. The OSC port is unauthenticated UDP, so it
 * never takes a path.
 */
bool resolveOscLutName(std::string_view name, std::string& resolved, std::string& error,
                       std::string_view baseDir = kLutDirectory);

} // namespace videocomposer

#endif // VIDEOCOMPOSER_CUBELUT_H
