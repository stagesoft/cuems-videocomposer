#ifndef VIDEOCOMPOSER_ASYNCDECODEQUEUE_H
#define VIDEOCOMPOSER_ASYNCDECODEQUEUE_H

#include "../video/FrameBuffer.h"
#include "../video/GPUTextureFrameBuffer.h"
#include <string>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <functional>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h>
}

namespace videocomposer {

// Forward declarations
class DisplayBackend;
class VaapiInterop;

/**
 * AsyncDecodeQueue - Threaded frame decoder with pre-buffering
 * 
 * This class implements mpv-style async decoding:
 * - Decode thread runs independently, filling a frame queue
 * - Main thread requests frames by number, gets them from queue instantly
 * - Pre-buffers ahead of current playback position
 * - Handles seeking by flushing queue and restarting
 * 
 * For VAAPI hardware decode:
 * - Decode thread creates AVFrames with VAAPI surfaces
 * - Main thread does vaSyncSurface + EGL import (fast)
 * - This decouples slow GPU decode from display timing
 */
class AsyncDecodeQueue {
public:
    AsyncDecodeQueue();
    ~AsyncDecodeQueue();

    /**
     * Open video file and start decode thread
     * @param filename Path to video file
     * @param hwDeviceCtx Hardware device context (for VAAPI, can be nullptr for software)
     * @return true on success
     */
    bool open(const std::string& filename, AVBufferRef* hwDeviceCtx = nullptr);

    /**
     * Close and stop decode thread
     */
    void close();

    /**
     * Request a frame by number
     * If frame is in queue, returns immediately.
     * If not, waits briefly then returns nullptr (caller should use previous frame).
     * @param frameNumber Requested frame number
     * @param maxWaitMs Maximum time to wait if frame not ready (0 = no wait)
     * @return AVFrame pointer (caller must NOT free) or nullptr
     */
    AVFrame* getFrame(int64_t frameNumber, int maxWaitMs = 0);

    /**
     * Seek to a new position (flushes queue)
     * @param frameNumber Target frame number
     */
    void seek(int64_t frameNumber);

    /**
     * Check if a frame is ready in the queue
     */
    bool hasFrame(int64_t frameNumber) const;

    /**
     * Get video properties
     */
    int getWidth() const { return width_; }
    int getHeight() const { return height_; }
    double getFramerate() const { return framerate_; }
    int64_t getFrameCount() const { return frameCount_; }
    bool isHardwareDecoding() const { return useHardware_; }
    bool isReady() const { return ready_; }

    /**
     * Set the target frame for pre-buffering
     * Decode thread will buffer frames starting from this position
     */
    void setTargetFrame(int64_t frameNumber);

    /**
     * Get queue statistics for debugging
     */
    size_t getQueueSize() const;
    int64_t getOldestFrame() const;
    int64_t getNewestFrame() const;

private:
    // Decoded frame in queue
    struct QueuedFrame {
        int64_t frameNumber;
        AVFrame* frame;  // Owned by this struct
        bool ready;
        
        QueuedFrame() : frameNumber(-1), frame(nullptr), ready(false) {}
        ~QueuedFrame() {
            if (frame) {
                av_frame_free(&frame);
            }
        }
        
        // Move-only
        QueuedFrame(QueuedFrame&& other) noexcept 
            : frameNumber(other.frameNumber), frame(other.frame), ready(other.ready) {
            other.frame = nullptr;
        }
        QueuedFrame& operator=(QueuedFrame&& other) noexcept {
            if (this != &other) {
                if (frame) av_frame_free(&frame);
                frameNumber = other.frameNumber;
                frame = other.frame;
                ready = other.ready;
                other.frame = nullptr;
            }
            return *this;
        }
        
        // No copy
        QueuedFrame(const QueuedFrame&) = delete;
        QueuedFrame& operator=(const QueuedFrame&) = delete;
    };

    // Decode thread function
    void decodeThreadFunc();
    
    // Internal decode (called from thread)
    bool decodeNextFrame();
    bool seekInternal(int64_t frameNumber);
    
    // FFmpeg objects (owned by decode thread)
    AVFormatContext* formatCtx_;
    AVCodecContext* codecCtx_;
    AVFrame* decodeFrame_;
    SwsContext* swsCtx_;
    int videoStream_;
    AVRational timeBase_;
    AVRational frameRateQ_;
    
    // Hardware decoding
    AVBufferRef* hwDeviceCtx_;  // Not owned, shared from VideoFileInput
    bool useHardware_;
    
    // Video properties
    int width_;
    int height_;
    double framerate_;
    int64_t frameCount_;
    bool ready_;
    std::string filename_;
    
    // Frame queue
    static constexpr size_t MAX_QUEUE_SIZE = 8;  // Buffer up to 8 frames
    std::deque<QueuedFrame> frameQueue_;
    mutable std::mutex queueMutex_;
    std::condition_variable queueCond_;
    
    // Decode thread control
    std::unique_ptr<std::thread> decodeThread_;
    std::atomic<bool> threadStop_{false};
    std::atomic<int64_t> targetFrame_{0};
    std::atomic<int64_t> lastDecodedFrame_{-1};
    std::atomic<bool> seekRequested_{false};
    std::atomic<int64_t> seekTarget_{0};
    
    // Synchronization
    std::condition_variable seekCond_;
    std::mutex seekMutex_;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_ASYNCDECODEQUEUE_H

