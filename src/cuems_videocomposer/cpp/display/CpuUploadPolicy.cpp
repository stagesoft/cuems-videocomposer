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

#include "CpuUploadPolicy.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace videocomposer {
namespace cpuupload {

bool Policy::needsUpload(const Identity& draw) const {
    return !have_ || draw != last_;
}

Decision Policy::finish(Staging staging, const Identity& draw, int64_t nowNs) {
    Decision d;
    switch (staging) {
        case Staging::Ok:
            have_ = true;
            last_ = draw;
            d.recorded = true;
            return d;
        case Staging::NoPbo:
            // The texture holds this frame (direct upload), so an equal draw can skip.
            have_ = true;
            last_ = draw;
            d.directFromCpu = true;
            d.recorded = true;
            return d;
        case Staging::MapFailed:
        case Staging::UnmapFailed:
            break;
    }
    // Upload this frame straight from the CPU buffer and record nothing: the next draw (the same frame
    // included) goes through the PBO again.
    have_ = false;
    d.directFromCpu = true;
    if (!warned_ || nowNs - lastWarnNs_ >= kWarnIntervalNs) {
        d.warn = true;
        d.suppressed = suppressed_;
        suppressed_ = 0;
        warned_ = true;
        lastWarnNs_ = nowNs;
    } else {
        ++suppressed_;
    }
    return d;
}

void Policy::forget() {
    have_ = false;
}

bool statsEnabledFromEnv() {
    const char* v = std::getenv("VIDEOCOMPOSER_CADENCE_STATS");
    return v && std::strcmp(v, "0") != 0 && v[0] != '\0';
}

void Stats::add(double ms, bool direct, int width, int height, int64_t nowNs) {
    if (!open_) {
        open_ = true;
        windowStartNs_ = nowNs;
    }
    ms_.push_back(ms);
    if (direct) {
        ++direct_;
    }
    width_ = width;
    height_ = height;
}

bool Stats::takeLine(int layerId, int64_t nowNs, std::string& line) {
    if (!open_ || nowNs - windowStartNs_ < kIntervalNs) {
        return false;
    }
    line = format(layerId, false);
    reset();
    return true;
}

bool Stats::takeFinalLine(int layerId, std::string& line) {
    if (!open_) {
        return false;
    }
    line = format(layerId, true);
    reset();
    return true;
}

std::string Stats::format(int layerId, bool isFinal) const {
    std::vector<double> sorted(ms_);
    std::sort(sorted.begin(), sorted.end());
    const size_t n = sorted.size();
    double sum = 0.0;
    for (double v : sorted) {
        sum += v;
    }
    const double mean = n ? sum / static_cast<double>(n) : 0.0;
    // Nearest rank: the smallest value with at least 95 % of the samples at or below it.
    const size_t rank = n ? static_cast<size_t>(std::ceil(0.95 * static_cast<double>(n))) : 0;
    const double p95 = n ? sorted[std::min(n, std::max<size_t>(rank, 1)) - 1] : 0.0;
    const double max = n ? sorted.back() : 0.0;

    std::ostringstream o;
    o << "CpuUploadStats: layer=" << layerId << (isFinal ? " final" : "") << " size=" << width_ << "x" << height_
      << " n=" << n << std::fixed << std::setprecision(3) << " mean=" << mean << "ms p95=" << p95
      << "ms max=" << max << "ms direct=" << direct_;
    return o.str();
}

void Stats::reset() {
    ms_.clear();
    direct_ = 0;
    open_ = false;
    windowStartNs_ = 0;
}

}  // namespace cpuupload
}  // namespace videocomposer
