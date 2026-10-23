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

#ifndef VIDEOCOMPOSER_OUTPUTCOLOR_H
#define VIDEOCOMPOSER_OUTPUTCOLOR_H

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>

namespace videocomposer {

/**
 * OutputColor - per-output color correction (programme phase 3, D4/D13).
 *
 * The five trims of the per-layer controls (same maths, same ranges, same
 * "active" rule as LayerProperties::ColorAdjustment) plus an optional 3D LUT,
 * applied inside the output blit pass: trims -> LUT -> blend ramp.
 *
 * GL-free on purpose: the parser, the OSC handler, the tests and the rig-row
 * comparator all use it without a context.
 */

/** State of an output's LUT as reported in the journal, output/list and the contract. */
enum class LutStatus {
    None,     ///< no LUT configured
    Ok,       ///< parsed and uploaded
    Missing,  ///< the file does not exist (or a directory on its path does not)
    Invalid   ///< exists but is unreadable, not a regular file, too large or malformed
};

inline const char* lutStatusName(LutStatus s) {
    switch (s) {
        case LutStatus::None:    return "none";
        case LutStatus::Ok:      return "ok";
        case LutStatus::Missing: return "missing";
        case LutStatus::Invalid: return "invalid";
    }
    return "none";
}

/** The five trims, in the order applyColorCorrection() applies them. */
enum class ColorTrim { Brightness, Contrast, Saturation, Hue, Gamma };

struct ColorTrimSpec {
    ColorTrim trim;
    const char* name;  ///< OSC parameter name; the display.conf key is "color_" + name
    float min;
    float max;
    float identity;
};

/** Ranges copied from the layer handlers (RemoteCommandRouter::handleLayer*). Hue is folded, not clamped. */
inline constexpr ColorTrimSpec kColorTrimSpecs[] = {
    {ColorTrim::Brightness, "brightness", -1.0f, 1.0f, 0.0f},
    {ColorTrim::Contrast,   "contrast",    0.0f, 2.0f, 1.0f},
    {ColorTrim::Saturation, "saturation",  0.0f, 2.0f, 1.0f},
    {ColorTrim::Hue,        "hue",      -180.0f, 180.0f, 0.0f},
    {ColorTrim::Gamma,      "gamma",       0.1f, 3.0f, 1.0f},
};

inline const ColorTrimSpec* findColorTrim(std::string_view name) {
    for (const auto& spec : kColorTrimSpecs) {
        if (name == spec.name) return &spec;
    }
    return nullptr;
}

/**
 * Parse one number the way every phase-3 input does (display.conf values, OSC
 * arguments, .cube tokens): surrounding whitespace and '\r' stripped, one leading
 * '+' allowed, the whole token consumed by std::from_chars (locale-independent),
 * and non-finite values refused. from_chars itself accepts "inf"/"nan", so the
 * isfinite check is what refuses them.
 */
inline bool parseFiniteFloat(std::string_view text, float& out) {
    const char* ws = " \t\r\n";
    const auto first = text.find_first_not_of(ws);
    if (first == std::string_view::npos) return false;
    const auto last = text.find_last_not_of(ws);
    text = text.substr(first, last - first + 1);
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
        if (!text.empty() && (text.front() == '+' || text.front() == '-')) return false;
    }
    if (text.empty()) return false;
    float value = 0.0f;
    const auto res = std::from_chars(text.data(), text.data() + text.size(), value);
    if (res.ec != std::errc() || res.ptr != text.data() + text.size()) return false;
    if (!std::isfinite(value)) return false;
    out = value;
    return true;
}

/**
 * Fold a hue angle into [-180, 180] without a loop. The layer handlers use
 * `while (hue > 180) hue -= 360`, which never ends for inf or |h| >~ 1e11 (869c3p77c);
 * this gives the same result as that loop for every finite input (180 stays 180,
 * -180 stays -180, 540 -> 180, -540 -> -180) and is O(1). Callers refuse non-finite
 * values before calling it.
 */
inline float foldHue(float hue) {
    double h = std::fmod(static_cast<double>(hue), 360.0);
    if (h > 180.0) h -= 360.0;
    else if (h < -180.0) h += 360.0;
    return static_cast<float>(h);
}

struct OutputColor {
    float brightness = 0.0f;  // -1.0 to 1.0 (0 = no change)
    float contrast = 1.0f;    // 0.0 to 2.0 (1 = no change)
    float saturation = 1.0f;  // 0.0 to 2.0 (1 = no change, 0 = grayscale)
    float hue = 0.0f;         // -180 to 180 degrees
    float gamma = 1.0f;       // 0.1 to 3.0 (1 = no change; > 1 brightens, pow(x, 1/g))

    std::string lutPath;      ///< resolved path of the configured LUT, empty when none
    std::string lutValue;     ///< the LUT as the operator wrote it (display.conf), for paste-ready lines
    LutStatus lutStatus = LutStatus::None;

    /** Same rule as LayerProperties::ColorAdjustment::isActive(): any trim off its default. */
    bool trimsActive() const {
        return brightness != 0.0f || contrast != 1.0f ||
               saturation != 1.0f || hue != 0.0f || gamma != 1.0f;
    }

    bool lutConfigured() const { return !lutPath.empty(); }

    void resetTrims() {
        brightness = 0.0f;
        contrast = 1.0f;
        saturation = 1.0f;
        hue = 0.0f;
        gamma = 1.0f;
    }

    float get(ColorTrim t) const {
        switch (t) {
            case ColorTrim::Brightness: return brightness;
            case ColorTrim::Contrast:   return contrast;
            case ColorTrim::Saturation: return saturation;
            case ColorTrim::Hue:        return hue;
            case ColorTrim::Gamma:      return gamma;
        }
        return 0.0f;
    }

    /**
     * Store a finite value for a trim: hue folded, the others clamped to the layer
     * ranges. Returns false when the stored value differs from the one given
     * (clamped), so callers can log a WARNING naming the value used.
     */
    bool set(ColorTrim t, float value) {
        float stored = value;
        switch (t) {
            case ColorTrim::Brightness: stored = std::clamp(value, -1.0f, 1.0f); brightness = stored; break;
            case ColorTrim::Contrast:   stored = std::clamp(value, 0.0f, 2.0f);  contrast = stored;   break;
            case ColorTrim::Saturation: stored = std::clamp(value, 0.0f, 2.0f);  saturation = stored; break;
            case ColorTrim::Hue:        stored = foldHue(value);                 hue = stored;        return true;
            case ColorTrim::Gamma:      stored = std::clamp(value, 0.1f, 3.0f);  gamma = stored;      break;
        }
        return stored == value;
    }

    /**
     * CPU reference of the shader's applyColorCorrection() (display/VideoShaders.h), used by the
     * tests and the rig-row comparator. The hue rotation is the GLSL mat3 (column-major) written
     * out per channel, so a row/column mix-up cannot hide in a matrix literal here.
     */
    void apply(float& r, float& g, float& b) const {
        r += brightness; g += brightness; b += brightness;

        r = (r - 0.5f) * contrast + 0.5f;
        g = (g - 0.5f) * contrast + 0.5f;
        b = (b - 0.5f) * contrast + 0.5f;

        const float luma = r * 0.299f + g * 0.587f + b * 0.114f;
        r = luma + (r - luma) * saturation;
        g = luma + (g - luma) * saturation;
        b = luma + (b - luma) * saturation;

        if (hue != 0.0f) {
            const float rad = hue * 3.14159265359f / 180.0f;
            const float c = std::cos(rad);
            const float s = std::sin(rad);
            const float nr = (0.213f + c * 0.787f - s * 0.213f) * r
                           + (0.715f - c * 0.715f - s * 0.715f) * g
                           + (0.072f - c * 0.072f + s * 0.928f) * b;
            const float ng = (0.213f - c * 0.213f + s * 0.143f) * r
                           + (0.715f + c * 0.285f + s * 0.140f) * g
                           + (0.072f - c * 0.072f - s * 0.283f) * b;
            const float nb = (0.213f - c * 0.213f - s * 0.787f) * r
                           + (0.715f - c * 0.715f + s * 0.715f) * g
                           + (0.072f + c * 0.928f + s * 0.072f) * b;
            r = nr; g = ng; b = nb;
        }

        if (gamma != 1.0f) {
            const float e = 1.0f / gamma;
            r = std::pow(std::max(r, 0.0f), e);
            g = std::pow(std::max(g, 0.0f), e);
            b = std::pow(std::max(b, 0.0f), e);
        }

        r = std::clamp(r, 0.0f, 1.0f);
        g = std::clamp(g, 0.0f, 1.0f);
        b = std::clamp(b, 0.0f, 1.0f);
    }
};

/**
 * Outcome of a per-output color call on a display backend (display/color, plan 2.6).
 * The handler turns each into its own journal line, so a failure is never logged as
 * "Unknown output" unless it is one. The show lock is checked in the handler, before
 * any backend call, so it is not a backend status.
 */
enum class OutputColorStatus {
    Applied,             ///< done (for a read: the data is valid)
    UnknownOutput,       ///< no output region with that connector name
    Unsupported,         ///< no virtual canvas (legacy X11/Wayland, headless, DRM legacy mode)
    BadValue,            ///< a trim not finite or out of range: nothing changed
    LutFailed,           ///< the LUT could not be loaded or uploaded: the previous LUT is kept
    ProgramUnavailable   ///< stored on the output, but the color program did not link: no color on screen
};

inline const char* outputColorStatusName(OutputColorStatus s) {
    switch (s) {
        case OutputColorStatus::Applied:            return "applied";
        case OutputColorStatus::UnknownOutput:      return "unknown_output";
        case OutputColorStatus::Unsupported:        return "unsupported";
        case OutputColorStatus::BadValue:           return "bad_value";
        case OutputColorStatus::LutFailed:          return "lut_failed";
        case OutputColorStatus::ProgramUnavailable: return "program_unavailable";
    }
    return "unsupported";
}

/** One output's color as the backend holds it (display/color dump, output/list). */
struct OutputColorReport {
    std::string name;       ///< connector
    OutputColor color;      ///< trims + configured LUT and its status
    bool applied = false;   ///< color program linked and this output's color draw has not failed
};

/** What a hot LUT change did, for its journal lines. */
struct OutputLutLoadInfo {
    std::string error;               ///< reason when the status is LutFailed
    int size = 0;                    ///< LUT_3D_SIZE when loaded
    double parseMs = 0.0;            ///< read + parse time
    std::size_t clampedValues = 0;   ///< data values outside [0, 1] that were clamped
};

/**
 * Maps a color in the LUT's domain to a 3D texture coordinate on texel centres:
 * coord = x * scale + offset, with scale = (N-1) / (N * (dmax - dmin)) and
 * offset = 0.5/N - dmin * scale, per channel. These are the uLutScale/uLutOffset
 * uniforms of the color blit program. The .cube parser refuses dmax <= dmin, so
 * the division is safe.
 */
struct LutTransform {
    float scale[3] = {1.0f, 1.0f, 1.0f};
    float offset[3] = {0.0f, 0.0f, 0.0f};

    static LutTransform make(int size, const float domainMin[3], const float domainMax[3]) {
        LutTransform t;
        const float n = static_cast<float>(size);
        for (int c = 0; c < 3; ++c) {
            t.scale[c] = (n - 1.0f) / (n * (domainMax[c] - domainMin[c]));
            t.offset[c] = 0.5f / n - domainMin[c] * t.scale[c];
        }
        return t;
    }

    float coord(int channel, float x) const { return x * scale[channel] + offset[channel]; }
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_OUTPUTCOLOR_H
