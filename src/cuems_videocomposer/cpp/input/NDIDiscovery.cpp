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

#include "NDIDiscovery.h"
#include "../remote/JournalContract.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#ifdef HAVE_NDI_SDK
#include <Processing.NDI.Lib.h>
#endif

namespace videocomposer {

NDIDiscovery::~NDIDiscovery() {
    shutdown();
}

namespace {
constexpr int kExtraPassSeconds = 3;   // a black-holed IP must not hold busy_ longer
}

bool NDIDiscovery::start(int seconds, const std::string& extraIps) {
    seconds = std::max(1, std::min(10, seconds));
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true)) {
        LOG_INFO << journal::ndiDiscoverBusy();
        return false;
    }
    // The previous run has finished (busy_ was false): reap its thread.
    if (thread_.joinable()) {
        thread_.join();
    }
    abort_ = false;
    thread_ = std::thread(&NDIDiscovery::run, this, seconds, extraIps);
    return true;
}

void NDIDiscovery::shutdown() {
    abort_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}

#ifdef HAVE_NDI_SDK
namespace {
// Wait in slices so shutdown() is never held up for long.
void waitForSources(NDIlib_find_instance_t finder, int seconds, const std::atomic<bool>& abort) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!abort) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            break;
        }
        NDIlib_find_wait_for_sources(finder, static_cast<uint32_t>(std::min<long long>(left, 500)));
    }
}

size_t countIps(const std::string& list) {
    return list.empty() ? 0 : 1 + static_cast<size_t>(std::count(list.begin(), list.end(), ','));
}
}  // namespace
#endif

void NDIDiscovery::run(int seconds, std::string extraIps) {
#ifdef HAVE_NDI_SDK
    LOG_INFO << journal::ndiDiscoverStarted(seconds, countIps(extraIps));
    size_t found = 0;
    std::vector<std::pair<std::string, std::string>> plain;   // (name, address) of pass 1
    NDIlib_initialize();
    NDIlib_find_instance_t finder = NDIlib_find_create_v2();
    if (!finder) {
        LOG_WARNING << "NDI discover: could not create an NDI finder";
    } else {
        waitForSources(finder, seconds, abort_);
        if (!abort_) {
            uint32_t count = 0;
            const NDIlib_source_t* sources = NDIlib_find_get_current_sources(finder, &count);
            for (uint32_t i = 0; i < count; ++i) {
                const std::string name = sources[i].p_ndi_name ? sources[i].p_ndi_name : "";
                const std::string address = sources[i].p_url_address ? sources[i].p_url_address : "";
                LOG_INFO << journal::ndiDiscoverSource(name, address);
                plain.emplace_back(name, address);
            }
            found = count;
        }
        NDIlib_find_destroy(finder);
    }
    if (!abort_ && !extraIps.empty()) {
        // Pass 2: its own finder, asking those hosts directly. It also sees
        // mDNS sources again; only what pass 1 did not list is new.
        NDIlib_find_create_t cfg;
        cfg.show_local_sources = false;
        cfg.p_groups = nullptr;
        cfg.p_extra_ips = extraIps.c_str();
        NDIlib_find_instance_t byIp = NDIlib_find_create_v2(&cfg);
        if (!byIp) {
            LOG_WARNING << "NDI discover: could not create the extra-IP finder";
        } else {
            waitForSources(byIp, std::min(seconds, kExtraPassSeconds), abort_);
            if (!abort_) {
                uint32_t count = 0;
                const NDIlib_source_t* sources = NDIlib_find_get_current_sources(byIp, &count);
                for (uint32_t i = 0; i < count; ++i) {
                    const std::string name = sources[i].p_ndi_name ? sources[i].p_ndi_name : "";
                    const std::string address = sources[i].p_url_address ? sources[i].p_url_address : "";
                    if (std::find(plain.begin(), plain.end(), std::make_pair(name, address)) != plain.end()) {
                        continue;
                    }
                    LOG_INFO << journal::ndiDiscoverSourceByIp(name, address);
                    ++found;
                }
            }
            NDIlib_find_destroy(byIp);
        }
    }
    if (abort_) {
        LOG_INFO << "NDI discover: aborted (shutting down)";
    } else {
        LOG_INFO << journal::ndiDiscoverDone(found);
    }
#else
    (void)seconds;
    (void)extraIps;
    LOG_WARNING << journal::ndiDiscoverUnavailable();
    LOG_INFO << journal::ndiDiscoverDone(0);
#endif
    busy_ = false;
}

}  // namespace videocomposer
