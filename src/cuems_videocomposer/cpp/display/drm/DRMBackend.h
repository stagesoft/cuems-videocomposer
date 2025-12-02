/**
 * DRMBackend.h - DRM/KMS display backend
 * 
 * Part of the Multi-Display Implementation for cuems-videocomposer.
 * Primary display backend for production use with lowest latency.
 * 
 * Features:
 * - Direct DRM/KMS rendering (no compositor)
 * - Multi-output support
 * - Atomic modesetting
 * - GBM/EGL integration
 * - VAAPI zero-copy support
 */

#ifndef VIDEOCOMPOSER_DRMBACKEND_H
#define VIDEOCOMPOSER_DRMBACKEND_H

#include "../DisplayBackend.h"
#include "../OutputInfo.h"
#include "DRMOutputManager.h"
#include "DRMSurface.h"
#include <vector>
#include <memory>

namespace videocomposer {

// Forward declarations
class OpenGLRenderer;
class LayerManager;
class OSDManager;

/**
 * DRMBackend - DRM/KMS display backend for direct rendering
 * 
 * Provides:
 * - Direct GPU access via DRM/KMS
 * - Multi-output rendering with per-output surfaces
 * - Lowest possible latency (no compositor overhead)
 * - Hardware cursor support (future)
 */
class DRMBackend : public DisplayBackend {
public:
    DRMBackend();
    virtual ~DRMBackend();
    
    // ===== DisplayBackend Interface =====
    
    bool openWindow() override;
    void closeWindow() override;
    bool isWindowOpen() const override;
    
    void render(LayerManager* layerManager, OSDManager* osdManager = nullptr) override;
    void handleEvents() override;
    
    void resize(unsigned int width, unsigned int height) override;
    void getWindowSize(unsigned int* width, unsigned int* height) const override;
    
    void setPosition(int x, int y) override;
    void getWindowPos(int* x, int* y) const override;
    
    void setFullscreen(int action) override;
    bool getFullscreen() const override;
    
    void setOnTop(int action) override;
    bool getOnTop() const override;
    
    bool supportsMultiDisplay() const override { return true; }
    
    void* getContext() override;
    void makeCurrent() override;
    void clearCurrent() override;
    
    OpenGLRenderer* getRenderer() override;
    
#ifdef HAVE_EGL
    EGLDisplay getEGLDisplay() const override;
    bool hasVaapiSupport() const override { return true; }
    PFNEGLCREATEIMAGEKHRPROC getEglCreateImageKHR() const override;
    PFNEGLDESTROYIMAGEKHRPROC getEglDestroyImageKHR() const override;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC getGlEGLImageTargetTexture2DOES() const override;
#endif
    
#ifdef HAVE_VAAPI_INTEROP
    VADisplay getVADisplay() const override;
#endif
    
    // ===== DRM-Specific Methods =====
    
    /**
     * Get all detected outputs
     */
    const std::vector<OutputInfo>& getOutputs() const;
    
    /**
     * Get output count
     */
    size_t getOutputCount() const;
    
    /**
     * Get surface for a specific output
     */
    DRMSurface* getSurface(int index);
    DRMSurface* getSurface(const std::string& name);
    
    /**
     * Get primary surface (first output)
     */
    DRMSurface* getPrimarySurface();
    
    /**
     * Set output mode
     */
    bool setOutputMode(int index, int width, int height, double refresh = 0.0);
    bool setOutputMode(const std::string& name, int width, int height, double refresh = 0.0);
    
    /**
     * Get DRM output manager
     */
    DRMOutputManager* getOutputManager() { return outputManager_.get(); }
    
    /**
     * Set the device path for DRM
     * Must be called before openWindow()
     */
    void setDevicePath(const std::string& path) { devicePath_ = path; }
    
private:
    // DRM management
    std::unique_ptr<DRMOutputManager> outputManager_;
    std::vector<std::unique_ptr<DRMSurface>> surfaces_;
    
    // Rendering
    std::unique_ptr<OpenGLRenderer> renderer_;
    
    // Configuration
    std::string devicePath_;
    int primaryOutput_ = 0;
    
    // State
    bool initialized_ = false;
    bool fullscreen_ = true;  // DRM is always "fullscreen"
    
    // EGL function pointers (for VAAPI interop)
#ifdef HAVE_EGL
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ = nullptr;
#endif
    
#ifdef HAVE_VAAPI_INTEROP
    VADisplay vaDisplay_ = nullptr;
#endif
    
    // Initialize EGL extensions
    void initEGLExtensions();
    
    // Initialize VAAPI
    void initVAAPI();
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_DRMBACKEND_H

