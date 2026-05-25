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

#include "AsyncHapDecoder.h"
#include "../utils/Logger.h"

#include <algorithm>
#include <chrono>
#include <pthread.h>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
}

namespace videocomposer {

AsyncHapDecoder::AsyncHapDecoder()
    : codecCtx_(nullptr)
    , videoStream_(-1)
    , width_(0)
    , height_(0)
    , framerate_(0)
    , frameCount_(0)
    , ready_(false)
{
    timeBase_ = {1, 1};
    frameRateQ_ = {1, 1};
}

AsyncHapDecoder::~AsyncHapDecoder() {
    close();
}

bool AsyncHapDecoder::open(const std::string& filename) {
    close();

    filename_ = filename;

    if (!mediaReader_.open(filename)) {
        LOG_ERROR << "AsyncHapDecoder: [HAP-DECODE] failed to open " << filename;
        return false;
    }

    videoStream_ = mediaReader_.findStream(AVMEDIA_TYPE_VIDEO);
    if (videoStream_ < 0) {
        LOG_ERROR << "AsyncHapDecoder: [HAP-DECODE] no video stream in " << filename;
        mediaReader_.close();
        return false;
    }

    AVStream* stream = mediaReader_.getStream(videoStream_);
    AVCodecParameters* codecpar = mediaReader_.getCodecParameters(videoStream_);
    if (!stream || !codecpar) {
        LOG_ERROR << "AsyncHapDecoder: [HAP-DECODE] failed to get stream metadata";
        mediaReader_.close();
        return false;
    }

    // Allocate codec context for avcodec_flush_buffers on seek.
    // We do NOT send packets through the codec — HAP direct decode bypasses it
    // entirely. The flush is needed to keep FFmpeg's internal state in sync
    // with seek operations on the demuxer.
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (codec) {
        codecCtx_ = avcodec_alloc_context3(codec);
        if (codecCtx_) {
            if (avcodec_parameters_to_context(codecCtx_, codecpar) < 0 ||
                avcodec_open2(codecCtx_, codec, nullptr) < 0) {
                avcodec_free_context(&codecCtx_);
                codecCtx_ = nullptr;
            }
        }
    }

    width_ = codecpar->width;
    height_ = codecpar->height;
    timeBase_ = stream->time_base;

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

    double duration = mediaReader_.getDuration();
    frameCount_ = (framerate_ > 0 && duration > 0)
        ? static_cast<int64_t>(framerate_ * duration)
        : 0;

    ready_ = true;

    LOG_INFO << "AsyncHapDecoder: [HAP-DECODE] opened " << filename
             << " " << width_ << "x" << height_ << "@" << framerate_ << "fps";

    threadStop_ = false;
    targetFrame_ = 0;
    lastDecodedFrame_ = -1;
    decodeThread_ = std::make_unique<std::thread>(&AsyncHapDecoder::decodeThreadFunc, this);

    return true;
}

void AsyncHapDecoder::close() {
    if (decodeThread_) {
        threadStop_ = true;
        queueCond_.notify_all();
        if (decodeThread_->joinable()) {
            decodeThread_->join();
        }
        decodeThread_.reset();
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        frameQueue_.clear();
    }
    borrowedFrame_ = HapDecodedFrame{};

    if (codecCtx_) {
        avcodec_free_context(&codecCtx_);
    }
    mediaReader_.close();

    videoStream_ = -1;
    ready_ = false;
}

const HapDecodedFrame* AsyncHapDecoder::borrowFrame(const HapDecodedFrame& src) {
    // Caller holds queueMutex_. Deep-copy the DXT blobs into borrowedFrame_;
    // the queue slot can then be trimmed or replaced without affecting the
    // render thread's upload.
    borrowedFrame_.frameNumber = src.frameNumber;
    borrowedFrame_.variant = src.variant;
    borrowedFrame_.ready = src.ready;
    borrowedFrame_.textures = src.textures;  // vector copy: includes the inner std::vector<uint8_t> data
    return &borrowedFrame_;
}

const HapDecodedFrame* AsyncHapDecoder::getFrame(int64_t frameNumber, int maxWaitMs) {
    std::unique_lock<std::mutex> lock(queueMutex_);

    targetFrame_ = frameNumber;

    for (auto& qf : frameQueue_) {
        if (qf.frameNumber == frameNumber && qf.ready) {
            return borrowFrame(qf);
        }
    }

    if (maxWaitMs > 0) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxWaitMs);
        while (std::chrono::steady_clock::now() < deadline) {
            queueCond_.notify_one();
            queueCond_.wait_for(lock, std::chrono::milliseconds(1));
            for (auto& qf : frameQueue_) {
                if (qf.frameNumber == frameNumber && qf.ready) {
                    return borrowFrame(qf);
                }
            }
        }
    }

    // Fall back to closest earlier frame (hold-last semantics)
    const HapDecodedFrame* closest = nullptr;
    int64_t closestDiff = INT64_MAX;
    for (auto& qf : frameQueue_) {
        if (qf.ready && qf.frameNumber <= frameNumber) {
            int64_t diff = frameNumber - qf.frameNumber;
            if (diff < closestDiff) {
                closestDiff = diff;
                closest = &qf;
            }
        }
    }
    if (closest) {
        return borrowFrame(*closest);
    }
    return nullptr;
}

void AsyncHapDecoder::seek(int64_t frameNumber) {
    seekTarget_ = frameNumber;
    seekRequested_ = true;
    eofReached_ = false;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        frameQueue_.clear();
    }
    targetFrame_ = frameNumber;
    lastDecodedFrame_ = -1;
    queueCond_.notify_all();
}

bool AsyncHapDecoder::hasFrame(int64_t frameNumber) const {
    std::lock_guard<std::mutex> lock(queueMutex_);
    for (const auto& qf : frameQueue_) {
        if (qf.frameNumber == frameNumber && qf.ready) return true;
    }
    return false;
}

void AsyncHapDecoder::setTargetFrame(int64_t frameNumber) {
    targetFrame_ = frameNumber;
    queueCond_.notify_one();
}

void AsyncHapDecoder::setLoopMode(bool loop) {
    loopMode_ = loop;
    if (loop) {
        eofReached_ = false;  // resume decode if we'd stopped at EOF
        queueCond_.notify_one();
    }
}

size_t AsyncHapDecoder::getQueueSize() const {
    std::lock_guard<std::mutex> lock(queueMutex_);
    return frameQueue_.size();
}

int64_t AsyncHapDecoder::getOldestFrame() const {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (frameQueue_.empty()) return -1;
    return frameQueue_.front().frameNumber;
}

int64_t AsyncHapDecoder::getNewestFrame() const {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (frameQueue_.empty()) return -1;
    return frameQueue_.back().frameNumber;
}

void AsyncHapDecoder::insertFrameSorted(HapDecodedFrame&& qf) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    int64_t fn = qf.frameNumber;
    for (auto it = frameQueue_.begin(); it != frameQueue_.end(); ++it) {
        if (it->frameNumber == fn) {
            *it = std::move(qf);  // dedup: newer decode replaces older
            return;
        } else if (it->frameNumber > fn) {
            frameQueue_.insert(it, std::move(qf));
            return;
        }
    }
    frameQueue_.push_back(std::move(qf));
}

void AsyncHapDecoder::decodeThreadFunc() {
    pthread_setname_np(pthread_self(), "hap-decoder");
    LOG_INFO << "AsyncHapDecoder: [HAP-DECODE] worker thread started";

    while (!threadStop_) {
        if (seekRequested_) {
            seekRequested_ = false;
            eofReached_ = false;
            int64_t seekFrame = seekTarget_.load();
            if (!seekInternal(seekFrame)) {
                LOG_WARNING << "AsyncHapDecoder: [HAP-DECODE] seek to frame " << seekFrame << " failed";
            }
            if (codecCtx_) avcodec_flush_buffers(codecCtx_);
            lastDecodedFrame_ = seekFrame - 1;
        }

        // Backward/forward jump detection
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            int64_t current = targetFrame_.load();

            if (!frameQueue_.empty()) {
                int64_t oldest = frameQueue_.front().frameNumber;
                if (oldest > current + static_cast<int64_t>(MAX_QUEUE_SIZE)) {
                    LOG_INFO << "AsyncHapDecoder: [HAP-DECODE] backward jump oldest=" << oldest
                             << " target=" << current << " — clearing";
                    frameQueue_.clear();
                    seekTarget_ = current;
                    seekRequested_ = true;
                    lastDecodedFrame_ = -1;
                    eofReached_ = false;
                    continue;
                }
            } else if (lastDecodedFrame_ >= 0 &&
                       current < lastDecodedFrame_ - static_cast<int64_t>(MAX_QUEUE_SIZE)) {
                LOG_INFO << "AsyncHapDecoder: [HAP-DECODE] backward jump (empty queue) lastDecoded="
                         << lastDecodedFrame_.load() << " target=" << current << " — seeking";
                seekTarget_ = current;
                seekRequested_ = true;
                lastDecodedFrame_ = -1;
                continue;
            }

            if (lastDecodedFrame_.load() >= 0 &&
                current > lastDecodedFrame_.load() + FORWARD_JUMP_THRESHOLD) {
                LOG_INFO << "AsyncHapDecoder: [HAP-DECODE] forward jump target=" << current
                         << " lastDecoded=" << lastDecodedFrame_.load() << " — seeking";
                frameQueue_.clear();
                seekTarget_ = current;
                seekRequested_ = true;
                lastDecodedFrame_ = -1;
                eofReached_ = false;
                continue;
            }
        }

        // Decide whether to decode another frame
        int64_t target = targetFrame_.load();
        size_t queueSize;
        int64_t newestInQueue = -1;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            queueSize = frameQueue_.size();
            if (!frameQueue_.empty()) newestInQueue = frameQueue_.back().frameNumber;
        }

        bool shouldDecode = false;
        if (eofReached_) {
            // Non-loop EOF: hold last frame until a seek clears it
        } else if (queueSize < MAX_QUEUE_SIZE) {
            if (newestInQueue < 0 ||
                newestInQueue < target + static_cast<int64_t>(MAX_QUEUE_SIZE)) {
                shouldDecode = true;
            }
        }

        if (shouldDecode && !threadStop_) {
            if (!decodeNextFrame()) {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueCond_.wait_for(lock, std::chrono::milliseconds(10));
            }
        } else {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCond_.wait_for(lock, std::chrono::milliseconds(5));
        }

        // Trim stale frames behind the playhead
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            int64_t current = targetFrame_.load();
            while (!frameQueue_.empty() &&
                   frameQueue_.front().frameNumber < current - 2) {
                frameQueue_.pop_front();
            }
        }
    }

    LOG_INFO << "AsyncHapDecoder: [HAP-DECODE] worker thread stopped";
}

bool AsyncHapDecoder::decodeNextFrame() {
    AVPacket* packet = av_packet_alloc();
    if (!packet) return false;

    int bailout = 100;  // skip non-video packets
    while (bailout-- > 0 && !threadStop_) {
        av_packet_unref(packet);
        int err = mediaReader_.readPacket(packet);
        if (err < 0) {
            if (err == AVERROR_EOF) {
                if (loopMode_) {
                    // Seek to start and continue. HAP is all-keyframes so seek
                    // is ~1-2 ms; no need for virtual-frame pre-buffering.
                    AVFormatContext* fctx = mediaReader_.getFormatContext();
                    if (fctx && fctx->pb) fctx->pb->eof_reached = 0;
                    if (!mediaReader_.seekToTime(0.0, videoStream_, AVSEEK_FLAG_BACKWARD)) {
                        LOG_WARNING << "AsyncHapDecoder: [HAP-DECODE] EOF loop seek failed";
                        av_packet_free(&packet);
                        return false;
                    }
                    if (codecCtx_) avcodec_flush_buffers(codecCtx_);
                    lastDecodedFrame_ = -1;
                    LOG_INFO << "AsyncHapDecoder: [HAP-DECODE] EOF in loop, seeking to 0";
                    continue;  // re-read from start
                } else {
                    eofReached_ = true;
                    av_packet_free(&packet);
                    return false;
                }
            }
            av_packet_free(&packet);
            return false;
        }

        if (packet->stream_index != videoStream_) {
            continue;
        }

        // Got a video packet — decode it
        break;
    }

    if (packet->size == 0 || !packet->data) {
        av_packet_free(&packet);
        return false;
    }

    HapVariant variant = HapDecoder::getVariant(packet->data, packet->size);
    if (variant == HapVariant::NONE) {
        LOG_WARNING << "AsyncHapDecoder: [HAP-DECODE] unknown HAP variant in packet";
        av_packet_free(&packet);
        return false;
    }

    std::vector<HapDecodedTexture> textures;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = hapDecoder_.decode(packet->data, packet->size, width_, height_, textures);
    auto t1 = std::chrono::steady_clock::now();
    if (!ok || textures.empty()) {
        LOG_WARNING << "AsyncHapDecoder: [HAP-DECODE] decode failed: " << hapDecoder_.getLastError();
        av_packet_free(&packet);
        return false;
    }

    // Frame number from packet PTS (HAP packets carry valid PTS directly)
    int64_t pts = packet->pts;
    int64_t frameNum = 0;
    if (pts != AV_NOPTS_VALUE) {
        double seconds = pts * av_q2d(timeBase_);
        frameNum = static_cast<int64_t>(seconds * framerate_ + 0.5);
    } else {
        frameNum = lastDecodedFrame_.load() + 1;
    }

    av_packet_free(&packet);

    HapDecodedFrame qf;
    qf.frameNumber = frameNum;
    qf.variant = variant;
    qf.textures = std::move(textures);
    qf.ready = true;
    insertFrameSorted(std::move(qf));
    lastDecodedFrame_ = frameNum;

    auto decodeMs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;
    LOG_DEBUG << "AsyncHapDecoder: [HAP-DECODE] decoded frame=" << frameNum
              << " in " << decodeMs << "ms queue=" << getQueueSize() << "/" << MAX_QUEUE_SIZE;

    queueCond_.notify_all();
    return true;
}

bool AsyncHapDecoder::seekInternal(int64_t frameNumber) {
    if (videoStream_ < 0 || framerate_ <= 0) return false;
    double targetTime = static_cast<double>(frameNumber) / framerate_;
    bool ok = mediaReader_.seekToTime(targetTime, videoStream_, AVSEEK_FLAG_BACKWARD);
    AVFormatContext* fctx = mediaReader_.getFormatContext();
    if (ok && fctx && fctx->pb) {
        fctx->pb->eof_reached = 0;
    }
    return ok;
}

} // namespace videocomposer
