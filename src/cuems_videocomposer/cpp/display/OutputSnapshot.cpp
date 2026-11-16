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

#include "OutputSnapshot.h"
#include "../utils/Logger.h"

#include <GL/glew.h>
#include <GL/gl.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>

namespace videocomposer {

namespace {
std::atomic<bool> g_requested{false};
std::string g_dir;
bool g_installed = false;
// Render-thread state (capture/endPass run there only).
std::set<std::string> g_done;
unsigned g_round = 0;
bool g_inRound = false;

void onSigusr2(int) {
    g_requested.store(true, std::memory_order_relaxed);
}

void beginRoundIfRequested() {
    if (!g_inRound && g_requested.exchange(false, std::memory_order_relaxed)) {
        g_inRound = true;
        g_done.clear();
        ++g_round;
    }
}
}  // namespace

void OutputSnapshot::installFromEnv() {
    if (g_installed) {
        return;
    }
    g_installed = true;
    const char* dir = std::getenv("VIDEOCOMPOSER_SNAPSHOT_DIR");
    if (!dir || !*dir) {
        return;
    }
    g_dir = dir;
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onSigusr2;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGUSR2, &sa, nullptr);
    LOG_INFO << "OutputSnapshot: enabled -- kill -USR2 <pid> writes one post-blit PPM per output to "
             << g_dir << " (test aid: each round stalls the render loop, ~50-90 ms at 2x4K)";
}

bool OutputSnapshot::enabled() {
    return !g_dir.empty();
}

bool OutputSnapshot::wanted(const std::string& outputName) {
    if (g_dir.empty()) {
        return false;
    }
    beginRoundIfRequested();
    return g_inRound && g_done.count(outputName) == 0;
}

std::vector<uint8_t> OutputSnapshot::encodePpm(const std::vector<uint8_t>& rgba, int width, int height) {
    const std::string header = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    std::vector<uint8_t> out(header.begin(), header.end());
    out.reserve(header.size() + static_cast<size_t>(width) * height * 3);
    for (int y = height - 1; y >= 0; --y) {
        const uint8_t* row = rgba.data() + static_cast<size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            out.push_back(row[x * 4 + 0]);
            out.push_back(row[x * 4 + 1]);
            out.push_back(row[x * 4 + 2]);
        }
    }
    return out;
}

void OutputSnapshot::capture(const std::string& outputName, int width, int height) {
    if (width <= 0 || height <= 0) {
        g_done.insert(outputName);
        return;
    }
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    const std::string path = g_dir + "/" + outputName + "-" + std::to_string(g_round) + ".ppm";
    const auto ppm = encodePpm(rgba, width, height);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(ppm.data()), static_cast<std::streamsize>(ppm.size()));
    if (f) {
        LOG_INFO << "OutputSnapshot: " << outputName << " " << width << "x" << height << " -> " << path;
    } else {
        LOG_WARNING << "OutputSnapshot: could not write " << path;
    }
    g_done.insert(outputName);
}

void OutputSnapshot::endPass(size_t enabledOutputs) {
    if (g_inRound && g_done.size() >= enabledOutputs) {
        g_inRound = false;
    }
}

void OutputSnapshot::requestForTest() {
    g_requested.store(true, std::memory_order_relaxed);
    if (g_dir.empty()) {
        g_dir = "/tmp";
    }
}

} // namespace videocomposer
