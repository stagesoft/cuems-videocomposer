/**
 * DRMBackend.h - DRM/KMS display backend
 * 
 * Part of the Virtual Canvas architecture for cuems-videocomposer.
 * Primary display backend for production use with lowest latency.
 * 
 * Features:
 * - Direct DRM/KMS rendering (no compositor)
 * - Multi-output support with Virtual Canvas
 * - Edge blending and warping for projection mapping
 * - Atomic modesetting
 * - GBM/EGL integration
 * - VAAPI zero-copy support
 */

#ifndef VIDEOCOMPOSER_DRMBACKEND_H
#define VIDEOCOMPOSER_DRMBACKEND_H

#include "../DisplayBackend.h"
#include "../OutputInfo.h"
#include "../OutputRegion.h"
#include "../MultiOutputRenderer.h"
#include "DRMOutputManager.h"
#include "DRMSurface.h"
#include <vector>
#include <memory>

namespace videocomposer {

// Forward declarations
class OpenGLRenderer;
class LayerManager;
class OSDManager;
class VirtualCanvas;
class OutputBlitShader;

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
    PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC getGlEGLImageTargetTexStorageEXT() const override;
    bool isDesktopGL() const override { return true; }  // DRM/KMS always uses Desktop GL
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
    
    // ===== Virtual Canvas Mode =====
    
    /**
     * Enable/disable Virtual Canvas mode
     * Must be called before openWindow() or will take effect on next open.
     * 
     * Virtual Canvas mode:
     * - All layers render to a single canvas FBO
     * - Regions are blitted to outputs with blend/warp
     * - Supports layers spanning multiple outputs
     * - Required for projection mapping
     */
    void setVirtualCanvasMode(bool enabled) { useVirtualCanvas_ = enabled; }
    bool isVirtualCanvasMode() const { return useVirtualCanvas_; }
    
    /**
     * Get the multi-output renderer (for configuring output regions)
     */
    MultiOutputRenderer* getMultiOutputRenderer() { return multiRenderer_.get(); }
    
    /**
     * Configure output region in the virtual canvas
     * @param outputName Output name (e.g., "HDMI-A-1")
     * @param region Output region configuration
     */
    bool configureOutputRegion(const std::string& outputName, const OutputRegion& region);
    
    /**
     * Auto-configure output regions based on detected outputs
     * Arranges outputs side-by-side with optional overlap for blending.
     * 
     * @param arrangement "horizontal", "vertical", or "grid"
     * @param overlap Overlap in pixels (for edge blending)
     */
    void autoConfigureOutputs(const std::string& arrangement = "horizontal", int overlap = 0);
    
    /**
     * Get output regions
     */
    const std::vector<OutputRegion>& getOutputRegions() const { return outputRegions_; }
    
private:
    // DRM management
    std::unique_ptr<DRMOutputManager> outputManager_;
    std::vector<std::unique_ptr<DRMSurface>> surfaces_;
    
    // Rendering - Legacy mode (per-output)
    std::unique_ptr<OpenGLRenderer> renderer_;
    
    // Rendering - Virtual Canvas mode
    std::unique_ptr<MultiOutputRenderer> multiRenderer_;
    std::vector<OutputRegion> outputRegions_;
    bool useVirtualCanvas_ = true;  // Default to Virtual Canvas mode
    
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
    PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC glEGLImageTargetTexStorageEXT_ = nullptr;
#endif
    
#ifdef HAVE_VAAPI_INTEROP
    VADisplay vaDisplay_ = nullptr;
#endif
    
    // Initialize EGL extensions
    void initEGLExtensions();
    
    // Initialize VAAPI
    void initVAAPI();
    
    // Initialize Virtual Canvas mode
    bool initVirtualCanvas();
    
    // Render using Virtual Canvas
    void renderVirtualCanvas(LayerManager* layerManager, OSDManager* osdManager);
    
    // Render using legacy per-output mode
    void renderLegacy(LayerManager* layerManager, OSDManager* osdManager);
    
    // Build output regions from detected outputs
    void buildOutputRegions();
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_DRMBACKEND_H

