#include "FFmpegLiveInput.h"
#include "../utils/Logger.h"
#include <cuems_mediadecoder/FFmpegUtils.h>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace videocomposer {

FFmpegLiveInput::FFmpegLiveInput()
    : ready_(false)
    , frameCount_(0)
{
    frameInfo_ = {};
}

FFmpegLiveInput::~FFmpegLiveInput() {
    close();
}

bool FFmpegLiveInput::open(const std::string& source) {
    close();  // Close any existing connection

    // Open with format if specified (e.g., "v4l2" for /dev/video*)
    if (!mediaReader_.open(source, format_)) {
        LOG_ERROR << "Failed to open live source: " << source;
        return false;
    }

    // Get video stream
    int videoStream = mediaReader_.findStream(AVMEDIA_TYPE_VIDEO);
    if (videoStream < 0) {
        LOG_ERROR << "No video stream found in live source";
        mediaReader_.close();
        return false;
    }

    // Get codec parameters
    auto codecParams = mediaReader_.getCodecParameters(videoStream);
    if (!codecParams) {
        LOG_ERROR << "Failed to get codec parameters";
        mediaReader_.close();
        return false;
    }

    // Open video decoder (software only for live streams)
    if (!videoDecoder_.openCodec(codecParams)) {
        LOG_ERROR << "Failed to open video decoder for live source";
        mediaReader_.close();
        return false;
    }

    // Get stream for framerate
    AVStream* avStream = mediaReader_.getStream(videoStream);
    if (!avStream) {
        LOG_ERROR << "Failed to get stream";
        mediaReader_.close();
        return false;
    }

    // Get frame info
    frameInfo_.width = codecParams->width;
    frameInfo_.height = codecParams->height;
    frameInfo_.aspect = static_cast<float>(codecParams->width) / static_cast<float>(codecParams->height);
    
    // Try to get framerate from stream (r_frame_rate is preferred over avg_frame_rate)
    double framerate = 0.0;
    if (avStream->r_frame_rate.den > 0 && avStream->r_frame_rate.num > 0) {
        framerate = av_q2d(avStream->r_frame_rate);
    }
    if (framerate <= 0.0 && avStream->avg_frame_rate.den > 0 && avStream->avg_frame_rate.num > 0) {
        framerate = av_q2d(avStream->avg_frame_rate);
    }
    if (framerate <= 0.0) {
        framerate = 25.0;  // Default
    }
    frameInfo_.framerate = framerate;

    frameInfo_.format = PixelFormat::RGBA32;
    frameInfo_.totalFrames = 0;  // Live stream
    frameInfo_.duration = 0.0;

    ready_ = true;
    startCaptureThread();  // Start async capture
    LOG_INFO << "Opened live source: " << source << " (" << frameInfo_.width << "x" << frameInfo_.height << ")";
    return true;
}

void FFmpegLiveInput::close() {
    stopCaptureThread();  // Stop async capture first

    videoDecoder_.close();
    mediaReader_.close();
    ready_ = false;
    frameCount_ = 0;
}

bool FFmpegLiveInput::isReady() const {
    return ready_ && mediaReader_.isReady() && videoDecoder_.isReady();
}

FrameInfo FFmpegLiveInput::getFrameInfo() const {
    return frameInfo_;
}

int64_t FFmpegLiveInput::getCurrentFrame() const {
    return frameCount_.load();
}

InputSource::CodecType FFmpegLiveInput::detectCodec() const {
    return CodecType::SOFTWARE;  // Live streams via FFmpeg are software-decoded
}

InputSource::DecodeBackend FFmpegLiveInput::getOptimalBackend() const {
    return DecodeBackend::CPU_SOFTWARE;
}

bool FFmpegLiveInput::captureFrame(FrameBuffer& buffer) {
    if (!isReady()) {
        return false;
    }

    // Read packet
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return false;
    }

    int ret = mediaReader_.readPacket(packet);
    if (ret < 0) {
        av_packet_free(&packet);
        return false;  // No packet available or error
    }

    // Send packet to decoder
    if (videoDecoder_.sendPacket(packet) < 0) {
        av_packet_free(&packet);
        return false;
    }

    av_packet_free(&packet);

    // Receive frame
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        return false;
    }

    ret = videoDecoder_.receiveFrame(frame);
    if (ret < 0) {
        av_frame_free(&frame);
        return false;  // No frame available yet
    }

    // Convert frame to RGBA
    FrameInfo info;
    info.width = frame->width;
    info.height = frame->height;
    info.aspect = static_cast<float>(frame->width) / static_cast<float>(frame->height);
    info.format = PixelFormat::RGBA32;

    if (!buffer.allocate(info)) {
        av_frame_free(&frame);
        return false;
    }

    // Convert frame format to RGBA (using swscale)
    SwsContext* swsCtx = sws_getContext(
        frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
        frame->width, frame->height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (swsCtx) {
        uint8_t* dstData[1] = { buffer.data() };
        // RGBA = 4 bytes per pixel
        int dstLinesize[1] = { frame->width * 4 };
        sws_scale(swsCtx, frame->data, frame->linesize, 0, frame->height, dstData, dstLinesize);
        sws_freeContext(swsCtx);
        frameCount_++;
        av_frame_free(&frame);
        return true;
    }

    av_frame_free(&frame);
    return false;
}

} // namespace videocomposer

