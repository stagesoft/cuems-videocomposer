#include "FrameBuffer.h"
#include <cstdlib>
#include <cstring>

extern "C" {
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

namespace videocomposer {

FrameBuffer::FrameBuffer() : buffer_(nullptr), size_(0) {
}

FrameBuffer::FrameBuffer(const FrameBuffer& other) 
    : buffer_(nullptr), size_(0), info_(other.info_) {
    // Deep copy: allocate new buffer and copy data
    if (other.buffer_ && other.size_ > 0 && other.info_.width > 0 && other.info_.height > 0) {
        if (allocate(other.info_)) {
            // Only copy if allocation succeeded and sizes match
            if (buffer_ && size_ == other.size_) {
                memcpy(buffer_, other.buffer_, size_);
            }
        }
    }
}

FrameBuffer& FrameBuffer::operator=(const FrameBuffer& other) {
    if (this != &other) {
        release();
        info_ = other.info_;
        // Deep copy: allocate new buffer and copy data
        if (other.buffer_ && other.size_ > 0 && other.info_.width > 0 && other.info_.height > 0) {
            if (allocate(other.info_)) {
                // Only copy if allocation succeeded and sizes match
                if (buffer_ && size_ == other.size_) {
                    memcpy(buffer_, other.buffer_, size_);
                }
            }
        }
    }
    return *this;
}

FrameBuffer::~FrameBuffer() {
    release();
}

bool FrameBuffer::allocate(const FrameInfo& info) {
    release();
    
    info_ = info;
    
    // Convert PixelFormat to AVPixelFormat
    AVPixelFormat avFormat;
    switch (info.format) {
        case PixelFormat::YUV420P:
            avFormat = AV_PIX_FMT_YUV420P;
            break;
        case PixelFormat::RGB24:
            avFormat = AV_PIX_FMT_RGB24;
            break;
        case PixelFormat::RGBA32:
            avFormat = AV_PIX_FMT_RGB32;
            break;
        case PixelFormat::BGRA32:
            avFormat = AV_PIX_FMT_BGR32;
            break;
        case PixelFormat::UYVY422:
            avFormat = AV_PIX_FMT_UYVY422;
            break;
        default:
            avFormat = AV_PIX_FMT_YUV420P;
            break;
    }
    
    // Calculate buffer size using modern FFmpeg API
    int linesize[4];
    int ret = av_image_get_buffer_size(avFormat, info.width, info.height, 1);
    if (ret < 0) {
        return false;
    }
    size_ = ret;
    
    // Allocate buffer
    buffer_ = static_cast<uint8_t*>(calloc(1, size_));
    if (!buffer_) {
        size_ = 0;
        return false;
    }
    
    return true;
}

void FrameBuffer::release() {
    if (buffer_) {
        free(buffer_);
        buffer_ = nullptr;
    }
    size_ = 0;
}

} // namespace videocomposer

