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

#include "CadenceAdvice.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace videocomposer {
namespace cadence {

namespace {

std::string fixed(double v, int places) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", places, v);
    return buf;
}

/// "4.8 s", "21 s", "12 min", "3.2 h".
std::string formatInterval(double s) {
    if (s < 10.0) return fixed(s, 1) + " s";
    if (s < 120.0) return fixed(s, 0) + " s";
    if (s < 7200.0) return fixed(s / 60.0, 0) + " min";
    return fixed(s / 3600.0, 1) + " h";
}

std::string formatHz(double hz) { return fixed(hz, 3); }

/// floor(x + 0.5), halves away from zero for x ≥ 0 (the cadence engine's rounding).
int64_t roundHalfUp(double x) { return static_cast<int64_t>(std::floor(x + 0.5)); }

/// The rates content is really encoded at: a suggestion of "29.975 fps" helps nobody.
constexpr double kStandardFps[] = {24000.0 / 1001.0, 24.0, 25.0, 30000.0 / 1001.0, 30.0,
                                   50.0, 60000.0 / 1001.0, 60.0};

/// Seconds between single-vsync corrections for content at fps held n vsyncs of period (∞ when exact).
double correctionInterval(double fps, int n, double period) {
    const double e = std::fabs(static_cast<double>(n) * period * fps - 1.0);
    return e <= kExactRelTol ? INFINITY : period / e;
}

}  // namespace

std::string formatFps(double fps) {
    std::string s = fixed(fps, 3);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

Advice advise(double fps, int64_t periodNs, double warnBelowS) {
    Advice a;
    if (!(fps > 0.0)) {
        a.text = "fps unknown: no cadence advice";
        return a;
    }
    if (periodNs <= 0) {
        a.text = formatFps(fps) + " fps: no vsync period known yet (no vblank tick, no output mode): the ratio is "
                 "logged at reveal";
        return a;
    }
    const double period = static_cast<double>(periodNs) * 1e-9;
    a.hz = 1.0 / period;
    a.r = (1.0 / fps) / period;
    const std::string head = formatFps(fps) + " fps on " + formatHz(a.hz) + " Hz: ";
    const int64_t n = roundHalfUp(a.r);

    if (n >= 1 && std::fabs(a.r - static_cast<double>(n)) <= kNearIntegerTol) {
        a.n = static_cast<int>(n);
        // One content frame lasts 1/fps; held n vsyncs it lasts n·period. The mismatch e = |n·period·fps − 1|
        // accumulates e/fps seconds per frame, one whole vsync after period·fps/e frames = period/e seconds.
        const double e = std::fabs(static_cast<double>(n) * period * fps - 1.0);
        a.exactHz = static_cast<double>(n) * fps;
        if (e <= kExactRelTol) {
            a.kind = Kind::Exact;
            a.text = head + "ratio " + fixed(a.r, 4) + " -> hold " + std::to_string(a.n) + " (exact)";
            return a;
        }
        a.kind = Kind::NearInteger;
        a.intervalS = period / e;
        a.warning = a.intervalS < warnBelowS;
        a.text = head + "near-integer ratio " + fixed(a.r, 4) + " (the output runs " + formatHz(a.hz) + " Hz, not " +
                 formatHz(a.exactHz) + "): hold " + std::to_string(a.n) +
                 " with one single-vsync correction every ~" + formatInterval(a.intervalS) + "; use a " +
                 formatHz(a.exactHz) + " Hz mode";
        // A standard content rate for this output, only when it really is better: no correction faster than the
        // warning threshold (59.950 Hz: 29.97p corrects every ~100 s; 59.790 Hz: no standard rate helps).
        double best = 0.0, bestInterval = a.intervalS;
        for (double f : kStandardFps) {
            const double r = (1.0 / f) / period;
            if (roundHalfUp(r) != n || std::fabs(r - static_cast<double>(n)) > kNearIntegerTol) continue;
            const double iv = correctionInterval(f, a.n, period);
            if (iv > bestInterval) { best = f; bestInterval = iv; }
        }
        if (best > 0.0 && bestInterval >= warnBelowS) {
            a.text += " or content at " + formatFps(best) + " fps" +
                      (std::isinf(bestInterval) ? std::string(" (exact)")
                                                : " (a correction every ~" + formatInterval(bestInterval) + ")");
        }
        return a;
    }

    a.warning = true;
    if (a.r < kDropTol) {
        a.kind = Kind::Drop;
        a.dropEvery = 1.0 / (1.0 - a.r);
        a.exactHz = fps;
        a.text = head + "ratio " + fixed(a.r, 4) + " -> drop regime: about 1 frame in " + fixed(a.dropEvery, 1) +
                 " never shown; exact would be " + formatHz(a.exactHz) + " Hz";
        return a;
    }
    a.kind = Kind::NonInteger;
    const int lo = static_cast<int>(std::floor(a.r));
    const int hi = lo + 1;
    // The exact refresh to suggest: the multiple of fps nearest the current one (ties: the lower).
    const double loHz = static_cast<double>(std::max(lo, 1)) * fps;
    const double hiHz = static_cast<double>(hi) * fps;
    a.exactHz = (std::fabs(loHz - a.hz) <= std::fabs(hiHz - a.hz)) ? loHz : hiHz;
    a.text = head + "non-integer ratio " + fixed(a.r, 4) + " -> " + std::to_string(lo) + "," + std::to_string(hi) +
             " pulldown (a regular judder by construction); exact would be " + formatHz(a.exactHz) + " Hz";
    return a;
}

}  // namespace cadence
}  // namespace videocomposer
