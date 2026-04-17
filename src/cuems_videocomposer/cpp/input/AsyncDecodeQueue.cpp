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
 * AsyncDecodeQueue.cpp - Threaded frame decoder with pre-buffering
 * 
 * Implements mpv-style async decoding to decouple decode latency from display timing.
 */

#include "AsyncDecodeQueue.h"
#include "../utils/Logger.h"
#include <chrono>

// #region DEBUG
#include <fstream>
#include <iomanip>
#include <sys/stat.h>
namespace {
void dbg_log_decq(const std::string& msg) {
    try {
        mkdir("/tmp/.claude", 0755);
        auto now = std::chrono::system_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()) % 1000000;
        auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
        localtime_r(&t, &tm_buf);
        std::ofstream f("/tmp/.claude/debug.log", std::ios::app);
        f << "[" << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S")
          << "." << std::setw(6) << std::setfill('0') << us.count()
          << "] [VIDEO-DECQ] [DEBUG H3 H4 H6 H7] " << msg << "\n";
    } catch (...) {}
}
}
// #endregion DEBUG

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
}

namespace videocomposer {

AsyncDecodeQueue::AsyncDecodeQueue()
    : formatCtx_(nullptr)
    , codecCtx_(nullptr)
    , decodeFrame_(nullptr)
    , swsCtx_(nullptr)
    , videoStream_(-1)
    , hwDeviceCtx_(nullptr)
    , useHardware_(false)
    , width_(0)
    , height_(0)
    , framerate_(0)
    , frameCount_(0)
    , ready_(false)
{
    timeBase_ = {1, 1};
    frameRateQ_ = {1, 1};
}

AsyncDecodeQueue::~AsyncDecodeQueue() {
    close();
}

bool AsyncDecodeQueue::open(const std::string& filename, AVBufferRef* hwDeviceCtx) {
    close();  // Close any existing
    
    filename_ = filename;
    hwDeviceCtx_ = hwDeviceCtx;
    
    
    // Open format context
    formatCtx_ = nullptr;
    int ret = avformat_open_input(&formatCtx_, filename.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR << "AsyncDecodeQueue: Failed to open " << filename << ": " << errbuf;
        return false;
    }
    
    // Find stream info
    ret = avformat_find_stream_info(formatCtx_, nullptr);
    if (ret < 0) {
        LOG_ERROR << "AsyncDecodeQueue: Failed to find stream info";
        avformat_close_input(&formatCtx_);
        return false;
    }
    
    // Find video stream
    videoStream_ = -1;
    for (unsigned int i = 0; i < formatCtx_->nb_streams; i++) {
        if (formatCtx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream_ = i;
            break;
        }
    }
    
    if (videoStream_ < 0) {
        LOG_ERROR << "AsyncDecodeQueue: No video stream found";
        avformat_close_input(&formatCtx_);
        return false;
    }
    
    AVStream* stream = formatCtx_->streams[videoStream_];
    AVCodecParameters* codecpar = stream->codecpar;
    
    // Find decoder
    // NOTE: For VAAPI, we use the STANDARD decoder with hw_device_ctx attached
    // This is the "hwaccel" method - FFmpeg uses VAAPI when hw_device_ctx is set
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    
    // Determine if we'll use hardware acceleration
    if (hwDeviceCtx_ && codec) {
        switch (codecpar->codec_id) {
            case AV_CODEC_ID_H264:
            case AV_CODEC_ID_HEVC:
            case AV_CODEC_ID_VP9:
            case AV_CODEC_ID_AV1:
            case AV_CODEC_ID_MPEG2VIDEO:
            case AV_CODEC_ID_VC1:
                useHardware_ = true;
                LOG_INFO << "AsyncDecodeQueue: Using VAAPI hwaccel for " 
                         << avcodec_get_name(codecpar->codec_id);
                break;
            default:
                useHardware_ = false;
                break;
        }
    } else {
        useHardware_ = false;
    }
    
    if (!codec) {
        LOG_ERROR << "AsyncDecodeQueue: No decoder found for codec " << codecpar->codec_id;
        avformat_close_input(&formatCtx_);
        return false;
    }
    
    // Allocate codec context
    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        LOG_ERROR << "AsyncDecodeQueue: Failed to allocate codec context";
        avformat_close_input(&formatCtx_);
        return false;
    }
    
    // Copy codec parameters
    ret = avcodec_parameters_to_context(codecCtx_, codecpar);
    if (ret < 0) {
        LOG_ERROR << "AsyncDecodeQueue: Failed to copy codec parameters";
        avcodec_free_context(&codecCtx_);
        avformat_close_input(&formatCtx_);
        return false;
    }
    
    // Set hardware device context if using hardware decode
    if (useHardware_ && hwDeviceCtx_) {
        codecCtx_->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
        if (!codecCtx_->hw_device_ctx) {
            LOG_WARNING << "AsyncDecodeQueue: Failed to ref hw device context, falling back to software";
            useHardware_ = false;
        }
    }
    
    // Set thread count for software decode
    if (!useHardware_) {
        codecCtx_->thread_count = 4;
        codecCtx_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    } else {
        // Request extra VAAPI surfaces so the queue can hold 8 frames
        // without exhausting the pool (default pool ~17 is too small
        // when sync fallback + EGL import also hold surfaces).
        codecCtx_->extra_hw_frames = 16;
    }
    
    // Open codec
    ret = avcodec_open2(codecCtx_, codec, nullptr);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR << "AsyncDecodeQueue: Failed to open codec: " << errbuf;
        avcodec_free_context(&codecCtx_);
        avformat_close_input(&formatCtx_);
        return false;
    }
    
    // Allocate decode frame
    decodeFrame_ = av_frame_alloc();
    if (!decodeFrame_) {
        LOG_ERROR << "AsyncDecodeQueue: Failed to allocate frame";
        avcodec_free_context(&codecCtx_);
        avformat_close_input(&formatCtx_);
        return false;
    }
    
    // Get video properties
    width_ = codecCtx_->width;
    height_ = codecCtx_->height;
    timeBase_ = stream->time_base;
    
    // Calculate framerate
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
        frameRateQ_ = stream->avg_frame_rate;
        framerate_ = av_q2d(stream->avg_frame_rate);
    } else if (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0) {
        frameRateQ_ = stream->r_frame_rate;
        framerate_ = av_q2d(stream->r_frame_rate);
    } else {
        frameRateQ_ = {25, 1};
        framerate_ = 25.0;
    }
    
    // Calculate frame count
    if (stream->nb_frames > 0) {
        frameCount_ = stream->nb_frames;
    } else if (stream->duration > 0) {
        frameCount_ = static_cast<int64_t>(av_q2d(stream->time_base) * stream->duration * framerate_);
    } else if (formatCtx_->duration > 0) {
        frameCount_ = static_cast<int64_t>((formatCtx_->duration / AV_TIME_BASE) * framerate_);
    } else {
        frameCount_ = 0;
    }
    
    ready_ = true;
    
    LOG_INFO << "AsyncDecodeQueue: Opened " << filename 
             << " (" << width_ << "x" << height_ << " @ " << framerate_ << "fps"
             << ", " << (useHardware_ ? "hardware" : "software") << " decode)";
    
    // Start decode thread
    threadStop_ = false;
    targetFrame_ = 0;
    lastDecodedFrame_ = -1;
    decodeThread_ = std::make_unique<std::thread>(&AsyncDecodeQueue::decodeThreadFunc, this);
    
    return true;
}

void AsyncDecodeQueue::close() {
    // Stop decode thread
    if (decodeThread_) {
        threadStop_ = true;
        queueCond_.notify_all();
        seekCond_.notify_all();
        if (decodeThread_->joinable()) {
            decodeThread_->join();
        }
        decodeThread_.reset();
    }
    
    // Clear queue and borrowed frame
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        frameQueue_.clear();
    }
    if (borrowedFrame_) {
        av_frame_free(&borrowedFrame_);
    }

    // Cleanup FFmpeg
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }
    
    if (decodeFrame_) {
        av_frame_free(&decodeFrame_);
    }
    
    if (codecCtx_) {
        avcodec_free_context(&codecCtx_);
    }
    
    if (formatCtx_) {
        avformat_close_input(&formatCtx_);
    }
    
    ready_ = false;
}

AVFrame* AsyncDecodeQueue::borrowFrame(AVFrame* src) {
    if (!src) return nullptr;
    // #region DEBUG: record when the first post-conversion frame is served to
    // the renderer. Conversion (backward-jump branch in decodeThreadFunc)
    // resets virtualOffset_ to 0, so serving a frame while the transition
    // flag is still set AND virtualOffset_==0 AND firstVirtualInsertNs_ is
    // populated is the signal. Emit a single timing summary and clear the
    // flag so subsequent frames are not re-recorded.
    if (eofTransitionActive_.load() &&
        firstVirtualInsertNs_.load() != 0 &&
        virtualOffset_.load() == 0 &&
        firstVirtualConsumeNs_.load() == 0) {
        int64_t nowNs = std::chrono::steady_clock::now().time_since_epoch().count();
        firstVirtualConsumeNs_.store(nowNs);
        eofTransitionActive_.store(false);
        int64_t drainStart = eofDrainStartNs_.load();
        int64_t drainEnd   = eofDrainEndNs_.load();
        int64_t flushEnd   = eofFlushEndNs_.load();
        int64_t seekEnd    = eofSeekEndNs_.load();
        int64_t firstIns   = firstVirtualInsertNs_.load();
        auto toMs = [](int64_t fromNs, int64_t toNs) -> double {
            if (fromNs == 0 || toNs == 0) return -1.0;
            return static_cast<double>(toNs - fromNs) / 1.0e6;
        };
        dbg_log_decq(std::string("EOF-TRANSITION-TIMING") +
                     " drain_ms="                 + std::to_string(toMs(drainStart, drainEnd)) +
                     " flush_ms="                 + std::to_string(toMs(drainEnd,   flushEnd)) +
                     " seek_ms="                  + std::to_string(toMs(flushEnd,   seekEnd))  +
                     " first_virtual_decode_ms="  + std::to_string(toMs(seekEnd,    firstIns)) +
                     " produce_to_consume_ms="    + std::to_string(toMs(firstIns,   nowNs))    +
                     " total_ms="                 + std::to_string(toMs(drainStart, nowNs)));
    }
    // #endregion DEBUG
    // Release previous borrowed frame
    if (borrowedFrame_) {
        av_frame_unref(borrowedFrame_);
    } else {
        borrowedFrame_ = av_frame_alloc();
        if (!borrowedFrame_) return nullptr;
    }
    // Create a ref-counted copy (increments surface refcount for VAAPI)
    if (av_frame_ref(borrowedFrame_, src) < 0) {
        return nullptr;
    }
    return borrowedFrame_;
}

AVFrame* AsyncDecodeQueue::getFrame(int64_t frameNumber, int maxWaitMs) {
    std::unique_lock<std::mutex> lock(queueMutex_);

    // Update target so decode thread knows what we need
    targetFrame_ = frameNumber;

    // Look for frame in queue
    int64_t tf = totalFrames_.load();
    for (auto& qf : frameQueue_) {
        if (qf.frameNumber == frameNumber && qf.ready) {
            return borrowFrame(qf.frame);
        }
    }

    // Also check for virtual frames (pre-buffered loop start: stored as totalFrames_ + realFrame)
    if (tf > 0) {
        int64_t virtualNum = tf + frameNumber;
        for (auto& qf : frameQueue_) {
            if (qf.frameNumber == virtualNum && qf.ready) {
                return borrowFrame(qf.frame);
            }
        }
    }

    // Frame not ready - wait if requested
    if (maxWaitMs > 0) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxWaitMs);

        while (std::chrono::steady_clock::now() < deadline) {
            // Wake decode thread
            queueCond_.notify_one();

            // Wait for frame
            queueCond_.wait_for(lock, std::chrono::milliseconds(1));

            // Check again (exact match)
            for (auto& qf : frameQueue_) {
                if (qf.frameNumber == frameNumber && qf.ready) {
                    return borrowFrame(qf.frame);
                }
            }
            // Also check virtual match
            if (tf > 0) {
                int64_t virtualNum = tf + frameNumber;
                for (auto& qf : frameQueue_) {
                    if (qf.frameNumber == virtualNum && qf.ready) {
                        return borrowFrame(qf.frame);
                    }
                }
            }
        }
    }

    // Frame not available - return closest earlier frame if available
    AVFrame* closest = nullptr;
    int64_t closestDiff = INT64_MAX;

    for (auto& qf : frameQueue_) {
        if (qf.ready && qf.frameNumber <= frameNumber) {
            int64_t diff = frameNumber - qf.frameNumber;
            if (diff < closestDiff) {
                closestDiff = diff;
                closest = qf.frame;
            }
        }
    }

    return borrowFrame(closest);
}

void AsyncDecodeQueue::seek(int64_t frameNumber) {
    // Set seek request
    seekTarget_ = frameNumber;
    seekRequested_ = true;
    eofReached_ = false;  // Clear EOF state on external seek
    
    // Clear queue
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        frameQueue_.clear();
    }
    
    // Update target
    targetFrame_ = frameNumber;
    lastDecodedFrame_ = -1;
    
    // Wake decode thread
    queueCond_.notify_all();
}

bool AsyncDecodeQueue::hasFrame(int64_t frameNumber) const {
    std::lock_guard<std::mutex> lock(queueMutex_);
    for (const auto& qf : frameQueue_) {
        if (qf.frameNumber == frameNumber && qf.ready) {
            return true;
        }
    }
    return false;
}

void AsyncDecodeQueue::setTargetFrame(int64_t frameNumber) {
    targetFrame_ = frameNumber;
    queueCond_.notify_one();
}

void AsyncDecodeQueue::setLoopMode(bool loop, int64_t totalFrames) {
    loopMode_ = loop;
    totalFrames_ = totalFrames;
    if (loop) {
        eofReached_ = false;  // Entering loop mode: resume decode if at EOF
    } else {
        virtualOffset_ = 0;
    }
}

size_t AsyncDecodeQueue::getQueueSize() const {
    std::lock_guard<std::mutex> lock(queueMutex_);
    return frameQueue_.size();
}

int64_t AsyncDecodeQueue::getOldestFrame() const {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (frameQueue_.empty()) return -1;
    return frameQueue_.front().frameNumber;
}

int64_t AsyncDecodeQueue::getNewestFrame() const {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (frameQueue_.empty()) return -1;
    return frameQueue_.back().frameNumber;
}

void AsyncDecodeQueue::decodeThreadFunc() {
    LOG_INFO << "AsyncDecodeQueue: Decode thread started";
    
    while (!threadStop_) {
        // Check for seek request
        if (seekRequested_) {
            seekRequested_ = false;
            eofReached_ = false;  // New seek clears EOF state
            int64_t seekFrame = seekTarget_.load();
            
            if (!seekInternal(seekFrame)) {
                LOG_WARNING << "AsyncDecodeQueue: Seek to frame " << seekFrame << " failed";
            }
            
            // Flush decoder
            avcodec_flush_buffers(codecCtx_);
            lastDecodedFrame_ = seekFrame - 1;
        }
        
        // Detect backward jump (e.g. cue loop): if the oldest queued frame is more than
        // MAX_QUEUE_SIZE frames ahead of the current target, the target has jumped backward.
        //
        // Special case: when loop pre-buffering is active, the queue contains both real
        // end-of-video frames AND virtual frames (totalFrames_ + realN) representing the
        // start of the next loop. In this case, instead of a full clear+seek, we:
        //   1. Remove the stale real frames
        //   2. Convert virtual frames to their real frame numbers in-place
        //   3. Reset virtualOffset_ so subsequent decoding uses real numbers
        // This makes frame 0 immediately available without a seek.
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            int64_t current = targetFrame_.load();
            int64_t tf = totalFrames_.load();

            if (!frameQueue_.empty()) {
                int64_t oldest = frameQueue_.front().frameNumber;

                if (oldest > current + static_cast<int64_t>(MAX_QUEUE_SIZE)) {
                    // Check if the queue contains virtual pre-buffered frames
                    bool hasVirtual = false;
                    if (tf > 0) {
                        for (const auto& qf : frameQueue_) {
                            if (qf.frameNumber >= tf) { hasVirtual = true; break; }
                        }
                    }

                    if (hasVirtual && tf > 0 && current < static_cast<int64_t>(MAX_QUEUE_SIZE) * 2) {
                        // Loop boundary: target has wrapped to near 0 and we have
                        // pre-buffered virtual frames. Convert them to real in-place
                        // so frame 0 is immediately available without a seek.
                        frameQueue_.erase(
                            std::remove_if(frameQueue_.begin(), frameQueue_.end(),
                                [tf](const QueuedFrame& f) { return f.frameNumber < tf; }),
                            frameQueue_.end()
                        );
                        for (auto& qf : frameQueue_) {
                            if (qf.frameNumber >= tf) {
                                qf.frameNumber -= tf;
                            }
                        }
                        virtualOffset_ = 0;
                        if (!frameQueue_.empty()) {
                            lastDecodedFrame_ = frameQueue_.back().frameNumber;
                        } else {
                            lastDecodedFrame_ = -1;
                        }
                        LOG_INFO << "AsyncDecodeQueue: Loop boundary - converted virtual frames to real, newest="
                                 << lastDecodedFrame_.load();
                    } else {
                        LOG_INFO << "AsyncDecodeQueue: Backward jump detected (oldest=" << oldest
                                 << ", target=" << current << ") - clearing stale frames and seeking";
                        frameQueue_.clear();
                        seekTarget_ = current;
                        seekRequested_ = true;
                        lastDecodedFrame_ = -1;
                        virtualOffset_ = 0;
                        eofReached_ = false;  // Clear EOF so decode resumes after seek
                        continue;  // Restart loop to process seek before decoding
                    }
                }
            } else if (lastDecodedFrame_ >= 0 && current < lastDecodedFrame_ - static_cast<int64_t>(MAX_QUEUE_SIZE)) {
                LOG_INFO << "AsyncDecodeQueue: Backward jump (empty queue) lastDecoded=" << lastDecodedFrame_.load()
                         << " target=" << current << " - seeking";
                seekTarget_ = current;
                seekRequested_ = true;
                lastDecodedFrame_ = -1;
                virtualOffset_ = 0;
                continue;
            }

            // Forward-jump: target is far AHEAD of what we've decoded — the
            // decoder has fallen behind (CPU stall, GPU contention, etc.).
            // Seek forward instead of chewing through the backlog while the
            // renderer is shown increasingly stale frames.
            //
            // The virtualOffset_==0 guard prevents spurious firing during the
            // EOF pre-buffer window, where lastDecodedFrame_ lives in the
            // virtual range (totalFrames + N) and a low target would otherwise
            // look like a forward jump.
            if (lastDecodedFrame_.load() >= 0 &&
                virtualOffset_.load() == 0 &&
                current > lastDecodedFrame_.load() + FORWARD_JUMP_THRESHOLD) {
                dbg_log_decq("FORWARD-JUMP target=" + std::to_string(current) +
                             " lastDecoded=" + std::to_string(lastDecodedFrame_.load()) +
                             " gap=" + std::to_string(current - lastDecodedFrame_.load()));
                LOG_INFO << "AsyncDecodeQueue: Forward jump detected (target=" << current
                         << ", lastDecoded=" << lastDecodedFrame_.load() << ") - clearing and seeking";
                frameQueue_.clear();
                seekTarget_ = current;
                seekRequested_ = true;
                lastDecodedFrame_ = -1;
                eofReached_ = false;
                continue;
            }
        }
        
        // Check if we should decode more
        int64_t target = targetFrame_.load();
        
        // Get queue state
        size_t queueSize;
        int64_t newestInQueue = -1;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            queueSize = frameQueue_.size();
            if (!frameQueue_.empty()) {
                newestInQueue = frameQueue_.back().frameNumber;
            }
        }
        
        // Decide if we should decode
        bool shouldDecode = false;
        if (eofReached_) {
            // Non-loop EOF: hold last frames, don't decode more until a seek clears this
        } else if (queueSize < MAX_QUEUE_SIZE) {
            if (newestInQueue < 0) {
                shouldDecode = true;
            } else if (newestInQueue < target + static_cast<int64_t>(MAX_QUEUE_SIZE)) {
                shouldDecode = true;
            }
        }

        if (shouldDecode && !threadStop_) {
            if (!decodeNextFrame()) {
                // Decode failed or EOF - wait a bit
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueCond_.wait_for(lock, std::chrono::milliseconds(10));
            }
        } else {
            // Nothing to do - wait for signal
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCond_.wait_for(lock, std::chrono::milliseconds(5));
        }
        
        // Trim old frames from queue
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            int64_t current = targetFrame_.load();
            int64_t tf = totalFrames_.load();

            while (!frameQueue_.empty()) {
                int64_t fn = frameQueue_.front().frameNumber;
                if (tf > 0 && fn >= tf) break;
                if (fn < current - 2) {
                    frameQueue_.pop_front();
                } else {
                    break;
                }
            }
        }
    }
    
    LOG_INFO << "AsyncDecodeQueue: Decode thread stopped";
}

bool AsyncDecodeQueue::decodeNextFrame() {
    // #region DEBUG
    static thread_local int dbg_frame_count = 0;
    static thread_local std::chrono::steady_clock::time_point dbg_last = std::chrono::steady_clock::now();
    static thread_local std::string dbg_tag = "?";
    if (dbg_tag == "?") {
        dbg_tag = formatCtx_ && formatCtx_->url ? std::string(formatCtx_->url) : std::string("?");
        size_t pos = dbg_tag.rfind('/');
        if (pos != std::string::npos) dbg_tag = dbg_tag.substr(pos+1);
    }
    auto dbg_now = std::chrono::steady_clock::now();
    auto dbg_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(dbg_now - dbg_last).count();
    if (dbg_elapsed_ms >= 1000) {
        dbg_log_decq("RATE file=" + dbg_tag + " decoded_frames=" + std::to_string(dbg_frame_count) + " elapsed_ms=" + std::to_string(dbg_elapsed_ms) + " effective_fps=" + std::to_string((double)dbg_frame_count * 1000.0 / dbg_elapsed_ms));
        dbg_frame_count = 0;
        dbg_last = dbg_now;
    }
    // #endregion DEBUG
    AVPacket* packet = av_packet_alloc();
    if (!packet) return false;

    bool gotFrame = false;
    int maxPackets = 100;  // Safety limit
    
    while (!gotFrame && maxPackets-- > 0 && !threadStop_) {
        // Try to receive a frame first
        int ret = avcodec_receive_frame(codecCtx_, decodeFrame_);
        if (ret == 0) {
            // Got a frame!
            gotFrame = true;
            break;
        } else if (ret == AVERROR(EAGAIN)) {
            // Need more input
        } else if (ret == AVERROR_EOF) {
            // End of stream
            av_packet_free(&packet);
            return false;
        } else {
            // Error
            av_packet_free(&packet);
            return false;
        }
        
        // Read next packet
        ret = av_read_frame(formatCtx_, packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                // #region DEBUG: mark EOF transition start and reset stage timestamps
                {
                    auto nowNs = std::chrono::steady_clock::now().time_since_epoch().count();
                    eofDrainStartNs_.store(nowNs);
                    eofDrainEndNs_.store(0);
                    eofFlushEndNs_.store(0);
                    eofSeekEndNs_.store(0);
                    firstVirtualInsertNs_.store(0);
                    firstVirtualConsumeNs_.store(0);
                    eofTransitionActive_.store(true);
                }
                // #endregion DEBUG
                // Drain remaining frames from the codec's B-frame reorder buffer.
                // For codecs like H.264/HEVC, the decoder holds the last few frames
                // for reordering. These MUST be queued (not discarded) to avoid
                // skipping frames at the loop boundary, which causes a visible jump.
                avcodec_send_packet(codecCtx_, nullptr);
                int drainCount = 0;
                while (avcodec_receive_frame(codecCtx_, decodeFrame_) == 0) {
                    // Calculate frame number from PTS (same logic as normal path)
                    int64_t drainPts = decodeFrame_->best_effort_timestamp;
                    if (drainPts == AV_NOPTS_VALUE) drainPts = decodeFrame_->pts;

                    int64_t drainFrameNum = 0;
                    if (drainPts != AV_NOPTS_VALUE) {
                        double seconds = drainPts * av_q2d(timeBase_);
                        drainFrameNum = static_cast<int64_t>(seconds * framerate_ + 0.5);
                    } else {
                        int64_t last = lastDecodedFrame_.load();
                        int64_t voff = virtualOffset_.load();
                        int64_t lastReal = (voff > 0 && last >= voff) ? (last - voff) : last;
                        drainFrameNum = lastReal + 1;
                    }

                    // Apply virtual offset (should be 0 at end-of-file)
                    int64_t voff = virtualOffset_.load();
                    if (voff > 0) drainFrameNum += voff;

                    // Sync VAAPI surface if hardware decoding
                    if (useHardware_ && decodeFrame_->format == AV_PIX_FMT_VAAPI) {
                        VASurfaceID surface = (VASurfaceID)(uintptr_t)decodeFrame_->data[3];
                        if (surface != VA_INVALID_SURFACE && hwDeviceCtx_) {
                            AVHWDeviceContext* hwctx = (AVHWDeviceContext*)hwDeviceCtx_->data;
                            AVVAAPIDeviceContext* vactx = (AVVAAPIDeviceContext*)hwctx->hwctx;
                            vaSyncSurface(vactx->display, surface);
                        }
                    }

                    // Queue the drained frame
                    QueuedFrame qf;
                    qf.frameNumber = drainFrameNum;
                    qf.frame = av_frame_alloc();
                    if (qf.frame) {
                        av_frame_move_ref(qf.frame, decodeFrame_);
                        qf.ready = true;

                        {
                            std::lock_guard<std::mutex> lock(queueMutex_);
                            bool inserted = false;
                            for (auto it = frameQueue_.begin(); it != frameQueue_.end(); ++it) {
                                if (it->frameNumber == drainFrameNum) {
                                    if (it->frame) av_frame_free(&(it->frame));
                                    it->frame = qf.frame;
                                    it->ready = true;
                                    qf.frame = nullptr;
                                    inserted = true;
                                    break;
                                } else if (it->frameNumber > drainFrameNum) {
                                    frameQueue_.insert(it, std::move(qf));
                                    inserted = true;
                                    break;
                                }
                            }
                            if (!inserted) {
                                frameQueue_.push_back(std::move(qf));
                            }
                        }

                        lastDecodedFrame_ = drainFrameNum;
                        drainCount++;
                    }
                }
                // #region DEBUG: drain stage complete
                eofDrainEndNs_.store(std::chrono::steady_clock::now().time_since_epoch().count());
                // #endregion DEBUG
                if (drainCount > 0) {
                    LOG_INFO << "AsyncDecodeQueue: Queued " << drainCount
                             << " drained frames from reorder buffer";
                    queueCond_.notify_all();
                }
                avcodec_flush_buffers(codecCtx_);
                // #region DEBUG: flush stage complete
                eofFlushEndNs_.store(std::chrono::steady_clock::now().time_since_epoch().count());
                // #endregion DEBUG

                if (loopMode_ && totalFrames_ > 0) {
                    // #region DEBUG
                    dbg_log_decq("EOF-SEEK total_frames=" + std::to_string(totalFrames_.load()) + " prev_virtual_offset=" + std::to_string(virtualOffset_.load()) + " new_virtual_offset=" + std::to_string(totalFrames_.load()));
                    // #endregion DEBUG
                    // Loop mode: seek to start and pre-buffer frames with virtual numbers
                    if (formatCtx_->pb) formatCtx_->pb->eof_reached = 0;
                    av_seek_frame(formatCtx_, videoStream_, 0, AVSEEK_FLAG_BACKWARD);
                    // #region DEBUG: seek stage complete
                    eofSeekEndNs_.store(std::chrono::steady_clock::now().time_since_epoch().count());
                    // #endregion DEBUG
                    lastDecodedFrame_ = -1;
                    virtualOffset_ = totalFrames_.load();
                    LOG_INFO << "AsyncDecodeQueue: EOF in loop mode, pre-buffering start frames (virtualOffset="
                             << virtualOffset_.load() << ")";
                    continue;  // Re-read packet from seeked position
                } else {
                    // Non-loop mode: stop decoding and hold last frames.
                    eofReached_ = true;
                    // #region DEBUG: no loop boundary will follow, abandon transition tracking
                    eofTransitionActive_.store(false);
                    // #endregion DEBUG
                    LOG_INFO << "AsyncDecodeQueue: EOF reached, stopping decode (non-loop)";
                    av_packet_free(&packet);
                    return false;
                }
            } else {
                av_packet_free(&packet);
                return false;
            }
        }
        
        // Skip non-video packets
        if (packet->stream_index != videoStream_) {
            av_packet_unref(packet);
            continue;
        }
        
        // Send to decoder
        ret = avcodec_send_packet(codecCtx_, packet);
        av_packet_unref(packet);
        
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            av_packet_free(&packet);
            return false;
        }
    }
    
    av_packet_free(&packet);
    
    if (!gotFrame) {
        return false;
    }
    
    // Calculate frame number from PTS
    int64_t pts = decodeFrame_->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) {
        pts = decodeFrame_->pts;
    }
    
    int64_t frameNum = 0;
    if (pts != AV_NOPTS_VALUE) {
        // Convert PTS to frame number
        double seconds = pts * av_q2d(timeBase_);
        frameNum = static_cast<int64_t>(seconds * framerate_ + 0.5);
    } else {
        // No PTS - use sequential numbering based on last real decoded frame
        // (virtualOffset_ is added below, so use the actual lastDecodedFrame_ here)
        int64_t last = lastDecodedFrame_.load();
        int64_t voff = virtualOffset_.load();
        // Subtract virtual offset to get the last real decoded frame number
        int64_t lastReal = (voff > 0 && last >= voff) ? (last - voff) : last;
        frameNum = lastReal + 1;
    }

    // Apply virtual offset for loop pre-buffering
    int64_t voff = virtualOffset_.load();
    if (voff > 0) {
        frameNum += voff;
    }
    
    // For VAAPI hardware frames, sync the GPU before marking frame as ready
    // This ensures the decode is complete before we put it in the queue
    if (useHardware_ && decodeFrame_->format == AV_PIX_FMT_VAAPI) {
        VASurfaceID surface = (VASurfaceID)(uintptr_t)decodeFrame_->data[3];
        if (surface != VA_INVALID_SURFACE && hwDeviceCtx_) {
            AVHWDeviceContext* hwctx = (AVHWDeviceContext*)hwDeviceCtx_->data;
            AVVAAPIDeviceContext* vactx = (AVVAAPIDeviceContext*)hwctx->hwctx;
            VADisplay vaDisplay = vactx->display;
            
            // Sync the surface - blocks until GPU decode is complete
            VAStatus vaStatus = vaSyncSurface(vaDisplay, surface);
            if (vaStatus != VA_STATUS_SUCCESS) {
                LOG_WARNING << "AsyncDecodeQueue: vaSyncSurface failed: " << vaStatus;
            }
        }
    }
    
    // Create queue entry
    QueuedFrame qf;
    qf.frameNumber = frameNum;
    qf.frame = av_frame_alloc();
    if (!qf.frame) {
        return false;
    }
    
    // Move frame data (avoids copy for hardware frames)
    av_frame_move_ref(qf.frame, decodeFrame_);
    qf.ready = true;
    
    // Add to queue (deduplicated, sorted)
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        
        bool inserted = false;
        for (auto it = frameQueue_.begin(); it != frameQueue_.end(); ++it) {
            if (it->frameNumber == frameNum) {
                // Duplicate: replace existing frame with newer decode
                if (it->frame) av_frame_free(&(it->frame));
                it->frame = qf.frame;
                it->ready = qf.ready;
                qf.frame = nullptr;  // Prevent double-free in moved-from QueuedFrame
                inserted = true;
                break;
            } else if (it->frameNumber > frameNum) {
                frameQueue_.insert(it, std::move(qf));
                inserted = true;
                break;
            }
        }
        if (!inserted) {
            frameQueue_.push_back(std::move(qf));
        }
    }
    
    lastDecodedFrame_ = frameNum;

    // #region DEBUG: record when the first virtual (pre-buffered) frame lands
    // in the queue after the EOF-SEEK transition. This is the decode-side end
    // of "seek → first virtual frame produced".
    if (voff > 0 &&
        eofTransitionActive_.load() &&
        firstVirtualInsertNs_.load() == 0) {
        firstVirtualInsertNs_.store(std::chrono::steady_clock::now().time_since_epoch().count());
    }
    // #endregion DEBUG

    // Notify waiting threads
    queueCond_.notify_all();
    // #region DEBUG
    dbg_frame_count++;
    // #endregion DEBUG

    return true;
}

bool AsyncDecodeQueue::seekInternal(int64_t frameNumber) {
    if (!formatCtx_ || videoStream_ < 0) {
        return false;
    }
    
    // Calculate timestamp
    int64_t timestamp = av_rescale_q(frameNumber, av_inv_q(frameRateQ_), timeBase_);
    
    // Seek with BACKWARD flag to ensure we land at or before the target
    int ret = av_seek_frame(formatCtx_, videoStream_, timestamp, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        // Try seeking by byte position or other method
        ret = av_seek_frame(formatCtx_, -1, timestamp * AV_TIME_BASE / av_q2d(timeBase_), AVSEEK_FLAG_BACKWARD);
    }
    
    // Clear AVIO EOF flag — some demuxers (MOV/MP4) don't reset it after seek,
    // causing av_read_frame() to keep returning AVERROR_EOF even though the seek succeeded.
    if (ret >= 0 && formatCtx_->pb) {
        formatCtx_->pb->eof_reached = 0;
    }
    
    return ret >= 0;
}

} // namespace videocomposer

