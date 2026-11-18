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
#include "../guard/SaturationSignals.h"
#include "../utils/ExitReporter.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <sys/stat.h>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>      // av_get_pix_fmt_name() - defect 6(b) reporting
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

bool AsyncDecodeQueue::open(const std::string& filename, AVBufferRef* hwDeviceCtx,
                            int extraHwFrames, size_t fillDepth) {
    close();  // Close any existing
    
    filename_ = filename;
    hwDeviceCtx_ = hwDeviceCtx;
    extraHwFrames_ = extraHwFrames;

    // Clamp: a fill depth below the trim window is a permanent miss, above the
    // queue capacity is meaningless.
    if (fillDepth < FILL_DEPTH_REDUCED) fillDepth = FILL_DEPTH_REDUCED;
    if (fillDepth > MAX_QUEUE_SIZE)     fillDepth = MAX_QUEUE_SIZE;
    fillDepth_ = fillDepth;

    lastOpenFailure_ = OpenFailure::NONE;
    lastOpenAVError_ = 0;
    consecutiveErrors_ = 0;
    lastDecodeAVError_ = 0;
    // Recovery reads this as "good frames since the reopen that ended the last
    // episode", which is only true because it is reset here. See framesDecoded().
    framesDecoded_ = 0;

    // Reset latched playback state. close() clears the queue and the thread but
    // NOT these: reopening on a recovery attempt with eofReached_ or a virtual
    // loop offset still set would leave the fresh queue refusing to decode, or
    // numbering frames into a window the renderer never asks for.
    // Loop mode is reset too - the caller re-asserts it after a successful open.
    // Defect 6(b): re-arm the first-frame format latch. A reopen can land on a
    // different decode path than the open before it, so a stale "we already
    // looked" would report the previous attempt's answer forever.
    softDespiteHwClaim_ = false;
    firstDecodedPixFmt_ = AV_PIX_FMT_NONE;

    // The decode-thread export's warm-up record belongs to one decoder
    // (1amfwm7gq3x): a reopen builds a new pool.
    warmInode_.clear();
    warmSurfaces_ = 0;
    warmFailed_ = 0;
    loggedNoWarmRecord_ = false;
    loggedExportFailed_ = false;
    loggedInodeChanged_ = false;

    eofReached_ = false;
    virtualOffset_ = 0;
    seekGoal_ = -1;
    seekRequested_ = false;
    seekTarget_ = 0;
    loopMode_ = false;
    totalFrames_ = 0;

    // Open format context
    formatCtx_ = nullptr;
    int ret = avformat_open_input(&formatCtx_, filename.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR << "AsyncDecodeQueue: Failed to open " << filename << ": " << errbuf;
        lastOpenFailure_ = OpenFailure::DEMUX;
        lastOpenAVError_ = ret;
        return false;
    }
    
    // Find stream info
    ret = avformat_find_stream_info(formatCtx_, nullptr);
    if (ret < 0) {
        LOG_ERROR << "AsyncDecodeQueue: Failed to find stream info";
        avformat_close_input(&formatCtx_);
        lastOpenFailure_ = OpenFailure::DEMUX;
        lastOpenAVError_ = ret;
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
        lastOpenFailure_ = OpenFailure::NO_STREAM;
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
        lastOpenFailure_ = OpenFailure::DECODER_LOOKUP;
        return false;
    }
    
    // Allocate codec context
    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        LOG_ERROR << "AsyncDecodeQueue: Failed to allocate codec context";
        avformat_close_input(&formatCtx_);
        lastOpenFailure_ = OpenFailure::INTERNAL;
        return false;
    }
    
    // Copy codec parameters
    ret = avcodec_parameters_to_context(codecCtx_, codecpar);
    if (ret < 0) {
        LOG_ERROR << "AsyncDecodeQueue: Failed to copy codec parameters";
        avcodec_free_context(&codecCtx_);
        avformat_close_input(&formatCtx_);
        lastOpenFailure_ = OpenFailure::INTERNAL;
        lastOpenAVError_ = ret;
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
        // Request extra VAAPI surfaces for the frames this queue pins outside
        // the decoder's own DPB: MAX_QUEUE_SIZE in the queue, plus the borrowed
        // frame, the frame being transferred, and the one the interop still
        // holds for the previous vsync - MAX_QUEUE_SIZE + 3.
        //
        // This was 16, sized when a second (synchronous) decoder held surfaces
        // of its own on the same file. That decoder is gone, and the surplus is
        // not free: it is VRAM reserved per layer against a carve-out that the
        // eviction-storm hang is measured against.
        codecCtx_->extra_hw_frames = extraHwFrames_;

        // 1amfwm7gq3x: with the decode-thread export on, the pool is built
        // in get_format so every surface can be exported once before any
        // decode targets it (buildWarmPool).
        if (decodeThreadExport_) {
            codecCtx_->opaque = this;
            codecCtx_->get_format = &AsyncDecodeQueue::getFormatCallback;
        }
    }
    
    // Open codec
    ret = avcodec_open2(codecCtx_, codec, nullptr);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR << "AsyncDecodeQueue: Failed to open codec: " << errbuf;
        avcodec_free_context(&codecCtx_);
        avformat_close_input(&formatCtx_);
        lastOpenFailure_ = OpenFailure::CODEC_OPEN;
        lastOpenAVError_ = ret;
        return false;
    }
    
    // Allocate decode frame
    decodeFrame_ = av_frame_alloc();
    if (!decodeFrame_) {
        LOG_ERROR << "AsyncDecodeQueue: Failed to allocate frame";
        avcodec_free_context(&codecCtx_);
        avformat_close_input(&formatCtx_);
        lastOpenFailure_ = OpenFailure::INTERNAL;
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

    // Join the live-decoder census (F1). Paired with close() through ready_,
    // which is the only thing that distinguishes "open() got this far" from a
    // failed open that still calls close() on the way out.
    if (useHardware_) {
        exitreport::decoderOpened(extraHwFrames_);
    }

    LOG_INFO << "AsyncDecodeQueue: Opened " << filename 
             << " (" << width_ << "x" << height_ << " @ " << framerate_ << "fps"
             << ", " << (useHardware_ ? "hardware" : "software") << " decode"
             << (useHardware_ ? ", pool +" + std::to_string(extraHwFrames_) +
                                ", fill depth " + std::to_string(fillDepth_.load())
                              : std::string())
             << ")";
    
    // Start decode thread
    threadStop_ = false;
    targetFrame_ = 0;
    lastDecodedFrame_ = -1;
    decodeThread_ = std::make_unique<std::thread>(&AsyncDecodeQueue::decodeThreadFunc, this);
    
    return true;
}

void AsyncDecodeQueue::close() {
    if (ready_ && useHardware_) {
        exitreport::decoderClosed(extraHwFrames_);
    }

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
    warmInode_.clear();   // the decode thread, its only other user, is joined
    if (borrowedFrame_) {
        av_frame_free(&borrowedFrame_);
    }
    borrowedExported_.reset();

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

AVFrame* AsyncDecodeQueue::borrowFrame(const QueuedFrame* src) {
    // The only setter of the borrowed pair (1amfwm7gq3x): the frame and its
    // export are taken or dropped together, so the caller can never see one
    // frame with another frame's export. A miss drops both; the previous
    // frame's pointer was only valid until this call anyway.
    borrowedExported_.reset();
    if (borrowedFrame_) {
        av_frame_unref(borrowedFrame_);
    }
    if (!src || !src->frame) return nullptr;
    if (!borrowedFrame_) {
        borrowedFrame_ = av_frame_alloc();
        if (!borrowedFrame_) return nullptr;
    }
    // Create a ref-counted copy (increments surface refcount for VAAPI)
    if (av_frame_ref(borrowedFrame_, src->frame) < 0) {
        return nullptr;
    }
    borrowedExported_ = src->exported;
    return borrowedFrame_;
}

AVFrame* AsyncDecodeQueue::getFrame(int64_t frameNumber, int64_t& returnedFrame, int maxWaitMs) {
    std::unique_lock<std::mutex> lock(queueMutex_);
    returnedFrame = -1;

    // Update target so decode thread knows what we need
    targetFrame_ = frameNumber;

    // Look for frame in queue
    int64_t tf = totalFrames_.load();
    for (auto& qf : frameQueue_) {
        if (qf.frameNumber == frameNumber && qf.ready) {
            AVFrame* hit = borrowFrame(&qf);
            returnedFrame = hit ? frameNumber : -1;
            return hit;
        }
    }

    // Also check for virtual frames (pre-buffered loop start: stored as totalFrames_ + realFrame)
    if (tf > 0) {
        int64_t virtualNum = tf + frameNumber;
        for (auto& qf : frameQueue_) {
            if (qf.frameNumber == virtualNum && qf.ready) {
                AVFrame* hit = borrowFrame(&qf);
                returnedFrame = hit ? frameNumber : -1;
                return hit;
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
                    AVFrame* hit = borrowFrame(&qf);
                    returnedFrame = hit ? frameNumber : -1;
                    return hit;
                }
            }
            // Also check virtual match
            if (tf > 0) {
                int64_t virtualNum = tf + frameNumber;
                for (auto& qf : frameQueue_) {
                    if (qf.frameNumber == virtualNum && qf.ready) {
                        AVFrame* hit = borrowFrame(&qf);
                        returnedFrame = hit ? frameNumber : -1;
                        return hit;
                    }
                }
            }
        }
    }

    // Frame not available - return closest earlier frame if available
    const QueuedFrame* closest = nullptr;
    int64_t closestNumber = -1;
    int64_t closestDiff = INT64_MAX;

    for (auto& qf : frameQueue_) {
        if (qf.ready && qf.frameNumber <= frameNumber) {
            int64_t diff = frameNumber - qf.frameNumber;
            if (diff < closestDiff) {
                closestDiff = diff;
                closest = &qf;
                closestNumber = qf.frameNumber;
            }
        }
    }

    // Report what is returned, not what was asked for (1amfwm7hk6j): a virtual
    // (loop pre-buffer) entry is totalFrames_ + its real frame number.
    AVFrame* borrowed = borrowFrame(closest);
    if (borrowed) {
        returnedFrame = (tf > 0 && closestNumber >= tf) ? closestNumber - tf : closestNumber;
    }
    return borrowed;
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
            // Arm post-seek catch-up: seekInternal positions the demuxer at
            // the keyframe K <= seekFrame, so the first frame produced by
            // decodeNextFrame() will be K, not seekFrame. The decoder then
            // walks K, K+1, ... up to seekFrame across subsequent worker
            // iterations. seekGoal_ suppresses the forward-jump check
            // during that climb (see the check below).
            seekGoal_ = seekFrame;
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
            //
            // The seekGoal_ guard suppresses re-firing while the decoder is
            // climbing from a just-landed keyframe up to a previous seek's
            // target. Without it, a seek to T whose nearest preceding
            // keyframe K satisfies T - K > FORWARD_JUMP_THRESHOLD would
            // trap the queue in a clear+seek loop: every iteration decodes
            // frame K, the check fires (T - K > threshold), clears, seeks
            // back to T, lands at K again. Common at GOP sizes >= 16 (e.g.
            // long-GOP H.264 with 30-frame keyframe interval).
            int64_t goal = seekGoal_.load();
            if (goal >= 0 && lastDecodedFrame_.load() >= goal) {
                // Caught up to the post-seek goal — re-arm the check.
                seekGoal_ = -1;
                goal = -1;
            }
            bool inPostSeekCatchUp = (goal >= 0);

            if (lastDecodedFrame_.load() >= 0 &&
                virtualOffset_.load() == 0 &&
                !inPostSeekCatchUp &&
                current > lastDecodedFrame_.load() + FORWARD_JUMP_THRESHOLD) {
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
        } else if (queueSize < fillDepth_.load()) {
            // Fill gate only. The jump thresholds, the loop-boundary
            // discriminant and the trim window below keep the compile-time
            // MAX_QUEUE_SIZE: they describe queue geometry, not how deep this
            // open() is allowed to buffer.
            if (newestInQueue < 0) {
                shouldDecode = true;
            } else if (newestInQueue < target + static_cast<int64_t>(fillDepth_.load())) {
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

void AsyncDecodeQueue::noteDecodedFormat(const AVFrame* frame) {
    // Decode thread only, first decoded frame of each open().
    //
    // useHardware_ is decided inside open(), from "a VAAPI device attached and
    // the codec is whitelisted" - a statement of intent. This is the first
    // moment the truth exists: a frame came back, and its format says which
    // path produced it. The disagreement case used to be silently ignored (the
    // two VAAPI sync sites just took their else branch), which is defect 6(b):
    // the layer reports Health::ok and hardware while the CPU does the work,
    // and the only thing that disagrees is the GPU's decode engine at zero.
    if (!frame || firstDecodedPixFmt_.load() != AV_PIX_FMT_NONE) {
        return;
    }
    firstDecodedPixFmt_.store(frame->format);

    if (useHardware_ && frame->format != AV_PIX_FMT_VAAPI) {
        softDespiteHwClaim_.store(true);
        const char* fmt = av_get_pix_fmt_name((AVPixelFormat)frame->format);
        // ERROR, not WARNING, and deliberately: this is the silent-wrong-answer
        // class the whole 869en65tm campaign is about, and it is the only
        // observable there is until the F9 health ping exists.
        LOG_ERROR << "AsyncDecodeQueue: opened with hardware acceleration for "
                  << filename_ << " but frames are decoding in SOFTWARE ("
                  << (fmt ? fmt : "unknown pixel format")
                  << ") - this layer is on the CPU; the GPU decode engine will "
                     "show idle for it";
    }
}


void AsyncDecodeQueue::recordDecodeError(int averr, const char* where) {
    lastDecodeAVError_ = averr;
    // F1's live-decoder census. This is the only production caller: the unit
    // suite drives decodeErrorObserved() directly, so a death record's
    // decode_errors field is trustworthy only while this line is here.
    exitreport::decodeErrorObserved(averr);

    // The same event, but keyed by queue so the saturation monitor can tell
    // one sick file from a platform-wide event. Errors on a single layer are
    // that layer's own recovery story; two or more layers erroring within the
    // same window is a property of the GPU, and that is what raises an alarm.
    // `this` is only ever compared for equality, never dereferenced.
    signals::decodeErrorOnLayer(reinterpret_cast<uint64_t>(this));

    const int n = ++consecutiveErrors_;
    const int suppressed = ++decodeErrorsSinceLog_;

    // A declared-failed layer keeps erroring at its retry cadence forever.
    // Counting continues (above); saying so does not - see quiesceErrorLog().
    if (errorLogQuiesced_.load()) {
        return;
    }

    // Crossing the threshold always speaks - that is the transition to
    // unhealthy, and recovery follows it.
    if (n == DECODE_ERROR_THRESHOLD) {
        LOG_ERROR << "AsyncDecodeQueue: " << n << " consecutive decode errors for "
                  << filename_ << " - queue declared unhealthy, recovery may reopen it";
        lastDecodeErrorLog_ = std::chrono::steady_clock::now();
        decodeErrorsSinceLog_ = 0;
        return;
    }

    // Otherwise report at a bounded rate. Un-throttled this floods the journal
    // on a stream that is damaged but still decoding, because every good frame
    // resets the run and re-arms the "first error" line.
    const auto now = std::chrono::steady_clock::now();
    const bool firstEver = (lastDecodeErrorLog_.time_since_epoch().count() == 0);
    const auto sinceLog = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - lastDecodeErrorLog_).count();
    if (!firstEver && sinceLog < DECODE_ERROR_LOG_INTERVAL_MS) {
        return;
    }

    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(averr, errbuf, AV_ERROR_MAX_STRING_SIZE);
    LOG_ERROR << "AsyncDecodeQueue: decode error in " << where << " for "
              << filename_ << ": " << errbuf << " (" << averr << ")"
              << (suppressed > 1
                      ? " [" + std::to_string(suppressed) + " errors since last report]"
                      : std::string());
    lastDecodeErrorLog_ = now;
    decodeErrorsSinceLog_ = 0;
}

bool AsyncDecodeQueue::decodeNextFrame() {
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        recordDecodeError(AVERROR(ENOMEM), "av_packet_alloc");
        return false;
    }
    
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
            // End of stream - NOT an error. This is the bare receive-EOF at the
            // loop top; the post-drain eofReached_ path below is the other one.
            av_packet_free(&packet);
            return false;
        } else {
            recordDecodeError(ret, "avcodec_receive_frame");
            av_packet_free(&packet);
            return false;
        }
        
        // Read next packet
        ret = av_read_frame(formatCtx_, packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
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

                    // A very short clip can reach EOF with its first frame,
                    // so the latch has to be armed from here too (defect 6(b)).
                    noteDecodedFormat(decodeFrame_);

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
                        // Outside queueMutex_, as in the normal path below.
                        qf.exported = exportForQueue(qf.frame, drainFrameNum);
                        insertSorted(std::move(qf));

                        lastDecodedFrame_ = drainFrameNum;
                        drainCount++;
                    }
                }
                if (drainCount > 0) {
                    LOG_INFO << "AsyncDecodeQueue: Queued " << drainCount
                             << " drained frames from reorder buffer";
                    queueCond_.notify_all();
                }
                avcodec_flush_buffers(codecCtx_);

                if (loopMode_ && totalFrames_ > 0) {
                    // Loop mode: seek to start and pre-buffer frames with virtual numbers
                    if (formatCtx_->pb) formatCtx_->pb->eof_reached = 0;
                    av_seek_frame(formatCtx_, videoStream_, 0, AVSEEK_FLAG_BACKWARD);
                    lastDecodedFrame_ = -1;
                    virtualOffset_ = totalFrames_.load();
                    LOG_INFO << "AsyncDecodeQueue: EOF in loop mode, pre-buffering start frames (virtualOffset="
                             << virtualOffset_.load() << ")";
                    continue;  // Re-read packet from seeked position
                } else {
                    // Non-loop mode: stop decoding and hold last frames.
                    // Clean end of stream, not a fault - do NOT count it.
                    eofReached_ = true;
                    LOG_INFO << "AsyncDecodeQueue: EOF reached, stopping decode (non-loop)";
                    av_packet_free(&packet);
                    return false;
                }
            } else {
                recordDecodeError(ret, "av_read_frame");
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
            recordDecodeError(ret, "avcodec_send_packet");
            av_packet_free(&packet);
            return false;
        }
    }
    
    av_packet_free(&packet);
    
    if (!gotFrame) {
        // Not an error: the packet budget ran out (or a stop was requested)
        // without the decoder producing a frame. The thread simply tries again.
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
    
    // Defect 6(b): the same comparison the sync below makes, but recorded when
    // it FAILS instead of only acted on when it succeeds.
    noteDecodedFormat(decodeFrame_);

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
        recordDecodeError(AVERROR(ENOMEM), "av_frame_alloc");
        return false;
    }
    
    // A decoded frame clears the error run, and counts as recovery's evidence
    // that this layer is actually delivering pictures again (RecoveryPolicy's
    // decay input). Counted here, at the one point a frame is known good -
    // decodeNextFrame() also returns false for EOF and for "no frame after 100
    // packets", neither of which is a delivered picture.
    consecutiveErrors_ = 0;
    ++framesDecoded_;

    // Move frame data (avoids copy for hardware frames)
    av_frame_move_ref(qf.frame, decodeFrame_);
    qf.ready = true;

    // Export before taking queueMutex_: the export is the slow driver call
    // this change moves off the render thread, and holding the queue lock
    // across it would make getFrame() wait on it instead (1amfwm7gq3x).
    qf.exported = exportForQueue(qf.frame, frameNum);

    // Add to queue (deduplicated, sorted)
    insertSorted(std::move(qf));
    
    lastDecodedFrame_ = frameNum;
    
    // Notify waiting threads
    queueCond_.notify_all();
    
    return true;
}

void AsyncDecodeQueue::insertSorted(QueuedFrame&& qf) {
    // One copy of the sorted, deduplicated insert for the drain path and the
    // normal path (they used to be two copies, 1amfwm7gq3x).
    std::lock_guard<std::mutex> lock(queueMutex_);
    for (auto it = frameQueue_.begin(); it != frameQueue_.end(); ++it) {
        if (it->frameNumber == qf.frameNumber) {
            // Duplicate: the newer decode replaces the entry, frame and export
            // together - the old export belongs to the old frame.
            *it = std::move(qf);
            return;
        }
        if (it->frameNumber > qf.frameNumber) {
            frameQueue_.insert(it, std::move(qf));
            return;
        }
    }
    frameQueue_.push_back(std::move(qf));
}

std::shared_ptr<const ExportedSurface> AsyncDecodeQueue::exportForQueue(const AVFrame* frame,
                                                                        int64_t frameNum) {
    if (!frame) return nullptr;
    // A frame below the trim window is dropped by the trim right after this
    // decode (post-seek catch-up from a keyframe, up to a GOP of them): no
    // export for it. Virtual loop frames (totalFrames_ + n) are always above.
    if (frameNum < targetFrame_.load() - 2) return nullptr;
    if (exporter_) return exporter_(frame);   // test seam, any pixel format
    if (!decodeThreadExport_ || !useHardware_ || frame->format != AV_PIX_FMT_VAAPI) return nullptr;
    return exportVaapi(frame);
}

AVPixelFormat AsyncDecodeQueue::getFormatCallback(AVCodecContext* ctx, const AVPixelFormat* fmts) {
    // The format FFmpeg would pick without a callback (VAAPI when the device
    // context matches, else the software format). Only the pool changes.
    const AVPixelFormat chosen = avcodec_default_get_format(ctx, fmts);
    auto* self = static_cast<AsyncDecodeQueue*>(ctx->opaque);
    if (chosen == AV_PIX_FMT_VAAPI && self) {
        self->buildWarmPool(ctx);
    }
    return chosen;
}

void AsyncDecodeQueue::buildWarmPool(AVCodecContext* ctx) {
    // ff_get_format drops the previous hwaccel and its frames context before
    // it calls get_format, and ff_decode_get_hw_frames_ctx keeps a frames
    // context set here (FFmpeg 5.1 decode.c:961, :1097). Any failure below
    // leaves hw_frames_ctx unset: FFmpeg then builds its own pool exactly as
    // before, and no surface has a warm-up record, so nothing is exported on
    // this thread.
    warmInode_.clear();
    warmSurfaces_ = 0;
    warmFailed_ = 0;
    if (!ctx->hw_device_ctx || ctx->hw_frames_ctx) return;

    const auto t0 = std::chrono::steady_clock::now();
    AVBufferRef* ref = nullptr;
    int ret = avcodec_get_hw_frames_parameters(ctx, ctx->hw_device_ctx, AV_PIX_FMT_VAAPI, &ref);
    if (ret < 0 || !ref) {
        LOG_WARNING << "AsyncDecodeQueue: VAAPI frame parameters unavailable (" << ret
                    << ") for " << filename_ << " - decoder builds its own pool, no decode-thread export";
        return;
    }
    auto* fc = reinterpret_cast<AVHWFramesContext*>(ref->data);
    // The 3 base work surfaces ff_decode_get_hw_frames_ctx adds on top
    // (decode.c:987-991): the same pool FFmpeg would build.
    if (fc->initial_pool_size) fc->initial_pool_size += 3;
    ret = av_hwframe_ctx_init(ref);
    if (ret < 0) {
        av_buffer_unref(&ref);
        LOG_WARNING << "AsyncDecodeQueue: VAAPI pool init failed (" << ret << ") for " << filename_
                    << " - decoder builds its own pool, no decode-thread export";
        return;
    }
    auto* vfc = static_cast<AVVAAPIFramesContext*>(fc->hwctx);
    auto* vdc = static_cast<AVVAAPIDeviceContext*>(fc->device_ctx->hwctx);

    int failed = 0;
    for (int i = 0; vfc->surface_ids && i < vfc->nb_surfaces; ++i) {
        const VASurfaceID id = vfc->surface_ids[i];
        // The first export of a surface is where a driver may reallocate it
        // (Mesa 22.3.6 surface.c:1428-1453, an interlaced buffer made
        // progressive and weave-copied); here nothing is pending on it. The
        // buffer it returns is the one every later export must return too.
        auto e = ExportedSurface::exportFrom(vdc->display, id);
        struct stat st{};
        if (!e || e->descriptor().num_objects == 0 ||
            fstat(e->descriptor().objects[0].fd, &st) != 0) {
            ++failed;
            continue;
        }
        warmInode_[id] = st.st_ino;
    }
    warmSurfaces_ = static_cast<int>(warmInode_.size());
    warmFailed_ = failed;
    ctx->hw_frames_ctx = ref;

    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    LOG_INFO << "AsyncDecodeQueue: VAAPI pool of " << vfc->nb_surfaces << " surfaces for "
             << filename_ << ", warm-up export: " << warmSurfaces_.load() << " ok, "
             << failed << " failed, " << static_cast<int>(ms + 0.5) << " ms";
}

std::shared_ptr<const ExportedSurface> AsyncDecodeQueue::exportVaapi(const AVFrame* frame) {
    const VASurfaceID id = static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(frame->data[3]));
    auto warm = warmInode_.find(id);
    if (warm == warmInode_.end() || !frame->hw_frames_ctx) {
        if (!loggedNoWarmRecord_) {
            loggedNoWarmRecord_ = true;
            LOG_INFO << "AsyncDecodeQueue: surface " << id << " of " << filename_
                     << " has no warm-up record - the render thread exports it";
        }
        return nullptr;
    }
    auto* fc = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
    auto* vdc = static_cast<AVVAAPIDeviceContext*>(fc->device_ctx->hwctx);

    VAStatus status = VA_STATUS_SUCCESS;
    auto exported = ExportedSurface::exportFrom(vdc->display, id, &status);
    if (!exported || exported->descriptor().num_objects == 0) {
        if (!loggedExportFailed_) {
            loggedExportFailed_ = true;
            LOG_WARNING << "AsyncDecodeQueue: decode-thread vaExportSurfaceHandle failed (" << status
                        << ") for " << filename_ << " - the render thread exports instead";
        }
        return nullptr;
    }
    // The buffer must be the one the warm-up left. A different one means the
    // driver reallocated it now, right after a decode: not trusted here.
    struct stat st{};
    if (fstat(exported->descriptor().objects[0].fd, &st) != 0 || st.st_ino != warm->second) {
        if (!loggedInodeChanged_) {
            loggedInodeChanged_ = true;
            LOG_WARNING << "AsyncDecodeQueue: surface " << id << " of " << filename_
                        << " changed buffer after its warm-up export - the render thread exports it";
        }
        warm->second = st.st_ino;
        return nullptr;
    }
    return exported;
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

