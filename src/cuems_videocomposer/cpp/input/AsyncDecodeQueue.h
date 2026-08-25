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
     * Why the last open() attempt failed.
     *
     * open() returns a bare bool, which left every failure site
     * indistinguishable to the caller - it could not tell "this file is not
     * demuxable" (nothing to fall back to) from "the codec would not open"
     * (software decode is worth a try). The failure ladder in
     * VideoFileInput::open() branches on this.
     *
     * NOTE there is deliberately no ENOMEM/pool-exhaustion value: the VAAPI
     * surface pool is allocated lazily at first decode (ff_get_format), never
     * at avcodec_open2, so pool exhaustion cannot surface here. It appears on
     * the decode thread and is handled by recovery.
     */
    enum class OpenFailure {
        NONE,            // no failure recorded
        DEMUX,           // container would not open / no stream info
        NO_STREAM,       // no video stream in the container
        DECODER_LOOKUP,  // no decoder for this codec id
        CODEC_OPEN,      // avcodec_open2 refused
        INTERNAL         // allocation / parameter-copy failure
    };

    /** Classified reason for the last failed open(). */
    OpenFailure lastOpenFailure() const { return lastOpenFailure_.load(); }

    /**
     * Whether the decode thread is still producing frames.
     *
     * A dead queue used to be indistinguishable from a slow one: the error
     * paths in decodeNextFrame() returned false silently, the thread retried
     * forever, and the renderer just kept showing the last frame. False here
     * means consecutive hard decode errors have passed the threshold and the
     * queue needs reopening.
     */
    bool isHealthy() const { return consecutiveErrors_.load() < DECODE_ERROR_THRESHOLD; }

    /** Consecutive hard decode errors (reset by any successfully decoded frame). */
    int consecutiveErrors() const { return consecutiveErrors_.load(); }

    /** Raw AVERROR behind the most recent hard decode error, or 0. */
    int lastDecodeAVError() const { return lastDecodeAVError_.load(); }

    /**
     * Consecutive hard decode errors tolerated before the queue reports
     * unhealthy.
     *
     * PROVISIONAL. The plan gates this number on an empirical fault-taxonomy
     * session on the FP530 (phase F2 step 0b): which AVERRORs actually reach
     * the decode error paths under pool exhaustion and under stream corruption,
     * and how many a healthy stream produces transiently. Until that runs, 50
     * is a deliberately conservative placeholder - high enough that ordinary
     * stream hiccups cannot trip recovery, low enough that a genuinely dead
     * queue is caught within ~a second at any sane frame rate.
     */
    static constexpr int DECODE_ERROR_THRESHOLD = 50;

    /** Raw AVERROR behind the last failed open(), or 0 if not applicable. */
    int lastOpenAVError() const { return lastOpenAVError_.load(); }

    /** Maximum frames buffered in the queue. */
    static constexpr size_t MAX_QUEUE_SIZE = 8;

    /** Extra VAAPI surfaces requested in normal operation (MAX_QUEUE_SIZE + 3). */
    static constexpr int EXTRA_HW_FRAMES_FULL = 11;

    /**
     * Extra VAAPI surfaces requested by a reduced-pool recovery reopen.
     * Pairs with FILL_DEPTH_REDUCED - a smaller pool MUST come with a smaller
     * fill depth or the decoder starves against its own pool.
     */
    static constexpr int EXTRA_HW_FRAMES_REDUCED = 7;

    /**
     * Fill depth floor for reduced mode.
     *
     * Not a free parameter: the trim window below keeps frames from
     * current - 2 upward, so a queue allowed to fill only 1-3 frames deep
     * locks into a permanent miss - it can never hold the frame the renderer
     * is about to ask for, and burns the full getFrame() wait every vsync.
     * 4 is the smallest depth that clears that window.
     */
    static constexpr size_t FILL_DEPTH_REDUCED = 4;

    /**
     * Open video file and start decode thread
     * @param filename Path to video file
     * @param hwDeviceCtx Hardware device context (for VAAPI, can be nullptr for software)
     * @param extraHwFrames Extra VAAPI surfaces to request (EXTRA_HW_FRAMES_FULL
     *                      normally, EXTRA_HW_FRAMES_REDUCED for a recovery retry)
     * @param fillDepth How many frames the decode thread may buffer ahead
     *                  (MAX_QUEUE_SIZE normally, FILL_DEPTH_REDUCED when the
     *                  pool is reduced). Clamped to [FILL_DEPTH_REDUCED, MAX_QUEUE_SIZE].
     * @return true on success
     */
    bool open(const std::string& filename, AVBufferRef* hwDeviceCtx = nullptr,
              int extraHwFrames = EXTRA_HW_FRAMES_FULL,
              size_t fillDepth = MAX_QUEUE_SIZE);

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
     * Enable/disable seamless loop pre-buffering.
     * When enabled, the decode thread pre-buffers the start of the video
     * using virtual frame numbers (totalFrames + N) so they survive the
     * trim logic until the loop boundary is detected, at which point they
     * are converted to real frame numbers in-place (no seek required).
     * @param loop      true to enable loop pre-buffering
     * @param totalFrames total number of frames in the video (used to
     *                    generate and detect virtual frame numbers)
     */
    void setLoopMode(bool loop, int64_t totalFrames);

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

    // Surfaces requested from the driver at the last open(), and how deep the
    // decode thread may fill. Runtime rather than compile-time so a recovery
    // reopen can come back on a smaller pool without rebuilding.
    int extraHwFrames_ = EXTRA_HW_FRAMES_FULL;
    std::atomic<size_t> fillDepth_{MAX_QUEUE_SIZE};

    // Classified reason for the last failed open() (see OpenFailure).
    std::atomic<OpenFailure> lastOpenFailure_{OpenFailure::NONE};
    std::atomic<int> lastOpenAVError_{0};

    // Decode-thread health. Counted inside decodeNextFrame(), which returns
    // false for non-errors too (EOF, and "no frame after 100 packets"), so the
    // counter cannot live at its call site.
    std::atomic<int> consecutiveErrors_{0};
    std::atomic<int> lastDecodeAVError_{0};

    /** Count one hard decode error and log the first of a run. */
    void recordDecodeError(int averr, const char* where);
    
    // Video properties
    int width_;
    int height_;
    double framerate_;
    int64_t frameCount_;
    bool ready_;
    std::string filename_;
    
    // Frame queue
    // If the renderer's target runs this many frames ahead of the decoder,
    // clear the queue and seek forward instead of chewing through the backlog
    // sequentially while the renderer shows stale frames. Guards against
    // CPU stalls / GPU contention. Does NOT fire at the loop boundary
    // (handled by the backward-jump path — target wraps LOW there).
    static constexpr int64_t FORWARD_JUMP_THRESHOLD = 15;
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
    // Post-seek catch-up goal: set to the requested target whenever the
    // worker processes a seek, cleared once lastDecodedFrame_ catches up.
    // While >=0, the forward-jump check is suppressed so the decoder can
    // walk from the keyframe-≤-target landing point up to the target
    // without re-triggering clear+seek on every iteration.
    // -1 means "not in post-seek catch-up window".
    std::atomic<int64_t> seekGoal_{-1};
    
    // Seamless loop pre-buffering
    // When loopMode_ is true, the decode thread uses virtual frame numbers
    // (totalFrames_ + realFrameN) for frames decoded after EOF so they are
    // not trimmed before the loop boundary conversion happens.
    std::atomic<bool> loopMode_{false};
    std::atomic<int64_t> totalFrames_{0};
    std::atomic<int64_t> virtualOffset_{0};  // Added to decoded frame numbers during pre-buffering
    std::atomic<bool> eofReached_{false};    // Set at EOF in non-loop mode; prevents decode-and-trim churn
    
    // Borrowed frame: ref-counted copy returned by getFrame().
    // Prevents use-after-free when the decode thread clears the queue
    // while the render thread is still using a frame for GPU transfer.
    // Valid until the next getFrame() call (always from the same thread).
    AVFrame* borrowedFrame_ = nullptr;

    // Ref the found frame into borrowedFrame_ and return it.
    // Must be called while queueMutex_ is held.
    AVFrame* borrowFrame(AVFrame* src);

    // Synchronization
    std::condition_variable seekCond_;
    std::mutex seekMutex_;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_ASYNCDECODEQUEUE_H

