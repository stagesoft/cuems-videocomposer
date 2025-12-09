/**
 * AsyncDecodeQueue.cpp - Threaded frame decoder with pre-buffering
 * 
 * Implements mpv-style async decoding to decouple decode latency from display timing.
 */

#include "AsyncDecodeQueue.h"
#include "../utils/Logger.h"
#include <chrono>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
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
    const AVCodec* codec = nullptr;
    
    // Try hardware decoder first if context provided
    if (hwDeviceCtx_) {
        // Get hardware decoder name based on codec
        const char* hwDecoderName = nullptr;
        switch (codecpar->codec_id) {
            case AV_CODEC_ID_H264:
                hwDecoderName = "h264_vaapi";
                break;
            case AV_CODEC_ID_HEVC:
                hwDecoderName = "hevc_vaapi";
                break;
            case AV_CODEC_ID_VP9:
                hwDecoderName = "vp9_vaapi";
                break;
            case AV_CODEC_ID_AV1:
                hwDecoderName = "av1_vaapi";
                break;
            default:
                break;
        }
        
        if (hwDecoderName) {
            codec = avcodec_find_decoder_by_name(hwDecoderName);
            if (codec) {
                LOG_INFO << "AsyncDecodeQueue: Found hardware decoder " << hwDecoderName;
                useHardware_ = true;
            }
        }
    }
    
    // Fallback to software decoder
    if (!codec) {
        codec = avcodec_find_decoder(codecpar->codec_id);
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
    
    // Clear queue
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        frameQueue_.clear();
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

AVFrame* AsyncDecodeQueue::getFrame(int64_t frameNumber, int maxWaitMs) {
    std::unique_lock<std::mutex> lock(queueMutex_);
    
    // Update target so decode thread knows what we need
    targetFrame_ = frameNumber;
    
    // Look for frame in queue
    for (auto& qf : frameQueue_) {
        if (qf.frameNumber == frameNumber && qf.ready) {
            return qf.frame;
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
            
            // Check again
            for (auto& qf : frameQueue_) {
                if (qf.frameNumber == frameNumber && qf.ready) {
                    return qf.frame;
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
    
    return closest;
}

void AsyncDecodeQueue::seek(int64_t frameNumber) {
    // Set seek request
    seekTarget_ = frameNumber;
    seekRequested_ = true;
    
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
            int64_t seekFrame = seekTarget_.load();
            
            if (!seekInternal(seekFrame)) {
                LOG_WARNING << "AsyncDecodeQueue: Seek to frame " << seekFrame << " failed";
            }
            
            // Flush decoder
            avcodec_flush_buffers(codecCtx_);
            lastDecodedFrame_ = seekFrame - 1;
        }
        
        // Check if we should decode more
        int64_t target = targetFrame_.load();
        int64_t lastDecoded = lastDecodedFrame_.load();
        
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
        if (queueSize < MAX_QUEUE_SIZE) {
            // Queue not full
            if (newestInQueue < 0) {
                // Queue empty - start from target
                shouldDecode = true;
            } else if (newestInQueue < target + static_cast<int64_t>(MAX_QUEUE_SIZE)) {
                // Buffer ahead of target
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
            
            // Remove frames that are more than 2 frames behind current
            while (!frameQueue_.empty() && frameQueue_.front().frameNumber < current - 2) {
                frameQueue_.pop_front();
            }
        }
    }
    
    LOG_INFO << "AsyncDecodeQueue: Decode thread stopped";
}

bool AsyncDecodeQueue::decodeNextFrame() {
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
                // Send flush packet
                avcodec_send_packet(codecCtx_, nullptr);
            }
            av_packet_free(&packet);
            return false;
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
            // Error
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
        // No PTS - use sequential numbering
        frameNum = lastDecodedFrame_ + 1;
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
    
    // Add to queue
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        
        // Insert in order
        bool inserted = false;
        for (auto it = frameQueue_.begin(); it != frameQueue_.end(); ++it) {
            if (it->frameNumber > frameNum) {
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
    
    // Notify waiting threads
    queueCond_.notify_all();
    
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
    
    return ret >= 0;
}

} // namespace videocomposer

