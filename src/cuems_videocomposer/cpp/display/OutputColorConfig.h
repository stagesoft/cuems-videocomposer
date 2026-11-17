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

#ifndef VIDEOCOMPOSER_OUTPUTCOLORCONFIG_H
#define VIDEOCOMPOSER_OUTPUTCOLORCONFIG_H

#include "CubeLut.h"
#include "OutputColor.h"

#include <charconv>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace videocomposer {

/**
 * The display.conf side of per-output color (programme phase 3, plan 2.5). GL-free.
 *
 * Six optional keys per [output:<connector>] section:
 *   color_brightness, color_contrast, color_saturation, color_hue, color_gamma, color_lut
 *
 * Keys are matched exactly, like every other display.conf key in the VC parser: no
 * trimming, so an indented or spaced key is a different, unknown key (the engine's
 * configparser reader strips keys; that is why the paste-ready lines never carry a
 * space). Only the value is stripped of surrounding whitespace and of the '\r' that
 * getline leaves at the end of a CRLF line.
 */

inline constexpr std::string_view kColorKeyPrefix = "color_";
inline constexpr std::string_view kColorLutKey = "color_lut";

/** True for the six color keys (exact match). */
inline bool isOutputColorKey(std::string_view key) {
    if (key == kColorLutKey) return true;
    if (key.substr(0, kColorKeyPrefix.size()) != kColorKeyPrefix) return false;
    return findColorTrim(key.substr(kColorKeyPrefix.size())) != nullptr;
}

/** A value with surrounding spaces, tabs, '\r' and '\n' removed. */
inline std::string_view stripConfigValue(std::string_view value) {
    const char* ws = " \t\r\n";
    const auto first = value.find_first_not_of(ws);
    if (first == std::string_view::npos) return std::string_view();
    const auto last = value.find_last_not_of(ws);
    return value.substr(first, last - first + 1);
}

/**
 * A trim value as display.conf text: the shortest form that reads back to the same
 * float (std::to_chars), so a logged or saved line round-trips exactly ("1.08", "-3").
 */
inline std::string formatColorNumber(float value) {
    char buf[32];
    const auto res = std::to_chars(buf, buf + sizeof(buf), value);
    if (res.ec != std::errc()) return std::to_string(value);
    return std::string(buf, res.ptr);
}

enum class ColorKeyResult {
    Applied,  ///< stored as given (hue folded into [-180, 180], which is not a change of meaning)
    Clamped,  ///< out of range: stored clamped to the layer range
    Refused   ///< unparsable, non-finite or a refused LUT path: a trim keeps its value, the LUT is unset
};

/**
 * Apply one color key (isOutputColorKey(key) must hold) with its raw value text to
 * `color`. On Clamped and Refused, `detail` is the tail of the log line: what was
 * wrong and what is used instead. A relative color_lut is resolved under `lutDir`.
 */
inline ColorKeyResult applyOutputColorKey(std::string_view key, std::string_view rawValue,
                                          OutputColor& color, std::string& detail,
                                          std::string_view lutDir = kLutDirectory) {
    const std::string_view text = stripConfigValue(rawValue);
    detail.clear();

    if (key == kColorLutKey) {
        std::string resolved, error;
        if (!resolveConfigLutPath(text, resolved, error, lutDir)) {
            color.lutPath.clear();
            color.lutValue.clear();
            color.lutStatus = LutStatus::None;
            detail = "refused: " + error + " — no LUT configured";
            return ColorKeyResult::Refused;
        }
        color.lutPath = resolved;
        color.lutValue = std::string(text);
        color.lutStatus = LutStatus::None;  // set when the backend loads it
        return ColorKeyResult::Applied;
    }

    const ColorTrimSpec* spec = findColorTrim(key.substr(kColorKeyPrefix.size()));
    if (!spec) {
        detail = "unknown color key";
        return ColorKeyResult::Refused;
    }
    float value = 0.0f;
    if (!parseFiniteFloat(text, value)) {
        detail = "not a finite number — ignored, " + std::string(key) + " stays " +
                 formatColorNumber(color.get(spec->trim));
        return ColorKeyResult::Refused;
    }
    if (!color.set(spec->trim, value)) {
        detail = "out of range (" + formatColorNumber(spec->min) + " to " +
                 formatColorNumber(spec->max) + ") — using " +
                 formatColorNumber(color.get(spec->trim));
        return ColorKeyResult::Clamped;
    }
    return ColorKeyResult::Applied;
}

/**
 * The color of one output as display.conf key/value pairs: only the trims off their
 * default, in the order the shader applies them, then color_lut when one is
 * configured (as the operator wrote it).
 */
inline std::vector<std::pair<std::string, std::string>> colorConfigEntries(const OutputColor& color) {
    std::vector<std::pair<std::string, std::string>> entries;
    for (const auto& spec : kColorTrimSpecs) {
        const float value = color.get(spec.trim);
        if (value != spec.identity) {
            entries.emplace_back(std::string(kColorKeyPrefix) + spec.name, formatColorNumber(value));
        }
    }
    if (color.lutConfigured()) {
        entries.emplace_back(std::string(kColorLutKey),
                             color.lutValue.empty() ? color.lutPath : color.lutValue);
    }
    return entries;
}

/** The same pairs as one paste-ready line: "color_contrast=1.08 color_lut=p2.cube". */
inline std::string colorConfigLine(const OutputColor& color) {
    std::string line;
    for (const auto& [key, value] : colorConfigEntries(color)) {
        if (!line.empty()) line += ' ';
        line += key;
        line += '=';
        line += value;
    }
    return line;
}

} // namespace videocomposer

#endif // VIDEOCOMPOSER_OUTPUTCOLORCONFIG_H
