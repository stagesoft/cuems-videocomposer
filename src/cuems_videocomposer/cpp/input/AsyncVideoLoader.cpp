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

/**
 * AsyncVideoLoader.cpp - Background thread for video file loading
 * 
 * Implements async video loading to avoid blocking the main render loop.
 */

#include "AsyncVideoLoader.h"
#include "VideoFileInput.h"
#include "HAPVideoInput.h"
#include "HardwareDecoder.h"
#include "../utils/Logger.h"
#include "../config/ConfigurationManager.h"
#include "../display/DisplayBackend.h"
#include <algorithm>
#include <cctype>
#include <chrono>

namespace videocomposer {

AsyncVideoLoader::AsyncVideoLoader()
    : config_(nullptr)
    , displayBackend_(nullptr)
    , numWorkers_(2)
    , running_(false)
{
}

AsyncVideoLoader::~AsyncVideoLoader() {
    shutdown();
}

void AsyncVideoLoader::initialize(ConfigurationManager* config, DisplayBackend* displayBackend) {
    config_ = config;
    displayBackend_ = displayBackend;

    // Start worker thread pool (numWorkers_ threads run workerThread() concurrently)
    running_ = true;
    workers_.clear();
    workers_.reserve(numWorkers_);
    for (size_t i = 0; i < numWorkers_; ++i) {
        workers_.emplace_back(&AsyncVideoLoader::workerThread, this);
    }

    LOG_INFO << "AsyncVideoLoader: " << numWorkers_ << " worker thread(s) started";
}

void AsyncVideoLoader::shutdown() {
    if (!running_) {
        return;
    }

    // Signal all threads to stop and wake them up
    running_ = false;
    requestCond_.notify_all();

    // Join all worker threads
    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    workers_.clear();

    // Clear queues
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        std::queue<LoadRequest> empty;
        std::swap(requestQueue_, empty);
    }
    {
        std::lock_guard<std::mutex> lock(resultMutex_);
        std::queue<LoadResult> empty;
        std::swap(resultQueue_, empty);
    }
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingCueIds_.clear();
    }

    LOG_INFO << "AsyncVideoLoader: Shut down";
}

void AsyncVideoLoader::requestLoad(const std::string& cueId, const std::string& filepath, LoadCallback callback) {
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingCueIds_.insert(cueId);
    }

    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        requestQueue_.push({cueId, filepath, callback});
    }
    requestCond_.notify_one();

    LOG_INFO << "AsyncVideoLoader: Queued load request for '" << filepath << "' (cue: " << cueId << ")";
}

int AsyncVideoLoader::pollCompleted() {
    int count = 0;

    while (true) {
        LoadResult result;
        {
            std::lock_guard<std::mutex> lock(resultMutex_);
            if (resultQueue_.empty()) {
                break;
            }
            result = std::move(resultQueue_.front());
            resultQueue_.pop();
        }

        // Remove from pending set
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            pendingCueIds_.erase(result.cueId);
        }

        // Invoke callback on main thread
        if (result.callback) {
            result.callback(result.cueId, result.filepath, 
                          std::move(result.inputSource), result.success);
        }
        count++;
    }

    return count;
}

void AsyncVideoLoader::cancelLoad(const std::string& cueId) {
    // Mark as cancelled in pending set (worker thread will check)
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pendingCueIds_.erase(cueId);
    LOG_INFO << "AsyncVideoLoader: Cancelled load for cue: " << cueId;
}

void AsyncVideoLoader::cancelAll() {
    // Drain request queue
    {
        std::lock_guard<std::mutex> lock(requestMutex_);
        std::queue<LoadRequest> empty;
        requestQueue_.swap(empty);
    }
    // Clear pending set
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingCueIds_.clear();
    }
    // Drain result queue (discard completed but unconsumed results)
    {
        std::lock_guard<std::mutex> lock(resultMutex_);
        std::queue<LoadResult> empty;
        resultQueue_.swap(empty);
    }
    LOG_INFO << "AsyncVideoLoader: Cancelled all pending loads";
}

bool AsyncVideoLoader::isLoadPending(const std::string& cueId) const {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    return pendingCueIds_.count(cueId) > 0;
}

size_t AsyncVideoLoader::pendingCount() const {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    return pendingCueIds_.size();
}

void AsyncVideoLoader::workerThread() {
    LOG_INFO << "AsyncVideoLoader: Worker thread running";

    while (running_) {
        LoadRequest request;
        
        // Wait for a request
        {
            std::unique_lock<std::mutex> lock(requestMutex_);
            requestCond_.wait(lock, [this] {
                return !requestQueue_.empty() || !running_;
            });

            if (!running_) {
                break;
            }

            if (requestQueue_.empty()) {
                continue;
            }

            request = std::move(requestQueue_.front());
            requestQueue_.pop();
        }

        // Check if load was cancelled
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            if (pendingCueIds_.count(request.cueId) == 0) {
                LOG_INFO << "AsyncVideoLoader: Skipping cancelled load for cue: " << request.cueId;
                continue;
            }
        }

        LOG_INFO << "AsyncVideoLoader: Loading '" << request.filepath << "' (cue: " << request.cueId << ")";
        auto startTime = std::chrono::high_resolution_clock::now();

        // Perform the heavy loading work
        auto inputSource = createInputSourceAsync(request.filepath);

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        bool success = (inputSource != nullptr);
        if (success) {
            LOG_INFO << "AsyncVideoLoader: Loaded '" << request.filepath 
                     << "' in " << duration.count() << "ms";
        } else {
            LOG_WARNING << "AsyncVideoLoader: Failed to load '" << request.filepath << "'";
        }

        // Check again if cancelled before posting result
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            if (pendingCueIds_.count(request.cueId) == 0) {
                LOG_INFO << "AsyncVideoLoader: Discarding result for cancelled cue: " << request.cueId;
                continue;
            }
        }

        // Post result to main thread
        {
            std::lock_guard<std::mutex> lock(resultMutex_);
            resultQueue_.push({
                request.cueId,
                request.filepath,
                std::move(inputSource),
                success,
                request.callback
            });
        }
    }

    LOG_INFO << "AsyncVideoLoader: Worker thread exiting";
}

std::unique_ptr<InputSource> AsyncVideoLoader::createInputSourceAsync(const std::string& filepath) {
    // Check for HAP codec (uses custom decoder)
    std::string ext = filepath.substr(filepath.find_last_of('.') + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    
    // For HAP files or files that need HAP decoding
    // We do a quick probe to check codec before full open
    if (ext == "mov" || ext == "mp4") {
        // Quick check for HAP codec
        auto hapInput = std::make_unique<HAPVideoInput>();
        if (hapInput->open(filepath)) {
            LOG_INFO << "AsyncVideoLoader: Using HAP decoder for " << filepath;
            return hapInput;
        }
        // If HAP open fails, file isn't HAP - fall through to regular decoder
    }
    
    // Standard video file - use VideoFileInput with hardware decoding
    bool noIndex = config_ ? config_->getBool("want_noindex", false) : false;
    std::string hwPrefStr = config_ ? config_->getString("hardware_decoder", "auto") : "auto";
    std::transform(hwPrefStr.begin(), hwPrefStr.end(), hwPrefStr.begin(), 
                   [](unsigned char c) { return std::tolower(c); });
    
    VideoFileInput::HardwareDecodePreference hwPref = VideoFileInput::HardwareDecodePreference::AUTO;
    if (hwPrefStr == "software" || hwPrefStr == "cpu") {
        hwPref = VideoFileInput::HardwareDecodePreference::SOFTWARE_ONLY;
    } else if (hwPrefStr == "vaapi") {
        hwPref = VideoFileInput::HardwareDecodePreference::VAAPI;
    } else if (hwPrefStr == "cuda" || hwPrefStr == "nvdec") {
        hwPref = VideoFileInput::HardwareDecodePreference::CUDA;
    }
    
    auto videoInput = std::make_unique<VideoFileInput>();
    videoInput->setNoIndex(noIndex);
    videoInput->setHardwareDecodePreference(hwPref);
    
#ifdef HAVE_VAAPI_INTEROP
    // Set DisplayBackend for per-instance VaapiInterop creation
    if (displayBackend_) {
        videoInput->setDisplayBackend(displayBackend_);
    }
#endif
    
    if (!videoInput->open(filepath)) {
        LOG_ERROR << "AsyncVideoLoader: Failed to open " << filepath;
        return nullptr;
    }
    
    return videoInput;
}

} // namespace videocomposer

