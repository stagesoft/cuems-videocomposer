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

#include "CadenceStats.h"
#include "../utils/Logger.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <iomanip>

namespace videocomposer {

namespace {
// A ratio within this of an integer is treated as that integer (30.000 Hz
// against 30 fps, 50 Hz against 25 fps). 29.97 on 30 Hz (r = 1.001) is not.
constexpr double kIntegerRatioEps = 1e-4;
// A forward step this large or larger is a seek / resync, not cadence.
constexpr int64_t kDiscontinuityFrames = 3;
// A backwards step landing this close to the start is a loop wrap.
constexpr int64_t kWrapStartFrames = 2;
}  // namespace

bool CadenceStats::enabledFromEnv() {
    const char* v = std::getenv("VIDEOCOMPOSER_CADENCE_STATS");
    return v && std::strcmp(v, "0") != 0 && v[0] != '\0';
}

std::pair<int64_t, int64_t> CadenceStats::allowedHolds(double refreshHz, double contentFps) {
    if (refreshHz <= 0.0 || contentFps <= 0.0) {
        return {0, 0};
    }
    const double r = refreshHz / contentFps;
    const double nearest = std::round(r);
    if (std::abs(r - nearest) < kIntegerRatioEps * r) {
        const auto k = static_cast<int64_t>(std::max(1.0, nearest));
        return {k, k};
    }
    const auto lo = static_cast<int64_t>(std::floor(r));
    return {std::max<int64_t>(lo, 1), lo + 1};
}

void CadenceStats::onPresented(const std::string& output, int64_t msc, double refreshHz,
                               const std::vector<Sample>& samples, int64_t nowNs) {
    for (auto& [key, track] : tracks_) {
        if (key.first == output) {
            track.seen = false;
        }
    }

    for (const auto& s : samples) {
        Track& t = tracks_[Key{output, s.layerId}];
        t.seen = true;
        Counters& c = t.c;
        c.presents++;
        c.refreshHz = refreshHz;
        if (s.contentFps > 0.0) {
            c.contentFps = s.contentFps;
        }
        if (!t.have) {
            t.have = true;
            t.lastFrame = s.frame;
            t.lastChangeMsc = msc;
            t.lastLogNs = nowNs;
            continue;
        }
        if (s.frame == t.lastFrame) {
            continue;
        }

        const int64_t delta = s.frame - t.lastFrame;
        const int64_t hold = msc - t.lastChangeMsc;
        bool countHold = hold > 0;
        if (delta < 0) {
            if (s.frame <= kWrapStartFrames) {
                c.wraps++;
            } else {
                c.stepsBack++;
            }
        } else if (delta >= kDiscontinuityFrames) {
            c.discontinuities++;
            countHold = false;
        } else if (delta == 2) {
            c.skippedFrames++;
        }

        if (countHold) {
            c.changes++;
            c.holds[hold]++;
            const auto [lo, hi] = allowedHolds(c.refreshHz, c.contentFps);
            if (lo > 0 && hold != lo && hold != hi) {
                c.outsideA++;
            }
            if (lo > 0 && lo != hi) {
                const double r = c.refreshHz / c.contentFps;
                const double fracHi = r - static_cast<double>(lo);  // share of hi-holds
                const bool hiIsMinority = fracHi < 0.5;
                const int64_t minorityHold = hiIsMinority ? hi : lo;
                if (hold == minorityHold) {
                    c.minority++;
                }
                c.minorityExpected = static_cast<double>(c.changes)
                                     * (hiIsMinority ? fracHi : 1.0 - fracHi);
            }
        }
        t.lastFrame = s.frame;
        t.lastChangeMsc = msc;
    }

    // Periodic line per (output, layer); a layer gone from this output's
    // composite gets its summary and is dropped.
    for (auto it = tracks_.begin(); it != tracks_.end();) {
        if (it->first.first != output) {
            ++it;
            continue;
        }
        Track& t = it->second;
        if (!t.seen) {
            LOG_INFO << "CadenceStats: final " << format(it->first.first, it->first.second, t.c);
            it = tracks_.erase(it);
            continue;
        }
        if (nowNs - t.lastLogNs >= logIntervalNs_) {
            LOG_INFO << "CadenceStats: " << format(it->first.first, it->first.second, t.c);
            t.lastLogNs = nowNs;
        }
        ++it;
    }
}

void CadenceStats::logFinal() {
    for (const auto& [key, track] : tracks_) {
        LOG_INFO << "CadenceStats: final " << format(key.first, key.second, track.c);
    }
    tracks_.clear();
}

const CadenceStats::Counters* CadenceStats::counters(const std::string& output, int layerId) const {
    auto it = tracks_.find(Key{output, layerId});
    return it == tracks_.end() ? nullptr : &it->second.c;
}

std::string CadenceStats::format(const std::string& output, int layerId, const Counters& c) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(3);
    os << "output=" << output << " layer=" << layerId
       << " fps=" << c.contentFps << " hz=" << c.refreshHz
       << " presents=" << c.presents << " changes=" << c.changes << " holds{";
    bool first = true;
    for (const auto& [len, n] : c.holds) {
        os << (first ? "" : ",") << len << ":" << n;
        first = false;
    }
    os << "} outsideA=" << c.outsideA
       << " minority=" << c.minority << " expected=" << std::setprecision(1) << c.minorityExpected
       << " skipped=" << c.skippedFrames << " back=" << c.stepsBack
       << " wraps=" << c.wraps << " disc=" << c.discontinuities;
    return os.str();
}

} // namespace videocomposer
