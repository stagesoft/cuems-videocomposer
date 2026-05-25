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

#ifndef VIDEOCOMPOSER_ASYNCHAPDECODER_H
#define VIDEOCOMPOSER_ASYNCHAPDECODER_H

#include "../hap/HapDecoder.h"
#include "../video/GPUTextureFrameBuffer.h"
#include <cuems_mediadecoder/MediaFileReader.h>
#include <string>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <vector>
#include <cstdint>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace videocomposer {

/**
 * AsyncHapDecoder - HAP analogue of AsyncDecodeQueue.
 *
 * Pre-buffers HAP packets and runs the snappy + DXT unpacking off the render
 * thread. The render thread fetches by frame number and only does the GPU
 * upload (which must stay on the single OpenGL context's thread).
 *
 * Differences vs AsyncDecodeQueue:
 *  - Payload is CPU-side DXT blobs (HapDecodedFrame), not VAAPI surfaces.
 *  - No hwDeviceCtx / vaSyncSurface / EGL import path.
 *  - HAP is intra-only: no B-frame reorder drain, no GOP climb on seek.
 *  - Loop wraparound uses a simple seek-to-0 (HAP keyframes are cheap) rather
 *    than the virtual-frame trick AsyncDecodeQueue needs for h264-VAAPI.
 *  - Owns its own MediaFileReader so the parent HAPVideoInput's reader is
 *    untouched and remains usable for the synchronous FFmpeg fallback.
 */
struct HapDecodedFrame {
    int64_t frameNumber;
    HapVariant variant;
    std::vector<HapDecodedTexture> textures;  // 1 for HAP/HAP_Q/HAP_ALPHA, 2 for HAP_Q_ALPHA
    bool ready;

    HapDecodedFrame() : frameNumber(-1), variant(HapVariant::NONE), ready(false) {}
    // Copyable + movable. Queue insertion uses std::move so copies only happen
    // where intentional (borrowFrame copies into the borrowed-slot for the
    // render thread). If a non-copyable field is added in the future, that's
    // when to revisit; today the explicit deep copy is the desired behaviour.
};

class AsyncHapDecoder {
public:
    AsyncHapDecoder();
    ~AsyncHapDecoder();

    AsyncHapDecoder(const AsyncHapDecoder&) = delete;
    AsyncHapDecoder& operator=(const AsyncHapDecoder&) = delete;

    /**
     * Open file and start the decode worker thread.
     * Derives timeBase_ and framerate_ from the stream — does NOT inherit
     * HAPVideoInput::frameRateQ_ (which is stored inverted by convention).
     */
    bool open(const std::string& filename);

    /** Stop worker and free resources. */
    void close();

    /**
     * Request a frame by number.
     * Returns a borrowed pointer valid until the next getFrame() call on this
     * instance (SPSC: only the render thread is allowed to call). Returns
     * nullptr if not ready within maxWaitMs.
     */
    const HapDecodedFrame* getFrame(int64_t frameNumber, int maxWaitMs = 0);

    /** Flush queue and reposition worker at frameNumber. */
    void seek(int64_t frameNumber);

    bool hasFrame(int64_t frameNumber) const;
    void setTargetFrame(int64_t frameNumber);

    /** Enable loop wraparound: on EOF the worker seeks back to packet 0. */
    void setLoopMode(bool loop);

    bool isReady() const { return ready_; }
    int getWidth() const { return width_; }
    int getHeight() const { return height_; }
    double getFramerate() const { return framerate_; }

    size_t getQueueSize() const;
    int64_t getOldestFrame() const;
    int64_t getNewestFrame() const;

private:
    void decodeThreadFunc();
    bool decodeNextFrame();
    bool seekInternal(int64_t frameNumber);
    void insertFrameSorted(HapDecodedFrame&& qf);
    const HapDecodedFrame* borrowFrame(const HapDecodedFrame& src);

    // Owned demuxer (separate from HAPVideoInput::mediaReader_)
    cuems_mediadecoder::MediaFileReader mediaReader_;
    AVCodecContext* codecCtx_;      // used only for avcodec_flush_buffers on seek
    int videoStream_;
    AVRational timeBase_;
    AVRational frameRateQ_;         // canonical: num=fps_num, den=fps_den

    // Decoder
    HapDecoder hapDecoder_;         // per-worker — not thread-safe for concurrent calls
    int width_;
    int height_;
    double framerate_;
    int64_t frameCount_;
    bool ready_;
    std::string filename_;

    // Queue
    static constexpr size_t MAX_QUEUE_SIZE = 8;
    static constexpr int64_t FORWARD_JUMP_THRESHOLD = 15;
    std::deque<HapDecodedFrame> frameQueue_;
    mutable std::mutex queueMutex_;
    std::condition_variable queueCond_;

    // Worker control
    std::unique_ptr<std::thread> decodeThread_;
    std::atomic<bool> threadStop_{false};
    std::atomic<int64_t> targetFrame_{0};
    std::atomic<int64_t> lastDecodedFrame_{-1};
    std::atomic<bool> seekRequested_{false};
    std::atomic<int64_t> seekTarget_{0};

    std::atomic<bool> loopMode_{false};
    std::atomic<bool> eofReached_{false};

    // Borrow slot: held under queueMutex_ during copy; render-thread-only consumer
    HapDecodedFrame borrowedFrame_;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_ASYNCHAPDECODER_H
