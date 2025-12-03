/**
 * DRMSurface.h - Per-output rendering surface for DRM/KMS
 * 
 * Part of the Virtual Canvas architecture for cuems-videocomposer.
 * Provides GBM+EGL rendering surface for a single DRM output.
 * Inherits from OutputSurface for compatibility with MultiOutputRenderer.
 * 
 * Features:
 * - GBM surface for buffer allocation
 * - EGL surface for OpenGL rendering
 * - Double-buffered page flipping
 * - Synchronized vsync presentation
 */

#ifndef VIDEOCOMPOSER_DRMSURFACE_H
#define VIDEOCOMPOSER_DRMSURFACE_H

#include "../OutputInfo.h"
#include "../MultiOutputRenderer.h"  // For OutputSurface base class
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <xf86drmMode.h>
#include <vector>
#include <cstdint>

namespace videocomposer {

class DRMOutputManager;

/**
 * DRMSurface - Rendering surface for a single DRM output
 * 
 * Inherits from OutputSurface for use with MultiOutputRenderer.
 * Manages:
 * - GBM surface allocation
 * - EGL context and surface creation
 * - Framebuffer management
 * - Page flipping with vsync
 */
class DRMSurface : public OutputSurface {
public:
    /**
     * Create a surface for a specific output
     * @param outputManager DRM output manager (not owned)
     * @param outputIndex Index of the output to render to
     */
    DRMSurface(DRMOutputManager* outputManager, int outputIndex);
    ~DRMSurface() override;
    
    // ===== Initialization =====
    
    /**
     * Initialize GBM + EGL for this output
     * @param sharedContext Optional EGL context to share resources with
     * @param sharedDisplay Optional shared EGL display (required for context sharing)
     * @param sharedGbmDevice Optional shared GBM device (for resource sharing)
     * @return true on success
     */
    bool init(EGLContext sharedContext = EGL_NO_CONTEXT, 
              EGLDisplay sharedDisplay = EGL_NO_DISPLAY,
              gbm_device* sharedGbmDevice = nullptr);
    
    /**
     * Cleanup all resources
     */
    void cleanup();
    
    /**
     * Reinitialize with new mode/resolution
     * @param width New width
     * @param height New height
     * @return true on success
     */
    bool resize(int width, int height);
    
    /**
     * Check if initialized
     */
    bool isInitialized() const { return initialized_; }
    
    // ===== Rendering =====
    
    /**
     * Begin a new frame
     * Makes this surface's context current
     * @return true on success
     */
    bool beginFrame();
    
    /**
     * End current frame
     * Unlocks front buffer, prepares for swap
     */
    void endFrame();
    
    // ===== Page Flipping =====
    
    /**
     * Schedule page flip (non-blocking)
     * @return true on success
     */
    bool schedulePageFlip();
    
    /**
     * Wait for pending page flip to complete
     * Blocks until flip is done (vsync)
     */
    void waitForFlip();
    
    /**
     * Check if a flip is pending
     */
    bool isFlipPending() const { return flipPending_; }
    
    // ===== Output Information (OutputSurface interface) =====
    
    /**
     * Get output info
     */
    const OutputInfo& getOutputInfo() const override;
    
    /**
     * Get current width
     */
    uint32_t getWidth() const override { return width_; }
    
    /**
     * Get current height
     */
    uint32_t getHeight() const override { return height_; }
    
    /**
     * Get output index
     */
    int getOutputIndex() const { return outputIndex_; }
    
    // ===== EGL/OpenGL Access (OutputSurface interface) =====
    
    /**
     * Make this surface's context current
     */
    void makeCurrent() override;
    
    /**
     * Release context (make none current)
     */
    void releaseCurrent() override;
    
    /**
     * Swap buffers (for non-atomic mode)
     */
    void swapBuffers() override;
    
    /**
     * Get EGL context
     */
    EGLContext getContext() const { return eglContext_; }
    
    /**
     * Get EGL display
     */
    EGLDisplay getDisplay() const { return eglDisplay_; }
    
    /**
     * Get EGL surface
     */
    EGLSurface getSurface() const { return eglSurface_; }
    
    /**
     * Get GBM device
     */
    gbm_device* getGbmDevice() const { return gbmDevice_; }
    
    /**
     * Get GBM surface
     */
    gbm_surface* getGbmSurface() const { return gbmSurface_; }
    
private:
    // Framebuffer info
    struct Framebuffer {
        gbm_bo* bo = nullptr;
        uint32_t fbId = 0;
    };
    
    // Create a DRM framebuffer from GBM buffer
    bool createFramebuffer(gbm_bo* bo, Framebuffer& fb);
    
    // Destroy framebuffer
    void destroyFramebuffer(Framebuffer& fb);
    
    // Page flip handler callback
    static void pageFlipHandler(int fd, unsigned int frame, 
                                unsigned int sec, unsigned int usec, 
                                void* data);
    
    // ===== Members =====
    
    DRMOutputManager* outputManager_;  // Not owned
    int outputIndex_;
    uint32_t width_;
    uint32_t height_;
    
    // GBM
    gbm_device* gbmDevice_ = nullptr;
    gbm_surface* gbmSurface_ = nullptr;
    
    // EGL
    EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
    EGLContext eglContext_ = EGL_NO_CONTEXT;
    EGLSurface eglSurface_ = EGL_NO_SURFACE;
    EGLConfig eglConfig_ = nullptr;
    
    // Framebuffers for page flipping
    Framebuffer currentFb_;
    Framebuffer nextFb_;
    gbm_bo* currentBo_ = nullptr;
    
    // Flip state
    bool flipPending_ = false;
    bool initialized_ = false;
    bool modeSet_ = false;           // True if CRTC mode has been set (initial modeset done)
    
    // Ownership flags (for cleanup)
    bool ownGbmDevice_ = false;      // True if we created the GBM device
    bool ownEglDisplay_ = false;     // True if we created the EGL display
    bool ownEglContext_ = false;     // True if we created the EGL context
    
    // DRM IDs (cached)
    uint32_t connectorId_ = 0;
    uint32_t crtcId_ = 0;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_DRMSURFACE_H

