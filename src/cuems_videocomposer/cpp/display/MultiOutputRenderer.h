/**
 * MultiOutputRenderer.h - Multi-output rendering orchestration
 * 
 * Part of the Virtual Canvas architecture for cuems-videocomposer.
 * 
 * Renders all layers to a single VirtualCanvas, then blits regions
 * to physical outputs with optional edge blending and warping.
 * 
 * Features:
 * - Single unified canvas for all content (layers can span outputs)
 * - Edge blending for projector overlap
 * - Geometric warping for keystone/curved surfaces
 * - Frame capture for virtual outputs (NDI, streaming)
 * - Synchronized multi-output presentation
 */

#ifndef VIDEOCOMPOSER_MULTIOUTPUTRENDERER_H
#define VIDEOCOMPOSER_MULTIOUTPUTRENDERER_H

#include "OutputInfo.h"
#include "OutputConfig.h"
#include "OutputRegion.h"
#include "VirtualCanvas.h"
#include "OutputBlitShader.h"
#include "OpenGLRenderer.h"
#include "../layer/LayerManager.h"
#include "../osd/OSDManager.h"
#include <vector>
#include <memory>
#include <map>
#include <functional>

#ifdef HAVE_EGL
#include <EGL/egl.h>
#endif

namespace videocomposer {

// Forward declarations
class OutputSurface;
class OutputSinkManager;

/**
 * Geometry definitions
 */
struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 1.0f;
    float height = 1.0f;
    
    static Rect fullFrame() { return Rect{0.0f, 0.0f, 1.0f, 1.0f}; }
    
    bool operator==(const Rect& other) const {
        return x == other.x && y == other.y && 
               width == other.width && height == other.height;
    }
};

/**
 * Blend mode for layer compositing
 */
enum class BlendMode {
    NORMAL,
    ADDITIVE,
    MULTIPLY,
    SCREEN
};

/**
 * Per-layer rendering information for an output
 */
struct LayerRenderInfo {
    int layerId = -1;
    Rect sourceRegion = Rect::fullFrame();  // Region of video to show
    Rect destRegion = Rect::fullFrame();     // Where on output to render
    float opacity = 1.0f;
    BlendMode blendMode = BlendMode::NORMAL;
};

/**
 * Abstract base class for output surfaces
 * Implemented by DRMSurface, WaylandSurface, X11Surface, etc.
 */
class OutputSurface {
public:
    virtual ~OutputSurface() = default;
    
    virtual void makeCurrent() = 0;
    virtual void releaseCurrent() = 0;
    virtual void swapBuffers() = 0;
    
    // Schedule page flip (DRM-specific, no-op for X11/Wayland)
    // Called immediately after swapBuffers to minimize latency
    virtual bool schedulePageFlip() { return true; }
    
    virtual uint32_t getWidth() const = 0;
    virtual uint32_t getHeight() const = 0;
    virtual const OutputInfo& getOutputInfo() const = 0;
};

/**
 * MultiOutputRenderer - Orchestrates rendering to multiple displays
 * 
 * Uses VirtualCanvas architecture:
 * 1. All layers render to a single FBO (VirtualCanvas)
 * 2. Regions are blitted to each output with blend/warp via OutputBlitShader
 */
class MultiOutputRenderer {
public:
    MultiOutputRenderer();
    ~MultiOutputRenderer();
    
    // ===== Initialization =====
    
    /**
     * Initialize with EGL context for Virtual Canvas mode
     * This is the preferred initialization method for DRM/KMS.
     * 
     * @param eglDisplay EGL display
     * @param eglContext Shared EGL context (must be current when calling GL functions)
     * @return true on success
     */
#ifdef HAVE_EGL
    bool init(EGLDisplay eglDisplay, EGLContext eglContext);
#endif
    
    /**
     * Configure output regions for Virtual Canvas mode
     * Call after init() to set up output layout.
     * 
     * @param regions Vector of output regions defining canvas layout
     * @param surfaces Corresponding output surfaces (not owned)
     */
    void configureOutputs(const std::vector<OutputRegion>& regions,
                          const std::vector<OutputSurface*>& surfaces);
    
    /**
     * Reconfigure canvas size (call when outputs change)
     * Automatically calculates combined size from output regions.
     */
    void reconfigureCanvas();
    
    /**
     * Cleanup resources
     */
    void cleanup();
    
    /**
     * Check if initialized
     */
    bool isInitialized() const { return initialized_; }
    
    /**
     * Check if using Virtual Canvas mode
     */
    bool isVirtualCanvasMode() const { return true; }
    
    // ===== Virtual Canvas Access =====
    
    /**
     * Get the virtual canvas (for direct access if needed)
     */
    VirtualCanvas* getCanvas() { return canvas_.get(); }
    const VirtualCanvas* getCanvas() const { return canvas_.get(); }
    
    /**
     * Get canvas dimensions
     */
    int getCanvasWidth() const { return canvas_ ? canvas_->getWidth() : 0; }
    int getCanvasHeight() const { return canvas_ ? canvas_->getHeight() : 0; }
    
    // ===== Layer Mapping =====
    
    // ===== Virtual Output Integration =====
    
    /**
     * Set the output sink manager for virtual outputs
     */
    void setOutputSinkManager(OutputSinkManager* sinkManager);
    
    /**
     * Enable/disable frame capture for virtual outputs
     */
    void setCaptureEnabled(bool enabled);
    
    /**
     * Check if capture is enabled
     */
    bool isCaptureEnabled() const { return captureEnabled_; }
    
    /**
     * Set capture resolution
     */
    void setCaptureResolution(int width, int height);
    
    // ===== Rendering =====
    
    /**
     * Render all outputs
     * @param layerManager Layer manager with video layers
     * @param osdManager Optional OSD manager
     */
    void render(LayerManager* layerManager, OSDManager* osdManager = nullptr);
    
    /**
     * Present all outputs (synchronized swap/flip)
     */
    void presentAll();
    
    // ===== Query =====
    
    /**
     * Get output count
     */
    size_t getOutputCount() const { return outputs_.size(); }
    
    /**
     * Get output info by index
     */
    const OutputInfo* getOutputInfo(int index) const;
    
private:
    /**
     * Per-output rendering state
     */
    struct OutputState {
        OutputSurface* surface = nullptr;
        OutputRegion region;                      // Canvas region for this output
    };
    
    // Virtual Canvas components
    std::unique_ptr<VirtualCanvas> canvas_;
    std::unique_ptr<OutputBlitShader> blitShader_;
    
    std::vector<OutputState> outputs_;
    std::vector<OutputRegion> outputRegions_;     // Output regions in canvas
    std::unique_ptr<OpenGLRenderer> renderer_;
    
    // Virtual output capture
    OutputSinkManager* outputSinkManager_ = nullptr;  // Not owned
    int captureWidth_ = 0;
    int captureHeight_ = 0;
    bool captureEnabled_ = false;
    
    bool initialized_ = false;
    
    // ===== Private Methods =====
    
    /**
     * Render all layers to the virtual canvas
     */
    void renderToCanvas(LayerManager* layerManager, OSDManager* osdManager);
    
    /**
     * Blit canvas regions to all outputs
     */
    void blitToOutputs();
    
    /**
     * Blit canvas region to a single output
     */
    void blitToOutput(OutputState& output);
    
    /**
     * Capture frame for virtual outputs
     */
    void captureForVirtualOutputs();
    
    /**
     * Find output by name
     */
    int findOutputByName(const std::string& name) const;
    
    /**
     * Calculate canvas size from output regions
     */
    void calculateCanvasSize(int& width, int& height) const;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_MULTIOUTPUTRENDERER_H

