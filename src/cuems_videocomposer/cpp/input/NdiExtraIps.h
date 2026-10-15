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

#ifndef VIDEOCOMPOSER_NDIEXTRAIPS_H
#define VIDEOCOMPOSER_NDIEXTRAIPS_H

// The extra IPs an NDI finder asks directly for their sources, without mDNS
// (NDIlib_find_create_t::p_extra_ips): how the controller finds a sender on
// another subnet (plan 869ekxuez rev 12.2 §3.10). The argument comes from
// OSC, so it is checked strictly before it reaches the SDK: dotted IPv4
// only, comma-separated, at most kNdiMaxExtraIps, no spaces, no names.

#include <arpa/inet.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace videocomposer {

constexpr size_t kNdiMaxExtraIps = 8;

// Printable ASCII only, at most maxLen characters: safe to echo in a log line.
inline std::string ndiSanitizeForLog(const std::string& s, size_t maxLen = 64) {
    std::string out;
    for (char c : s) {
        if (out.size() >= maxLen) {
            break;
        }
        out += (c >= 0x20 && c < 0x7f) ? c : '?';
    }
    return out;
}

// arg: "10.20.0.15,10.20.0.16". On success normalized holds the canonical,
// de-duplicated list ("" for an empty arg) and count the number of IPs.
// On failure why says what is wrong and nothing should be searched.
inline bool parseNdiExtraIps(const std::string& arg, std::string& normalized, size_t& count,
                             std::string& why) {
    normalized.clear();
    count = 0;
    if (arg.empty()) {
        return true;
    }
    std::vector<std::string> ips;
    size_t start = 0;
    while (true) {
        const size_t comma = arg.find(',', start);
        const std::string item = arg.substr(start, comma == std::string::npos ? std::string::npos
                                                                               : comma - start);
        in_addr a;
        if (item.empty() || item.find_first_not_of("0123456789.") != std::string::npos ||
            ::inet_pton(AF_INET, item.c_str(), &a) != 1) {
            why = "not a dotted IPv4 address: '" + ndiSanitizeForLog(item, 32) + "'";
            return false;
        }
        const uint32_t h = ntohl(a.s_addr);
        if (h == 0 || h == 0xFFFFFFFFu || (h >> 24) == 127 || (h >> 28) == 0xE) {
            why = "not a unicast address: " + item;
            return false;
        }
        char buf[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &a, buf, sizeof(buf));
        if (std::find(ips.begin(), ips.end(), buf) == ips.end()) {
            ips.emplace_back(buf);
        }
        if (ips.size() > kNdiMaxExtraIps) {
            why = "more than " + std::to_string(kNdiMaxExtraIps) + " addresses";
            return false;
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    for (size_t i = 0; i < ips.size(); ++i) {
        normalized += (i ? "," : "") + ips[i];
    }
    count = ips.size();
    return true;
}

}  // namespace videocomposer

#endif  // VIDEOCOMPOSER_NDIEXTRAIPS_H
