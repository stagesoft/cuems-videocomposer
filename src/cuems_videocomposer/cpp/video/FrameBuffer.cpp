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

#include "FrameBuffer.h"
#include <cstdlib>
#include <cstring>
#include <utility>  // for std::swap

extern "C" {
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

namespace videocomposer {

FrameBuffer::FrameBuffer() : buffer_(nullptr), size_(0), ownsBuffer_(true) {
}

FrameBuffer::FrameBuffer(const FrameBuffer& other)
    : buffer_(nullptr), size_(0), info_(other.info_), ownsBuffer_(true) {
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
        ownsBuffer_ = true;  // deep copy always owns
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

FrameBuffer::FrameBuffer(FrameBuffer&& other) noexcept
    : buffer_(other.buffer_), size_(other.size_), info_(other.info_), ownsBuffer_(other.ownsBuffer_) {
    // Take ownership, leave other in valid empty state
    other.buffer_ = nullptr;
    other.size_ = 0;
    other.info_ = {};
    other.ownsBuffer_ = true;
}

FrameBuffer& FrameBuffer::operator=(FrameBuffer&& other) noexcept {
    if (this != &other) {
        release();
        buffer_ = other.buffer_;
        size_ = other.size_;
        info_ = other.info_;
        ownsBuffer_ = other.ownsBuffer_;
        // Leave other in valid empty state
        other.buffer_ = nullptr;
        other.size_ = 0;
        other.info_ = {};
        other.ownsBuffer_ = true;
    }
    return *this;
}

void FrameBuffer::swap(FrameBuffer& other) noexcept {
    std::swap(buffer_, other.buffer_);
    std::swap(size_, other.size_);
    std::swap(info_, other.info_);
    std::swap(ownsBuffer_, other.ownsBuffer_);
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
    if (buffer_ && ownsBuffer_) {
        free(buffer_);
    }
    buffer_ = nullptr;
    size_ = 0;
    ownsBuffer_ = true;
}

void FrameBuffer::copyFrom(const FrameBuffer& source) {
    // Release any owned buffer first
    release();
    // Create a non-owning view: same pointer + metadata, no pixel copy
    buffer_ = source.buffer_;
    size_ = source.size_;
    info_ = source.info_;
    ownsBuffer_ = false;  // we don't own this data
}

} // namespace videocomposer

