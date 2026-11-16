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

#ifndef VIDEOCOMPOSER_MODESELECT_H
#define VIDEOCOMPOSER_MODESELECT_H

#include <algorithm>
#include <cmath>
#include <vector>

namespace videocomposer {

/**
 * Mode choice for a requested size and refresh (ClickUp 1amfwm7evfj, "R11").
 *
 * The VC computes a mode's rate as clock / (htotal * vtotal): the FRAME rate,
 * so 1080i60 reads as 30 Hz. Matching a bare refresh=30 against that picked
 * 1080i60 on .111 (2026-10-20), and on every monitor in the rigs 1080 at
 * 25/29.97/30 exists only as interlaced. Rules (Ion, 2026-10-21):
 *  - interlaced modes are used only when asked for explicitly (refresh=60i,
 *    matched against the FIELD rate, the number tools such as modetest show);
 *  - a progressive mode within 0.5 Hz of the request wins (closest first);
 *  - if the rate does not exist in progressive, the native progressive mode
 *    of that size is used (preferred, else the highest rate) and the caller
 *    logs an ERROR naming both;
 *  - only when the size has no progressive mode at all is an interlaced one
 *    taken (the nearest, as before) with a WARNING.
 */
struct ModeView {
    int width = 0;
    int height = 0;
    double frameHz = 0.0;
    bool interlaced = false;
    bool preferred = false;
};

struct ModePick {
    enum class Kind { None, Exact, NativeProgressive, OnlyInterlaced };
    int index = -1;
    Kind kind = Kind::None;
};

inline double fieldRateHz(const ModeView& m) {
    return m.interlaced ? m.frameHz * 2.0 : m.frameHz;
}

inline ModePick pickModeForRefresh(const std::vector<ModeView>& modes, int width, int height,
                                   double refresh, bool wantInterlaced) {
    constexpr double kTolerance = 0.5;  // same as harmonizeRefreshRates
    ModePick pick;
    auto sameSize = [&](const ModeView& m) { return m.width == width && m.height == height; };

    if (refresh > 0.0 && wantInterlaced) {
        double best = kTolerance;
        for (int i = 0; i < static_cast<int>(modes.size()); ++i) {
            const auto& m = modes[i];
            if (sameSize(m) && m.interlaced && std::abs(fieldRateHz(m) - refresh) <= best) {
                best = std::abs(fieldRateHz(m) - refresh);
                pick = {i, ModePick::Kind::Exact};
            }
        }
        if (pick.index >= 0) {
            return pick;
        }
    }
    if (refresh > 0.0 && !wantInterlaced) {
        double best = kTolerance;
        for (int i = 0; i < static_cast<int>(modes.size()); ++i) {
            const auto& m = modes[i];
            if (sameSize(m) && !m.interlaced && std::abs(m.frameHz - refresh) <= best) {
                best = std::abs(m.frameHz - refresh);
                pick = {i, ModePick::Kind::Exact};
            }
        }
        if (pick.index >= 0) {
            return pick;
        }
    }
    // Native progressive of that size: preferred, else the highest rate.
    int nativeIdx = -1;
    for (int i = 0; i < static_cast<int>(modes.size()); ++i) {
        const auto& m = modes[i];
        if (!sameSize(m) || m.interlaced) {
            continue;
        }
        if (nativeIdx < 0 || (m.preferred && !modes[nativeIdx].preferred) ||
            (m.preferred == modes[nativeIdx].preferred && m.frameHz > modes[nativeIdx].frameHz)) {
            nativeIdx = i;
        }
    }
    if (nativeIdx >= 0) {
        return {nativeIdx, refresh > 0.0 ? ModePick::Kind::NativeProgressive : ModePick::Kind::Exact};
    }
    // No progressive mode of that size: nearest interlaced (frame-rate distance, as before).
    double best = 1e9;
    for (int i = 0; i < static_cast<int>(modes.size()); ++i) {
        const auto& m = modes[i];
        if (!sameSize(m)) {
            continue;
        }
        const double d = refresh > 0.0 ? std::abs(m.frameHz - refresh) : (m.preferred ? -1.0 : 0.0);
        if (d < best) {
            best = d;
            pick = {i, ModePick::Kind::OnlyInterlaced};
        }
    }
    return pick;
}

/**
 * The "refresh=<pin> not available" WARNING of the refresh rule (1amfwm7gq3y), as pure functions, so that what it
 * says agrees with what pickModeForRefresh() does.
 */

/// The rates a refresh= pin can obtain at a size: progressive modes only, ascending, one entry per distinct rate
/// (within 0.0005 Hz). An interlaced mode is never taken by a bare refresh= (rules above), so its frame rate
/// (1080i60 = 30) is not listed: it would send the operator back to the pin that was just refused.
inline std::vector<double> offeredRefreshRates(const std::vector<ModeView>& modes, int width, int height) {
    std::vector<double> rates;
    for (const auto& m : modes) {
        if (m.width != width || m.height != height || m.frameHz <= 0.0 || m.interlaced) {
            continue;
        }
        const double hz = m.frameHz;
        if (std::none_of(rates.begin(), rates.end(), [hz](double r) { return std::abs(r - hz) < 0.0005; })) {
            rates.push_back(hz);
        }
    }
    std::sort(rates.begin(), rates.end());
    return rates;
}

/// Whether the mode chosen for a pinned refresh= is not what was asked for, in the terms of the WARNING (progressive
/// rates). `askedInterlaced` is the trailing "i" of refresh=<field rate>i.
///  - An interlaced choice is never reported here: the selection has its own line for it (asked for: INFO with the
///    field rate; the only kind at that size: WARNING), and its frame rate is half the number that was asked.
///  - refresh=<n>i that ended on a progressive mode is always reported, even at the same number.
///  - Otherwise: the chosen rate differs from the pin by more than `toleranceHz`.
inline bool refreshPinNotHonoured(double askedHz, bool askedInterlaced, const ModeView& chosen, double toleranceHz) {
    if (askedHz <= 0.0 || chosen.interlaced) {
        return false;
    }
    if (askedInterlaced) {
        return true;
    }
    return std::abs(chosen.frameHz - askedHz) > toleranceHz;
}

} // namespace videocomposer

#endif // VIDEOCOMPOSER_MODESELECT_H
