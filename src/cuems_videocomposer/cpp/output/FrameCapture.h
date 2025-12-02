/**
 * FrameCapture.h - Async GPU frame capture using PBO
 * 
 * Shared component for Multi-Display and NDI implementations.
 * Provides efficient asynchronous GPU-to-CPU frame capture using
 * OpenGL Pixel Buffer Objects (PBO) for double-buffered transfers.
 * 
 * Features:
 * - Double-buffered PBO for async transfers
 * - Non-blocking capture initiation
 * - Configurable capture source (FBO or default framebuffer)
 * - Multiple pixel format support
 */

#ifndef VIDEOCOMPOSER_FRAMECAPTURE_H
#define VIDEOCOMPOSER_FRAMECAPTURE_H

#include "../video/FrameFormat.h"
#include <GL/glew.h>
#include <queue>
#include <mutex>
#include <cstdint>
#include <memory>

namespace videocomposer {

/**
 * Captured frame data
 */
struct FrameData {
    uint8_t* data = nullptr;
    size_t size = 0;
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::RGBA32;
    int64_t timestamp = 0;      // Timestamp in microseconds
    int64_t frameNumber = 0;    // Sequential frame number
    bool ownsData = false;      // If true, destructor frees data
    
    FrameData() = default;
    
    FrameData(FrameData&& other) noexcept
        : data(other.data), size(other.size), width(other.width),
          height(other.height), format(other.format), timestamp(other.timestamp),
          frameNumber(other.frameNumber), ownsData(other.ownsData) {
        other.data = nullptr;
        other.ownsData = false;
    }
    
    FrameData& operator=(FrameData&& other) noexcept {
        if (this != &other) {
            if (ownsData && data) {
                delete[] data;
            }
            data = other.data;
            size = other.size;
            width = other.width;
            height = other.height;
            format = other.format;
            timestamp = other.timestamp;
            frameNumber = other.frameNumber;
            ownsData = other.ownsData;
            other.data = nullptr;
            other.ownsData = false;
        }
        return *this;
    }
    
    ~FrameData() {
        if (ownsData && data) {
            delete[] data;
        }
    }
    
    // Disable copy
    FrameData(const FrameData&) = delete;
    FrameData& operator=(const FrameData&) = delete;
    
    /**
     * Calculate bytes per pixel for format
     */
    static int bytesPerPixel(PixelFormat fmt) {
        switch (fmt) {
            case PixelFormat::RGBA32:
            case PixelFormat::BGRA32:
                return 4;
            case PixelFormat::RGB24:
                return 3;
            case PixelFormat::YUV420P:
                return 1;  // Actually 1.5, handled specially
            case PixelFormat::UYVY422:
                return 2;
        }
        return 4;
    }
};

/**
 * FrameCapture - Async frame capture with PBO double-buffering
 */
class FrameCapture {
public:
    FrameCapture();
    ~FrameCapture();
    
    // ===== Initialization =====
    
    /**
     * Initialize capture with dimensions
     * @param width Capture width
     * @param height Capture height
     * @param format Pixel format (default RGBA)
     * @return true on success
     */
    bool initialize(int width, int height, PixelFormat format = PixelFormat::RGBA32);
    
    /**
     * Cleanup resources
     */
    void cleanup();
    
    /**
     * Check if initialized
     */
    bool isInitialized() const { return initialized_; }
    
    // ===== Capture Operations =====
    
    /**
     * Start async capture (non-blocking)
     * Call this after rendering, before swapBuffers
     * Initiates a GPU-to-PBO transfer
     */
    void startCapture();
    
    /**
     * Get completed frame (from previous capture)
     * @param frame Output frame data
     * @return true if a completed frame is available
     */
    bool getCompletedFrame(FrameData& frame);
    
    /**
     * Check if there's a pending capture
     */
    bool hasPendingCapture() const { return pendingRead_; }
    
    // ===== Configuration =====
    
    /**
     * Set source FBO to capture from
     * @param fbo FBO ID (0 = default framebuffer)
     */
    void setSourceFBO(GLuint fbo) { sourceFBO_ = fbo; }
    
    /**
     * Set read buffer
     * @param buffer GL_FRONT, GL_BACK, etc.
     */
    void setReadBuffer(GLenum buffer) { readBuffer_ = buffer; }
    
    /**
     * Set source rectangle within the framebuffer
     * @param x Source X offset
     * @param y Source Y offset
     */
    void setSourceOffset(int x, int y) { sourceX_ = x; sourceY_ = y; }
    
    // ===== Query =====
    
    /**
     * Check if PBO extension is supported
     */
    bool hasPBOSupport() const { return pboSupported_; }
    
    /**
     * Get capture dimensions
     */
    int getWidth() const { return width_; }
    int getHeight() const { return height_; }
    
    /**
     * Get format
     */
    PixelFormat getFormat() const { return format_; }
    
private:
    // PBO buffer indices
    static const int NUM_PBOS = 2;
    GLuint pbo_[NUM_PBOS] = {0, 0};
    int currentPBO_ = 0;
    int pendingPBO_ = -1;
    
    // Capture parameters
    int width_ = 0;
    int height_ = 0;
    PixelFormat format_ = PixelFormat::RGBA32;
    size_t frameSize_ = 0;
    
    // Source configuration
    GLuint sourceFBO_ = 0;
    GLenum readBuffer_ = GL_BACK;
    int sourceX_ = 0;
    int sourceY_ = 0;
    
    // State
    bool pboSupported_ = false;
    bool initialized_ = false;
    bool pendingRead_ = false;
    int64_t frameNumber_ = 0;
    
    // Completed frames queue
    std::queue<FrameData> completedFrames_;
    std::mutex queueMutex_;
    
    // ===== Private Methods =====
    
    /**
     * Check for PBO extension support
     */
    void checkPBOSupport();
    
    /**
     * Get GL format for our pixel format
     */
    GLenum getGLFormat() const;
    
    /**
     * Get GL type for our pixel format
     */
    GLenum getGLType() const;
    
    /**
     * Get current timestamp in microseconds
     */
    int64_t getCurrentTimestamp() const;
    
    /**
     * Complete a pending read (map PBO and copy data)
     */
    bool completePendingRead();
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_FRAMECAPTURE_H

