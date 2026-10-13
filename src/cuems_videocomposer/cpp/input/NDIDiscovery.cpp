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

#ifdef HAVE_NDI_SDK
#include <Processing.NDI.Lib.h>
#endif

namespace videocomposer {

NDIDiscovery::~NDIDiscovery() {
    shutdown();
}

bool NDIDiscovery::start(int seconds) {
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
    thread_ = std::thread(&NDIDiscovery::run, this, seconds);
    return true;
}

void NDIDiscovery::shutdown() {
    abort_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}

void NDIDiscovery::run(int seconds) {
#ifdef HAVE_NDI_SDK
    LOG_INFO << journal::ndiDiscoverStarted(seconds);
    size_t found = 0;
    NDIlib_initialize();
    NDIlib_find_instance_t finder = NDIlib_find_create_v2();
    if (!finder) {
        LOG_WARNING << "NDI discover: could not create an NDI finder";
    } else {
        // Wait in slices so shutdown() is never held up for long.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (!abort_) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) {
                break;
            }
            NDIlib_find_wait_for_sources(finder, static_cast<uint32_t>(std::min<long long>(left, 500)));
        }
        if (!abort_) {
            uint32_t count = 0;
            const NDIlib_source_t* sources = NDIlib_find_get_current_sources(finder, &count);
            for (uint32_t i = 0; i < count; ++i) {
                const char* address = sources[i].p_url_address;
                LOG_INFO << journal::ndiDiscoverSource(sources[i].p_ndi_name ? sources[i].p_ndi_name : "",
                                                       address ? address : "");
            }
            found = count;
        }
        NDIlib_find_destroy(finder);
    }
    if (abort_) {
        LOG_INFO << "NDI discover: aborted (shutting down)";
    } else {
        LOG_INFO << journal::ndiDiscoverDone(found);
    }
#else
    (void)seconds;
    LOG_WARNING << journal::ndiDiscoverUnavailable();
    LOG_INFO << journal::ndiDiscoverDone(0);
#endif
    busy_ = false;
}

}  // namespace videocomposer
