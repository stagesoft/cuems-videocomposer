/**
 * HeadlessDisplay.h - Headless rendering backend for servers
 * 
 * Part of the Multi-Display Implementation for cuems-videocomposer.
 * Provides EGL+GBM rendering without a physical display, suitable for:
 * - Server deployments
 * - NDI-only output
 * - Batch processing
 * - Testing
 * 
 * Uses EGL surfaceless context or pbuffer for offscreen rendering.
 */

#ifndef VIDEOCOMPOSER_HEADLESSDISPLAY_H
#define VIDEOCOMPOSER_HEADLESSDISPLAY_H

#include "DisplayBackend.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <memory>

// OpenGL type definition (avoid including full GL headers here)
#ifndef GL_VERSION_1_0
typedef unsigned int GLuint;
#endif

// Forward declarations
struct gbm_device;

namespace videocomposer {

class OpenGLRenderer;
class FrameCapture;

/**
 * HeadlessDisplay - Offscreen rendering without a display
 * 
 * Implements DisplayBackend for headless operation:
 * - Uses EGL with GBM for GPU rendering
 * - Renders to FBO for capture
 * - No window management
 */
class HeadlessDisplay : public DisplayBackend {
public:
    HeadlessDisplay();
    virtual ~HeadlessDisplay();
    
    // ===== DisplayBackend Interface =====
    
    bool openWindow() override;
    void closeWindow() override;
    bool isWindowOpen() const override { return initialized_; }
    
    void render(LayerManager* layerManager, OSDManager* osdManager = nullptr) override;
    void handleEvents() override {}  // No events in headless mode
    
    void resize(unsigned int width, unsigned int height) override;
    void getWindowSize(unsigned int* width, unsigned int* height) const override;
    
    void setPosition(int x, int y) override { (void)x; (void)y; }
    void getWindowPos(int* x, int* y) const override { *x = 0; *y = 0; }
    
    void setFullscreen(int action) override { (void)action; }
    bool getFullscreen() const override { return true; }
    
    void setOnTop(int action) override { (void)action; }
    bool getOnTop() const override { return false; }
    
    bool supportsMultiDisplay() const override { return false; }
    
    void* getContext() override;
    void makeCurrent() override;
    void clearCurrent() override;
    
    OpenGLRenderer* getRenderer() override;
    
#ifdef HAVE_EGL
    EGLDisplay getEGLDisplay() const override { return eglDisplay_; }
    bool hasVaapiSupport() const override;
    PFNEGLCREATEIMAGEKHRPROC getEglCreateImageKHR() const override { return eglCreateImageKHR_; }
    PFNEGLDESTROYIMAGEKHRPROC getEglDestroyImageKHR() const override { return eglDestroyImageKHR_; }
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC getGlEGLImageTargetTexture2DOES() const override { return glEGLImageTargetTexture2DOES_; }
#endif
    
#ifdef HAVE_VAAPI_INTEROP
    VADisplay getVADisplay() const override { return vaDisplay_; }
#endif
    
    // ===== Headless-Specific Methods =====
    
    /**
     * Get the offscreen FBO ID
     */
    GLuint getOffscreenFBO() const { return offscreenFBO_; }
    
    /**
     * Get the offscreen texture ID
     */
    GLuint getOffscreenTexture() const { return offscreenTexture_; }
    
    /**
     * Set render dimensions
     * Call before openWindow() to set initial size
     */
    void setDimensions(int width, int height) {
        width_ = width;
        height_ = height;
    }
    
    /**
     * Get frame capture helper
     */
    FrameCapture* getFrameCapture() { return frameCapture_.get(); }
    
private:
    // DRM/GBM
    int drmFd_ = -1;
    gbm_device* gbmDevice_ = nullptr;
    
    // EGL
    EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
    EGLContext eglContext_ = EGL_NO_CONTEXT;
    EGLSurface eglSurface_ = EGL_NO_SURFACE;  // Surfaceless or pbuffer
    EGLConfig eglConfig_ = nullptr;
    
    // Offscreen FBO
    GLuint offscreenFBO_ = 0;
    GLuint offscreenTexture_ = 0;
    GLuint depthRBO_ = 0;
    
    // Dimensions
    int width_ = 1920;
    int height_ = 1080;
    
    // State
    bool initialized_ = false;
    
    // Renderer
    std::unique_ptr<OpenGLRenderer> renderer_;
    std::unique_ptr<FrameCapture> frameCapture_;
    
    // EGL extension function pointers
#ifdef HAVE_EGL
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ = nullptr;
#endif
    
#ifdef HAVE_VAAPI_INTEROP
    VADisplay vaDisplay_ = nullptr;
#endif
    
    // ===== Private Methods =====
    
    /**
     * Initialize DRM device for GBM
     */
    bool initDRM();
    
    /**
     * Initialize EGL on GBM device
     */
    bool initEGL();
    
    /**
     * Create offscreen rendering surface (FBO)
     */
    bool createOffscreenSurface(int width, int height);
    
    /**
     * Destroy offscreen surface
     */
    void destroyOffscreenSurface();
    
    /**
     * Initialize EGL extension function pointers
     */
    void initEGLExtensions();
    
    /**
     * Initialize VAAPI
     */
    void initVAAPI();
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_HEADLESSDISPLAY_H

