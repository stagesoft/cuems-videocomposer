/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (C) 2020-2026 Stage Lab Coop.
 * Author: Ion Reguera <ion@stagelab.coop>
 *
 * cuems-videoindexer - Standalone video frame-index pre-builder
 *
 * Usage:
 *   cuems-videoindexer <video_file> [video_file ...]
 *
 * For each file it writes a binary .idx sidecar into the indexes/ subdirectory
 * next to the video.  If a valid (non-stale) index already exists it is skipped.
 *
 * Exit codes:
 *   0  all files indexed (or already up-to-date)
 *   1  one or more files failed
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
