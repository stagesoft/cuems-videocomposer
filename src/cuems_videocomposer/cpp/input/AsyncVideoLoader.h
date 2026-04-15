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

#ifndef VIDEOCOMPOSER_ASYNCVIDEOLOADER_H
#define VIDEOCOMPOSER_ASYNCVIDEOLOADER_H

#include "InputSource.h"
#include <memory>
#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <vector>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <set>

namespace videocomposer {

class DisplayBackend;
class ConfigurationManager;

/**
 * AsyncVideoLoader - Background thread for video file loading
 * 
 * Performs heavy file opening operations (format probing, codec init, indexing)
 * in a background thread to avoid blocking the main render loop.
 * 
 * Usage:
 * 1. Call requestLoad() with filepath and callback
 * 2. Call pollCompleted() each frame to process any finished loads
 * 3. Callback is invoked on main thread with the loaded InputSource
 */
class AsyncVideoLoader {
public:
    // Callback for when a video load completes (called on main thread via pollCompleted)
    // Parameters: (cueId, filepath, InputSource, success)
    using LoadCallback = std::function<void(const std::string&, const std::string&, 
                                            std::unique_ptr<InputSource>, bool)>;

    AsyncVideoLoader();
    ~AsyncVideoLoader();

    /**
     * Initialize the loader with required dependencies
     * @param config Configuration manager for hardware decode settings
     * @param displayBackend Display backend for VAAPI interop (can be nullptr)
     */
    void initialize(ConfigurationManager* config, DisplayBackend* displayBackend);

    /**
     * Request async loading of a video file
     * @param cueId Layer cue ID for this load
     * @param filepath Path to video file
     * @param callback Callback to invoke when loading completes
     */
    void requestLoad(const std::string& cueId, const std::string& filepath, LoadCallback callback);

    /**
     * Poll for completed loads and invoke callbacks
     * Call this from the main thread each frame
     * @return Number of callbacks invoked
     */
    int pollCompleted();

    /**
     * Cancel all pending loads for a cue ID
     * @param cueId Cue ID to cancel
     */
    void cancelLoad(const std::string& cueId);

    /**
     * Cancel all pending and queued loads (atomic reset for project load)
     * In-flight loads will complete but their callbacks will find no matching
     * layer and be harmlessly discarded.
     */
    void cancelAll();

    /**
     * Check if a load is pending for a cue ID
     * @param cueId Cue ID to check
     * @return true if load is pending
     */
    bool isLoadPending(const std::string& cueId) const;

    /**
     * Get number of pending loads
     */
    size_t pendingCount() const;

    /**
     * Shutdown the loader
     */
    void shutdown();

private:
    // Request structure
    struct LoadRequest {
        std::string cueId;
        std::string filepath;
        LoadCallback callback;
    };

    // Result structure
    struct LoadResult {
        std::string cueId;
        std::string filepath;
        std::unique_ptr<InputSource> inputSource;
        bool success;
        LoadCallback callback;
    };

    // Worker thread function
    void workerThread();

    // Create input source (runs in worker thread)
    std::unique_ptr<InputSource> createInputSourceAsync(const std::string& filepath);

    // Dependencies
    ConfigurationManager* config_;
    DisplayBackend* displayBackend_;

    // Threading – pool of worker threads (default: 2, so two videos load in parallel)
    std::vector<std::thread> workers_;
    size_t numWorkers_;
    std::atomic<bool> running_;

    // Request queue (protected by mutex)
    mutable std::mutex requestMutex_;
    std::condition_variable requestCond_;
    std::queue<LoadRequest> requestQueue_;

    // Result queue (protected by mutex, consumed by main thread)
    mutable std::mutex resultMutex_;
    std::queue<LoadResult> resultQueue_;

    // Track pending loads by cue ID
    mutable std::mutex pendingMutex_;
    std::set<std::string> pendingCueIds_;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_ASYNCVIDEOLOADER_H

