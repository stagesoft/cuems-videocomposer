#ifndef VIDEOCOMPOSER_LIVEINPUTSOURCE_H
#define VIDEOCOMPOSER_LIVEINPUTSOURCE_H

#include "InputSource.h"
#include "../video/FrameBuffer.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <cstdint>
#include <chrono>

namespace videocomposer {

/**
 * LiveInputSource - Base class for live input sources (NDI, V4L2, RTSP, etc.)
 * 
 * Provides async frame capture with buffering to prevent frame drops.
 * Live streams cannot seek, so they continuously capture frames in a background thread.
 * 
 * Features:
 * - Circular frame buffer with configurable size
 * - Move semantics for efficient frame handling
 * - Statistics tracking (frames captured, dropped, errors)
 * - Automatic error logging with threshold
 */
class LiveInputSource : public InputSource {
public:
    LiveInputSource();
    virtual ~LiveInputSource();

    // InputSource interface
    bool isLiveStream() const override { return true; }
    bool seek(int64_t frameNumber) override { return false; }  // No seeking for live streams
    bool readFrame(int64_t frameNumber, FrameBuffer& buffer) override;
    bool readLatestFrame(FrameBuffer& buffer) override;

    // Live stream specific - buffer management
    void setBufferSize(int frames);
    int getBufferSize() const { return bufferSize_; }
    bool isBufferEmpty() const;
    int getBufferedFrameCount() const;

    // Statistics
    struct Statistics {
        uint64_t framesCaptured = 0;     // Total frames captured
        uint64_t framesDropped = 0;      // Frames overwritten before read
        uint64_t captureErrors = 0;      // captureFrame() failures
        uint64_t framesDelivered = 0;    // Frames returned via readLatestFrame
        double avgCaptureTimeMs = 0.0;   // Average capture time
        double lastCaptureTimeMs = 0.0;  // Last capture time
    };
    Statistics getStatistics() const;
    void resetStatistics();

protected:
    // Subclasses implement this to capture frames from the source
    virtual bool captureFrame(FrameBuffer& buffer) = 0;

    // Start/stop the capture thread
    void startCaptureThread();
    void stopCaptureThread();

    // Subclass can override to get name for logging
    virtual const char* getSourceTypeName() const { return "LiveInput"; }

private:
    void captureLoop();  // Runs in dedicated thread

    std::thread captureThread_;
    std::atomic<bool> running_;

    // Frame buffer (circular)
    std::vector<FrameBuffer> frameBuffer_;
    int bufferSize_ = 3;  // Default: 3 frames
    std::atomic<int> writeIndex_;
    std::atomic<int> readIndex_;
    std::atomic<int> availableFrames_;  // Frames available for reading
    std::mutex bufferMutex_;
    std::condition_variable frameAvailable_;

    // Statistics (protected by statsMutex_)
    mutable std::mutex statsMutex_;
    Statistics stats_;
    std::chrono::steady_clock::time_point lastStatLogTime_;
    
    // Error tracking
    int consecutiveErrors_ = 0;
    static constexpr int ERROR_LOG_THRESHOLD = 10;  // Log warning every N errors
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_LIVEINPUTSOURCE_H

