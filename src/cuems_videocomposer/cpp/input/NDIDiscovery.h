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

#ifndef VIDEOCOMPOSER_NDIDISCOVERY_H
#define VIDEOCOMPOSER_NDIDISCOVERY_H

#include <atomic>
#include <thread>

namespace videocomposer {

/**
 * On-demand NDI source discovery for /videocomposer/ndi/discover.
 *
 * The videocomposer has no OSC reply channel, so the result goes to the log
 * (journal::ndiDiscoverSource / ndiDiscoverDone), where the controller's NDI
 * preview tool reads it. One discovery at a time: a request while one runs
 * logs journal::ndiDiscoverBusy() and is dropped. Runs on its own thread so
 * the render loop never waits for it; shutdown() aborts it within ~0.5 s.
 */
class NDIDiscovery {
public:
    NDIDiscovery() = default;
    ~NDIDiscovery();

    NDIDiscovery(const NDIDiscovery&) = delete;
    NDIDiscovery& operator=(const NDIDiscovery&) = delete;

    // seconds is clamped to 1..10. Returns false when one is already running.
    bool start(int seconds);
    void shutdown();

private:
    void run(int seconds);

    std::thread thread_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> abort_{false};
};

}  // namespace videocomposer

#endif  // VIDEOCOMPOSER_NDIDISCOVERY_H
