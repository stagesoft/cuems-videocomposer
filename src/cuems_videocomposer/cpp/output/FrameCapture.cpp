/**
 * FrameCapture.cpp - Async frame capture implementation
 */

#include "FrameCapture.h"
#include "../utils/Logger.h"

#include <GL/glew.h>
#include <chrono>
#include <cstring>

namespace videocomposer {

FrameCapture::FrameCapture() {
}

FrameCapture::~FrameCapture() {
    cleanup();
}

bool FrameCapture::initialize(int width, int height, PixelFormat format) {
    if (width <= 0 || height <= 0) {
        LOG_ERROR << "FrameCapture: Invalid dimensions " << width << "x" << height;
        return false;
    }
    
    cleanup();
    
    width_ = width;
    height_ = height;
    format_ = format;
    
    // Calculate frame size
    int bpp = FrameData::bytesPerPixel(format);
    frameSize_ = static_cast<size_t>(width) * height * bpp;
    
    // Check PBO support
    checkPBOSupport();
    
    if (!pboSupported_) {
        LOG_WARNING << "FrameCapture: PBO not supported, capture will be synchronous";
        initialized_ = true;
        return true;
    }
    
    // Create PBOs
    glGenBuffers(NUM_PBOS, pbo_);
    
    for (int i = 0; i < NUM_PBOS; ++i) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, frameSize_, nullptr, GL_STREAM_READ);
    }
    
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    
    // Check for errors
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        LOG_ERROR << "FrameCapture: OpenGL error during PBO creation: " << err;
        cleanup();
        return false;
    }
    
    initialized_ = true;
    LOG_INFO << "FrameCapture: Initialized " << width << "x" << height 
             << " with PBO support";
    
    return true;
}

void FrameCapture::cleanup() {
    // Complete any pending reads
    if (pendingRead_ && pboSupported_) {
        FrameData dummy;
        getCompletedFrame(dummy);
    }
    
    // Delete PBOs
    if (pbo_[0] != 0) {
        glDeleteBuffers(NUM_PBOS, pbo_);
        pbo_[0] = pbo_[1] = 0;
    }
    
    // Clear queue
    std::lock_guard<std::mutex> lock(queueMutex_);
    while (!completedFrames_.empty()) {
        completedFrames_.pop();
    }
    
    initialized_ = false;
    pendingRead_ = false;
    pendingPBO_ = -1;
    currentPBO_ = 0;
}

void FrameCapture::checkPBOSupport() {
    // Check for GL_ARB_pixel_buffer_object extension
    // Modern OpenGL (3.0+) always has it
    
    pboSupported_ = false;
    
    // Check if GLEW is initialized
    if (glewIsSupported("GL_ARB_pixel_buffer_object") ||
        glewIsSupported("GL_EXT_pixel_buffer_object")) {
        pboSupported_ = true;
    }
    
    // Also check GL version (PBO is core in 2.1+)
    const char* version = (const char*)glGetString(GL_VERSION);
    if (version) {
        int major = 0, minor = 0;
        sscanf(version, "%d.%d", &major, &minor);
        if (major > 2 || (major == 2 && minor >= 1)) {
            pboSupported_ = true;
        }
    }
}

GLenum FrameCapture::getGLFormat() const {
    switch (format_) {
        case PixelFormat::RGBA32:
            return GL_RGBA;
        case PixelFormat::BGRA32:
            return GL_BGRA;
        case PixelFormat::RGB24:
            return GL_RGB;
        case PixelFormat::YUV420P:
        case PixelFormat::UYVY422:
            // These formats need special handling, fallback to RGBA for readback
            return GL_RGBA;
    }
    return GL_RGBA;
}

GLenum FrameCapture::getGLType() const {
    return GL_UNSIGNED_BYTE;
}

int64_t FrameCapture::getCurrentTimestamp() const {
    auto now = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch());
    return us.count();
}

void FrameCapture::startCapture() {
    if (!initialized_) {
        return;
    }
    
    // Complete any pending read first
    if (pendingRead_) {
        completePendingRead();
    }
    
    // Bind source FBO
    if (sourceFBO_ != 0) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFBO_);
    }
    
    // Set read buffer
    glReadBuffer(readBuffer_);
    
    GLenum format = getGLFormat();
    GLenum type = getGLType();
    
    if (pboSupported_) {
        // Async read with PBO
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[currentPBO_]);
        
        // Initiate async read (non-blocking)
        glReadPixels(sourceX_, sourceY_, width_, height_, format, type, nullptr);
        
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        
        // Mark this PBO as pending
        pendingPBO_ = currentPBO_;
        pendingRead_ = true;
        
        // Swap to next PBO for next frame
        currentPBO_ = (currentPBO_ + 1) % NUM_PBOS;
        
    } else {
        // Synchronous read (fallback)
        FrameData frame;
        frame.width = width_;
        frame.height = height_;
        frame.format = format_;
        frame.size = frameSize_;
        frame.data = new uint8_t[frameSize_];
        frame.ownsData = true;
        frame.timestamp = getCurrentTimestamp();
        frame.frameNumber = ++frameNumber_;
        
        glReadPixels(sourceX_, sourceY_, width_, height_, format, type, frame.data);
        
        // Add to queue
        std::lock_guard<std::mutex> lock(queueMutex_);
        completedFrames_.push(std::move(frame));
    }
    
    // Unbind source FBO
    if (sourceFBO_ != 0) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    }
}

bool FrameCapture::completePendingRead() {
    if (!pendingRead_ || pendingPBO_ < 0 || !pboSupported_) {
        return false;
    }
    
    // Bind the pending PBO
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[pendingPBO_]);
    
    // Map buffer (this will block until transfer is complete)
    void* mappedData = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    
    if (mappedData) {
        // Create frame with copy of data
        FrameData frame;
        frame.width = width_;
        frame.height = height_;
        frame.format = format_;
        frame.size = frameSize_;
        frame.data = new uint8_t[frameSize_];
        frame.ownsData = true;
        frame.timestamp = getCurrentTimestamp();
        frame.frameNumber = ++frameNumber_;
        
        memcpy(frame.data, mappedData, frameSize_);
        
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        
        // Add to queue
        std::lock_guard<std::mutex> lock(queueMutex_);
        completedFrames_.push(std::move(frame));
    }
    
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    
    pendingRead_ = false;
    pendingPBO_ = -1;
    
    return true;
}

bool FrameCapture::getCompletedFrame(FrameData& frame) {
    // Complete any pending read
    if (pendingRead_) {
        completePendingRead();
    }
    
    // Get from queue
    std::lock_guard<std::mutex> lock(queueMutex_);
    
    if (completedFrames_.empty()) {
        return false;
    }
    
    frame = std::move(completedFrames_.front());
    completedFrames_.pop();
    
    return true;
}

} // namespace videocomposer

