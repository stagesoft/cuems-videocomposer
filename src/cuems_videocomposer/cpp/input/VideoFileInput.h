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

#ifndef VIDEOCOMPOSER_VIDEOFILEINPUT_H
#define VIDEOCOMPOSER_VIDEOFILEINPUT_H

#include "InputSource.h"
#include "HardwareDecoder.h"
#include "AsyncDecodeQueue.h"
#include "../video/GPUTextureFrameBuffer.h"
#include <cuems_mediadecoder/MediaFileReader.h>
#include <cuems_mediadecoder/VideoDecoder.h>
#include <string>
#include <memory>
#include <cstdint>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <sys/stat.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h>
}

namespace videocomposer {

// Forward declarations
#ifdef HAVE_VAAPI_INTEROP
class VaapiInterop;
class DisplayBackend;
#endif

/**
 * VideoFileInput - FFmpeg-based video file input source
 * 
 * Implements InputSource interface for reading video files using FFmpeg/libav.
 * This is the only input source implementation for now, but the architecture
 * is ready for future implementations (live video, streaming, etc.).
 */
class VideoFileInput : public InputSource {
public:
    VideoFileInput();
    virtual ~VideoFileInput();

    enum class HardwareDecodePreference {
        AUTO,
        SOFTWARE_ONLY,
        VAAPI,
        CUDA
    };

    // InputSource interface
    bool open(const std::string& source) override;
    void close() override;
    bool isReady() const override;
    bool readFrame(int64_t frameNumber, FrameBuffer& buffer) override;
    bool seek(int64_t frameNumber) override;
    void resetSeekState() override;
    FrameInfo getFrameInfo() const override;
    int64_t getCurrentFrame() const override;
    CodecType detectCodec() const override;
    bool supportsDirectGPUTexture() const override;
    DecodeBackend getOptimalBackend() const override;

    // Hardware decoding support
    /**
     * Read a frame directly to GPU texture (for hardware-decoded frames)
     * @param frameNumber Frame number to read
     * @param textureBuffer GPUTextureFrameBuffer to store the decoded texture
     * @return true on success, false on failure
     */
    bool readFrameToTexture(int64_t frameNumber, GPUTextureFrameBuffer& textureBuffer);

    // Additional methods specific to video files
    void setIgnoreStartOffset(bool ignore) { ignoreStartOffset_ = ignore; }
    bool getIgnoreStartOffset() const { return ignoreStartOffset_; }

    void setNoIndex(bool noIndex) { noIndex_ = noIndex; }
    bool getNoIndex() const { return noIndex_; }

    // Index path helpers (used by cuems-videoindexer CLI)
    static std::string getIndexPath(const std::string& videoPath);
    static bool isCacheValid(const std::string& videoPath);

    void setHardwareDecodePreference(HardwareDecodePreference preference) { hwPreference_ = preference; }

    /**
     * Enable/disable seamless loop pre-buffering in the async decode queue.
     * Call this when the cue's loop mode changes (e.g. engine sends /loop 1).
     * @param loop       true when the video is set to loop
     * @param totalFrames total frame count of the loaded video
     */
    void setLoopMode(bool loop, int64_t totalFrames);

#ifdef HAVE_VAAPI_INTEROP
    /**
     * Set DisplayBackend for creating per-instance VAAPI interop
     * @param displayBackend DisplayBackend instance with VAAPI support
     */
    void setDisplayBackend(DisplayBackend* displayBackend);

    /**
     * Check if zero-copy VAAPI decoding is available
     */
    bool hasVaapiZeroCopy() const;
#endif

    /**
     * Index-only mode: open the file to build/refresh its .idx and nothing else.
     *
     * Suppresses creation of the async decode queue, which exists to feed a
     * renderer - the indexer has no layers and no display. The hardware device
     * is still initialized, so indexFrames() keeps its Pass-2 short-circuit and
     * the .idx bytes are unchanged.
     *
     * NOTE with no queue there is no decode path at all, so getOptimalBackend()
     * still answers GPU_HARDWARE while nothing can actually render. That is moot
     * in the indexer, which never asks.
     */
    void setIndexOnly(bool indexOnly) { indexOnly_ = indexOnly; }
    bool getIndexOnly() const { return indexOnly_; }

    // Decode-path health (see InputSource::Health)
    Health getHealth() const override;
    std::string getHealthReason() const override;

private:
    struct FrameIndex {
        int64_t pkt_pts;
        int64_t pkt_pos;
        int64_t frame_pts;
        int64_t frame_pos;
        int64_t timestamp;
        int64_t seekpts;
        int64_t seekpos;
        uint8_t key;
    };

    bool initializeFFmpeg();
    bool openCodec();

    /**
     * Initialize the hardware DEVICE (not a codec).
     *
     * Was openHardwareCodec(): it opened a second, synchronous decoder on the
     * same file, with a VAAPI surface pool of its own, alongside the async
     * queue's. Only the device context survives - it is what the queue decodes
     * against and what the EGL interop shares.
     *
     * @return true when a usable hardware device was created
     */
    bool initializeHardwareDevice();

    /**
     * Release everything initializeHardwareDevice() created and fall back to
     * software. Without this the software tier would run with a live VAAPI
     * device still attached and re-allocate frames on top of its own.
     */
    void teardownHardwareDevice();

    /**
     * Codec parameters straight from the demuxer - valid in BOTH decode modes.
     *
     * Identity and geometry used to be read off codecCtx_, which only ever
     * worked because the hardware path kept a codec context of its own open.
     * That context is gone (F2), so codecCtx_ is null for every hardware layer
     * and codecpar is the only source that answers in both modes.
     *
     * @return codec parameters of the selected video stream, or nullptr when
     *         no file is open / no video stream was selected.
     */
    AVCodecParameters* streamCodecParams() const;
    bool indexFrames();
    bool isIntraFrameCodec() const;  // Check if codec is intra-frame only (all keyframes)
    void setupDirectSeekMode();      // Setup direct seek mode for intra-frame codecs
    bool seekToFrame(int64_t frameNumber);
    bool seekByTimestamp(int64_t frameNumber);
    int64_t parsePTSFromFrame(AVFrame* frame);
    bool transferHardwareFrameToGPU(AVFrame* hwFrame, GPUTextureFrameBuffer& textureBuffer, bool skipSync = false);
    void cleanup();

    // Index caching
    bool loadCachedIndex();
    void saveCachedIndex() const;

    // Media decoder module
    cuems_mediadecoder::MediaFileReader mediaReader_;
    cuems_mediadecoder::VideoDecoder videoDecoder_;
    
    // FFmpeg objects (kept for compatibility and advanced operations)
    AVFormatContext* formatCtx_;  // Access via mediaReader_.getFormatContext()
    AVCodecContext* codecCtx_;   // Access via videoDecoder_.getCodecContext()
    AVFrame* frame_;
    AVFrame* frameFMT_;
    SwsContext* swsCtx_;
    int swsCtxWidth_;
    int swsCtxHeight_;
    AVPixelFormat swsCtxFormat_;   // Track source format for sws context recreation
    int videoStream_;

    // Hardware decoding
    AVBufferRef* hwDeviceCtx_;        // Hardware device context
    HardwareDecoder::Type hwDecoderType_;  // Type of hardware decoder in use
    bool useHardwareDecoding_;        // Whether hardware decoding is enabled
    HardwareDecodePreference hwPreference_;
    bool indexOnly_ = false;          // Index-only mode: no decode queue (see setIndexOnly)
    
#ifdef HAVE_VAAPI_INTEROP
    std::unique_ptr<VaapiInterop> vaapiInterop_;  // VAAPI zero-copy interop (owned per-instance)
    DisplayBackend* displayBackend_;  // DisplayBackend for initializing interop (not owned)
#endif

    // Frame indexing
    FrameIndex* frameIndex_;
    int64_t frameCount_;
    int64_t lastDecodedPTS_;
    int64_t lastDecodedFrameNo_;
    bool scanComplete_;
    bool byteSeek_;
    bool noIndex_;

    // Video properties
    FrameInfo frameInfo_;
    std::string currentFile_;
    bool ignoreStartOffset_;
    int64_t currentFrame_;

    // Internal state
    bool ready_;
    AVRational frameRateQ_;
    
    // Decode-path health, written by open()'s failure ladder (loader thread)
    // and by the recovery worker; read by the render thread and by a future
    // load-time health ping.
    std::atomic<Health> health_{Health::ok};
    mutable std::mutex healthReasonMutex_;
    std::string healthReason_;
    void setHealth(Health health, const std::string& reason);

    // --- decode-queue recovery -------------------------------------------
    //
    // ONE long-lived thread, parked on a condition variable, created at the
    // first successful hardware open. Not a thread per recovery: re-assigning a
    // joinable std::thread member terminates the process on the second one, and
    // spawning from the vsync loop is not free under memory pressure - which is
    // exactly the condition that triggers recovery.
    //
    // queueAccessMutex_ is the gate. The render thread try_locks it around ALL
    // queue access and holds its last texture if it cannot get it; the worker
    // holds it for the whole recovery. Without it the render thread can be
    // borrowing a frame from a queue the worker is destroying.
    std::unique_ptr<std::thread> recoveryThread_;
    std::mutex queueAccessMutex_;
    std::mutex recoveryWakeMutex_;
    std::condition_variable recoveryWakeCond_;
    std::atomic<bool> recoveryWake_{false};
    std::atomic<bool> recoveryStop_{false};
    std::atomic<bool> recoveryActive_{false};
    std::atomic<int64_t> recoveryTargetFrame_{0};

    // Loop state mirrored here so a recovery reopen can re-assert it: the
    // queue's open() deliberately clears all latched playback state.
    std::atomic<bool> loopModeActive_{false};
    std::atomic<int64_t> loopTotalFrames_{0};

    // Surface pool the queue is currently open on. Persists across recoveries:
    // a layer that only survives on a reduced pool must not be handed the full
    // one again on the next fault.
    int queuePoolSize_ = 0;
    size_t queueFillDepth_ = 0;

    void startRecoveryWorker();
    void stopRecoveryWorker();
    void recoveryWorkerFunc();
    void maybeWakeRecovery(int64_t currentFrame);

    // Throttle for the "frame not in queue" warning. Per-instance, NOT static:
    // a static counter is shared by every layer in the process.
    int queueMissCount_ = 0;

    // Hardware decode frame tracking (per-instance, NOT static)
    int64_t lastDecodedHWFrame_;
    
    // NEW: Async decode queue for smooth hardware decoding
    // This provides mpv-style pre-buffering to decouple decode latency from display timing
    std::unique_ptr<AsyncDecodeQueue> asyncDecodeQueue_;
    bool useAsyncDecode_;  // Whether to use async decode (enabled for hardware decode)
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_VIDEOFILEINPUT_H

