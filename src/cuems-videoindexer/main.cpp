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

#include "../cuems_videocomposer/cpp/input/VideoFileInput.h"
#include <iostream>
#include <string>
#include <chrono>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: cuems-videoindexer <video_file> [video_file ...]\n";
        return 1;
    }

    int errors = 0;

    for (int i = 1; i < argc; i++) {
        const std::string path = argv[i];

        // Fast staleness check without opening the file
        if (videocomposer::VideoFileInput::isCacheValid(path)) {
            std::cout << "Skipping " << path << " (index up to date)\n";
            continue;
        }

        std::cout << "Indexing " << path << " ...\n";
        std::cout.flush();

        auto t0 = std::chrono::steady_clock::now();

        videocomposer::VideoFileInput vfi;
        vfi.setHardwareDecodePreference(
            videocomposer::VideoFileInput::HardwareDecodePreference::SOFTWARE_ONLY);

        if (!vfi.open(path)) {
            std::cerr << "ERROR: Failed to index " << path << "\n";
            errors++;
            continue;
        }

        auto t1  = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        std::string idxPath = videocomposer::VideoFileInput::getIndexPath(path);
        std::cout << "  -> " << idxPath
                  << " (" << vfi.getFrameInfo().totalFrames << " frames"
                  << ", " << ms / 1000.0 << "s)\n";
    }

    return errors > 0 ? 1 : 0;
}
