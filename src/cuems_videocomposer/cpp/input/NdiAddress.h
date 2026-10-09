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

#ifndef VIDEOCOMPOSER_NDIADDRESS_H
#define VIDEOCOMPOSER_NDIADDRESS_H

// "ndi://@<ipv4>:<port>": open the NDI source at that address, without
// discovery (plan rev 9 §2.5). NDI names are always "HOST (source)", so a
// leading '@' can never be a name. IPv4 only.

#include <arpa/inet.h>
#include <cstdlib>
#include <string>

namespace videocomposer {

// spec is the part after "ndi://". Returns true and the "ip:port" in addr for
// the address form; false with err set when it starts with '@' but is
// malformed. Callers check spec[0] == '@' first.
inline bool parseNdiAddress(const std::string& spec, std::string& addr, std::string& err) {
    if (spec.empty() || spec[0] != '@') {
        err = "not an address";
        return false;
    }
    const std::string rest = spec.substr(1);
    const size_t colon = rest.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= rest.size()) {
        err = "expected @<ipv4>:<port>";
        return false;
    }
    const std::string ip = rest.substr(0, colon);
    const std::string port = rest.substr(colon + 1);
    in_addr tmp;
    if (::inet_pton(AF_INET, ip.c_str(), &tmp) != 1) {
        err = "not an IPv4 address: " + ip;
        return false;
    }
    if (port.find_first_not_of("0123456789") != std::string::npos || port.size() > 5) {
        err = "bad port: " + port;
        return false;
    }
    const long p = std::strtol(port.c_str(), nullptr, 10);
    if (p < 1 || p > 65535) {
        err = "bad port: " + port;
        return false;
    }
    addr = ip + ":" + port;
    return true;
}

}  // namespace videocomposer

#endif  // VIDEOCOMPOSER_NDIADDRESS_H
