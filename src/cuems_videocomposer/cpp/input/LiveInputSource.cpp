#include "LiveInputSource.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <utility>

namespace videocomposer {

LiveInputSource::LiveInputSource()
    : running_(false)
    , bufferSize_(3)
    , writeIndex_(0)
    , readIndex_(0)
    , availableFrames_(0)
    , consecutiveErrors_(0)
{
    frameBuffer_.resize(bufferSize_);
    lastStatLogTime_ = std::chrono::steady_clock::now();
}

LiveInputSource::~LiveInputSource() {
    stopCaptureThread();
}

void LiveInputSource::setBufferSize(int frames) {
    if (frames < 1) frames = 1;
    if (frames > 30) frames = 30;  // Reasonable limit
    
    if (running_) {
        LOG_WARNING << getSourceTypeName() << ": Cannot change buffer size while running";
        return;
    }
    
    bufferSize_ = frames;
    frameBuffer_.resize(bufferSize_);
    LOG_INFO << getSourceTypeName() << ": Buffer size set to " << bufferSize_ << " frames";
}

void LiveInputSource::startCaptureThread() {
    if (running_) {
        return;  // Already running
    }

    // Reset state
    writeIndex_ = 0;
    readIndex_ = 0;
    availableFrames_ = 0;
    consecutiveErrors_ = 0;
    resetStatistics();
    frameBuffer_.resize(bufferSize_);

    running_ = true;
    captureThread_ = std::thread(&LiveInputSource::captureLoop, this);
    LOG_INFO << getSourceTypeName() << ": Capture thread started (buffer: " << bufferSize_ << " frames)";
}

void LiveInputSource::stopCaptureThread() {
    if (!running_) {
        return;
    }

    running_ = false;
    frameAvailable_.notify_all();  // Wake up thread if waiting

    if (captureThread_.joinable()) {
        captureThread_.join();
    }
    
    // Log final statistics
    Statistics stats = getStatistics();
    LOG_INFO << getSourceTypeName() << ": Capture thread stopped. Stats: "
             << stats.framesCaptured << " captured, "
             << stats.framesDelivered << " delivered, "
             << stats.framesDropped << " dropped, "
             << stats.captureErrors << " errors";
}

void LiveInputSource::captureLoop() {
    FrameBuffer tempBuffer;

    while (running_) {
        auto captureStart = std::chrono::steady_clock::now();
        
        // Capture frame from source (implemented by subclass)
        if (captureFrame(tempBuffer)) {
            auto captureEnd = std::chrono::steady_clock::now();
            double captureTimeMs = std::chrono::duration<double, std::milli>(captureEnd - captureStart).count();
            
            // Reset error count on success
            consecutiveErrors_ = 0;
            
            // Add to circular buffer using move semantics
            {
                std::lock_guard<std::mutex> lock(bufferMutex_);
                int idx = writeIndex_.load() % bufferSize_;
                
                // Check if we're overwriting an unread frame
                if (availableFrames_.load() >= bufferSize_) {
                    std::lock_guard<std::mutex> statsLock(statsMutex_);
                    stats_.framesDropped++;
                } else {
                    availableFrames_++;
                }
                
                // Move frame into buffer (avoids copy)
                frameBuffer_[idx].swap(tempBuffer);
                writeIndex_++;
                
                // Update statistics
                {
                    std::lock_guard<std::mutex> statsLock(statsMutex_);
                    stats_.framesCaptured++;
                    stats_.lastCaptureTimeMs = captureTimeMs;
                    // Running average
                    if (stats_.framesCaptured == 1) {
                        stats_.avgCaptureTimeMs = captureTimeMs;
                    } else {
                        stats_.avgCaptureTimeMs = stats_.avgCaptureTimeMs * 0.95 + captureTimeMs * 0.05;
                    }
                }
            }
            frameAvailable_.notify_one();
            
            // Periodic statistics logging (every 30 seconds)
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastStatLogTime_).count() >= 30) {
                Statistics stats = getStatistics();
                LOG_INFO << getSourceTypeName() << ": "
                         << stats.framesCaptured << " frames, "
                         << stats.framesDropped << " dropped, "
                         << "avg capture: " << stats.avgCaptureTimeMs << "ms";
                lastStatLogTime_ = now;
            }
        } else {
            // Capture failed
            consecutiveErrors_++;
            
            {
                std::lock_guard<std::mutex> statsLock(statsMutex_);
                stats_.captureErrors++;
            }
            
            // Log warning periodically, not every error
            if (consecutiveErrors_ == ERROR_LOG_THRESHOLD) {
                LOG_WARNING << getSourceTypeName() << ": " << consecutiveErrors_ 
                           << " consecutive capture errors";
            } else if (consecutiveErrors_ > 0 && consecutiveErrors_ % (ERROR_LOG_THRESHOLD * 10) == 0) {
                LOG_WARNING << getSourceTypeName() << ": " << consecutiveErrors_ 
                           << " consecutive capture errors (total: " << stats_.captureErrors << ")";
            }
            
            // Small delay to avoid busy-waiting on continuous failure
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

bool LiveInputSource::readLatestFrame(FrameBuffer& buffer) {
    std::unique_lock<std::mutex> lock(bufferMutex_);
    
    // Wait for frame with timeout
    if (availableFrames_.load() == 0) {
        // Wait up to 100ms for a frame
        bool gotFrame = frameAvailable_.wait_for(lock, std::chrono::milliseconds(100),
            [this] { return availableFrames_.load() > 0 || !running_; });
        
        if (!gotFrame || availableFrames_.load() == 0) {
            return false;  // Timeout or stopped
        }
    }

    // Get latest frame (most recent write)
    int idx = (writeIndex_.load() - 1) % bufferSize_;
    if (idx < 0) idx += bufferSize_;  // Handle wrap
    
    // Copy the frame (we keep buffer intact for possible re-reads)
    buffer = frameBuffer_[idx];
    
    // Mark all frames as consumed (we only care about latest)
    availableFrames_ = 0;
    readIndex_ = writeIndex_.load();
    
    {
        std::lock_guard<std::mutex> statsLock(statsMutex_);
        stats_.framesDelivered++;
    }
    
    return true;
}

bool LiveInputSource::readFrame(int64_t frameNumber, FrameBuffer& buffer) {
    // For live streams, ignore frameNumber and return latest frame
    (void)frameNumber;  // Unused parameter
    return readLatestFrame(buffer);
}

bool LiveInputSource::isBufferEmpty() const {
    return availableFrames_.load() == 0;
}

int LiveInputSource::getBufferedFrameCount() const {
    return availableFrames_.load();
}

LiveInputSource::Statistics LiveInputSource::getStatistics() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void LiveInputSource::resetStatistics() {
    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_ = Statistics{};
    lastStatLogTime_ = std::chrono::steady_clock::now();
}

} // namespace videocomposer
