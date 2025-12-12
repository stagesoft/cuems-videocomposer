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

namespace videocomposer {

AsyncVideoLoader::AsyncVideoLoader()
    : config_(nullptr)
    , displayBackend_(nullptr)
    , running_(false)
{
}

AsyncVideoLoader::~AsyncVideoLoader() {
    shutdown();
}

void AsyncVideoLoader::initialize(ConfigurationManager* config, DisplayBackend* displayBackend) {
    config_ = config;
    displayBackend_ = displayBackend;

    // Start worker thread
    running_ = true;
    workerThread_ = std::make_unique<std::thread>(&AsyncVideoLoader::workerThread, this);
    
    LOG_INFO << "AsyncVideoLoader: Worker thread started";
}

void AsyncVideoLoader::shutdown() {
    if (!running_) {
        return;
    }

    // Signal thread to stop
    running_ = false;
    requestCond_.notify_all();

    // Wait for thread to finish
    if (workerThread_ && workerThread_->joinable()) {
        workerThread_->join();
    }
    workerThread_.reset();

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

