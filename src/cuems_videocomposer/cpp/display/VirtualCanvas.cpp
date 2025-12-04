/**
 * VirtualCanvas.cpp - Unified rendering target implementation
 */

#include "VirtualCanvas.h"
#include "../utils/Logger.h"

#include <GL/glew.h>  // Must be included before GL/gl.h
#include <GL/gl.h>
#include <cstring>

namespace videocomposer {

VirtualCanvas::VirtualCanvas() {
}

VirtualCanvas::~VirtualCanvas() {
    cleanup();
}

#ifdef HAVE_EGL
bool VirtualCanvas::init(EGLDisplay eglDisplay, EGLContext eglContext) {
    if (initialized_) {
        LOG_WARNING << "VirtualCanvas: Already initialized";
        return true;
    }
    
    eglDisplay_ = eglDisplay;
    eglContext_ = eglContext;
    
    // Validate EGL context
    if (eglDisplay_ == EGL_NO_DISPLAY) {
        LOG_WARNING << "VirtualCanvas: No EGL display provided, proceeding anyway";
    }
    
    initialized_ = true;
    LOG_INFO << "VirtualCanvas: Initialized";
    
    return true;
}
#else
bool VirtualCanvas::init(void* display, void* context) {
    (void)display;
    (void)context;
    
    if (initialized_) {
        LOG_WARNING << "VirtualCanvas: Already initialized";
        return true;
    }
    
    initialized_ = true;
    LOG_INFO << "VirtualCanvas: Initialized (no EGL)";
    
    return true;
}
#endif

void VirtualCanvas::cleanup() {
    if (rendering_) {
        endFrame();
    }
    
    destroyPBOs();
    destroyFBO();
    
    width_ = 0;
    height_ = 0;
    initialized_ = false;
    
    LOG_INFO << "VirtualCanvas: Cleaned up";
}

bool VirtualCanvas::configure(int width, int height) {
    if (!initialized_) {
        LOG_ERROR << "VirtualCanvas: Not initialized";
        return false;
    }
    
    if (width <= 0 || height <= 0) {
        LOG_ERROR << "VirtualCanvas: Invalid dimensions " << width << "x" << height;
        return false;
    }
    
    // Check if resize is needed
    if (width == width_ && height == height_ && fbo_ != 0) {
        return true;  // No change needed
    }
    
    LOG_INFO << "VirtualCanvas: Configuring " << width << "x" << height;
    
    // End current frame if rendering
    if (rendering_) {
        endFrame();
    }
    
    // Create/recreate FBO
    if (!createFBO(width, height)) {
        LOG_ERROR << "VirtualCanvas: Failed to create FBO";
        return false;
    }
    
    // Recreate PBOs if they exist
    if (pboInitialized_) {
        destroyPBOs();
        initPBOs(width, height);
    }
    
    width_ = width;
    height_ = height;
    
    LOG_INFO << "VirtualCanvas: Configured " << width_ << "x" << height_;
    
    return true;
}

void VirtualCanvas::beginFrame() {
    if (!initialized_ || fbo_ == 0) {
        LOG_ERROR << "VirtualCanvas: Cannot begin frame - not configured";
        return;
    }
    
    if (rendering_) {
        LOG_WARNING << "VirtualCanvas: beginFrame called while already rendering";
        return;
    }
    
    // Bind our FBO
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    
    // Set viewport to full canvas
    glViewport(0, 0, width_, height_);
    
    // Clear to black
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    rendering_ = true;
}

void VirtualCanvas::endFrame() {
    if (!rendering_) {
        return;
    }
    
    // Flush GL commands (glFinish not needed - waitForFlip provides sync)
    glFlush();
    
    // Unbind FBO (restore default framebuffer)
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    rendering_ = false;
}

bool VirtualCanvas::captureFrame(void* buffer, size_t bufferSize) {
    return captureRegion(0, 0, width_, height_, buffer, bufferSize);
}

bool VirtualCanvas::captureRegion(int x, int y, int width, int height,
                                   void* buffer, size_t bufferSize) {
    if (!initialized_ || fbo_ == 0) {
        LOG_ERROR << "VirtualCanvas: Cannot capture - not configured";
        return false;
    }
    
    if (!buffer || bufferSize < static_cast<size_t>(width * height * 4)) {
        LOG_ERROR << "VirtualCanvas: Invalid buffer for capture";
        return false;
    }
    
    // Validate region
    if (x < 0 || y < 0 || x + width > width_ || y + height > height_) {
        LOG_ERROR << "VirtualCanvas: Capture region out of bounds";
        return false;
    }
    
    // Bind FBO for reading
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    
    // Read pixels (synchronous - use async methods for better performance)
    glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, buffer);
    
    // Unbind
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    
    return true;
}

void VirtualCanvas::startAsyncCapture() {
    if (!initialized_ || fbo_ == 0) {
        LOG_ERROR << "VirtualCanvas: Cannot start async capture - not configured";
        return;
    }
    
    // Initialize PBOs if needed
    if (!pboInitialized_) {
        if (!initPBOs(width_, height_)) {
            LOG_ERROR << "VirtualCanvas: Failed to initialize PBOs for async capture";
            return;
        }
    }
    
    // Swap PBO index
    currentPbo_ = (currentPbo_ + 1) % 2;
    
    // Bind FBO for reading
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    
    // Bind PBO for pixel pack
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[currentPbo_]);
    
    // Start async read (returns immediately)
    glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    
    // Unbind
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    
    asyncCaptureInProgress_ = true;
    asyncCaptureWidth_ = width_;
    asyncCaptureHeight_ = height_;
}

bool VirtualCanvas::isAsyncCaptureReady() const {
    if (!asyncCaptureInProgress_) {
        return false;
    }
    
    // Check if the previous PBO transfer is complete
    // (We read from the previous PBO while the current one is being filled)
    int prevPbo = (currentPbo_ + 1) % 2;
    
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[prevPbo]);
    
    // Try to map - if it succeeds, data is ready
    void* ptr = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    if (ptr) {
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return true;
    }
    
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return false;
}

bool VirtualCanvas::getAsyncCaptureResult(void* buffer, size_t bufferSize) {
    if (!asyncCaptureInProgress_) {
        return false;
    }
    
    size_t requiredSize = static_cast<size_t>(asyncCaptureWidth_ * asyncCaptureHeight_ * 4);
    if (!buffer || bufferSize < requiredSize) {
        LOG_ERROR << "VirtualCanvas: Invalid buffer for async capture result";
        return false;
    }
    
    // Read from the previous PBO (double-buffering)
    int prevPbo = (currentPbo_ + 1) % 2;
    
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[prevPbo]);
    
    void* ptr = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    if (!ptr) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return false;
    }
    
    // Copy data
    memcpy(buffer, ptr, requiredSize);
    
    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    
    return true;
}

bool VirtualCanvas::createFBO(int width, int height) {
    // Destroy existing FBO if any
    destroyFBO();
    
    // Create texture
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    
    // Create depth renderbuffer
    glGenRenderbuffers(1, &depthRbo_);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRbo_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    
    // Create FBO
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    
    // Attach texture as color buffer
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, texture_, 0);
    
    // Attach depth buffer
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, depthRbo_);
    
    // Check completeness
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR << "VirtualCanvas: FBO incomplete, status=" << status;
        destroyFBO();
        return false;
    }
    
    LOG_INFO << "VirtualCanvas: Created FBO " << width << "x" << height
             << " (texture=" << texture_ << ", fbo=" << fbo_ << ")";
    
    return true;
}

void VirtualCanvas::destroyFBO() {
    if (fbo_ != 0) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0;
    }
    
    if (texture_ != 0) {
        glDeleteTextures(1, &texture_);
        texture_ = 0;
    }
    
    if (depthRbo_ != 0) {
        glDeleteRenderbuffers(1, &depthRbo_);
        depthRbo_ = 0;
    }
}

bool VirtualCanvas::initPBOs(int width, int height) {
    if (pboInitialized_) {
        return true;
    }
    
    size_t bufferSize = static_cast<size_t>(width * height * 4);
    
    glGenBuffers(2, pbo_);
    
    for (int i = 0; i < 2; ++i) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, bufferSize, nullptr, GL_STREAM_READ);
    }
    
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    
    pboInitialized_ = true;
    currentPbo_ = 0;
    
    LOG_INFO << "VirtualCanvas: Initialized PBOs for async capture (" 
             << bufferSize << " bytes each)";
    
    return true;
}

void VirtualCanvas::destroyPBOs() {
    if (!pboInitialized_) {
        return;
    }
    
    glDeleteBuffers(2, pbo_);
    pbo_[0] = 0;
    pbo_[1] = 0;
    
    pboInitialized_ = false;
    asyncCaptureInProgress_ = false;
}

} // namespace videocomposer

