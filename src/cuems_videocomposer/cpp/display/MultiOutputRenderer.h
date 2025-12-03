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
#include "../output/FrameCapture.h"
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
class WarpMesh;

// BlendShader - Placeholder for edge blending shader (to be implemented)
class BlendShader {
public:
    BlendShader() = default;
    ~BlendShader() = default;
};

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
     * Initialize with list of output surfaces (legacy mode)
     * @param surfaces Vector of surfaces to render to (not owned)
     * @return true on success
     */
    bool init(const std::vector<OutputSurface*>& surfaces);
    
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
    bool isVirtualCanvasMode() const { return useVirtualCanvas_; }
    
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
    
    /**
     * Set layer mapping to output by name
     * @param layerId Layer ID to map
     * @param outputName Output name (e.g., "HDMI-A-1")
     * @param source Source region of video (normalized)
     * @param dest Destination region on output (normalized)
     */
    void setLayerMapping(int layerId, const std::string& outputName,
                        const Rect& source = Rect::fullFrame(),
                        const Rect& dest = Rect::fullFrame());
    
    /**
     * Set layer mapping to output by index
     */
    void setLayerMapping(int layerId, int outputIndex,
                        const Rect& source = Rect::fullFrame(),
                        const Rect& dest = Rect::fullFrame());
    
    /**
     * Clear all mappings for a layer
     */
    void clearLayerMapping(int layerId);
    
    /**
     * Assign layer to all outputs
     */
    void assignLayerToAllOutputs(int layerId);
    
    /**
     * Set opacity for a layer on an output
     */
    void setLayerOpacity(int layerId, int outputIndex, float opacity);
    
    /**
     * Set blend mode for a layer on an output
     */
    void setLayerBlendMode(int layerId, int outputIndex, BlendMode mode);
    
    // ===== Edge Blending =====
    
    /**
     * Set blend region for an output
     */
    void setBlendRegion(int outputIndex, const BlendRegion& blend);
    
    /**
     * Enable/disable blending for an output
     */
    void setBlendEnabled(int outputIndex, bool enabled);
    
    // ===== Geometric Warping =====
    
    /**
     * Set warp mesh for an output
     */
    void setWarpMesh(int outputIndex, std::shared_ptr<WarpMesh> mesh);
    
    /**
     * Enable/disable warping for an output
     */
    void setWarpEnabled(int outputIndex, bool enabled);
    
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
     * Set capture source
     * @param outputIndex Output to capture from, -1 = composite FBO
     */
    void setCaptureSource(int outputIndex);
    
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
     * Render a single output
     * @param outputIndex Output to render
     * @param layerManager Layer manager
     * @param osdManager Optional OSD manager
     */
    void renderOutput(int outputIndex, LayerManager* layerManager,
                      OSDManager* osdManager = nullptr);
    
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
    
    /**
     * Get layer mappings for an output
     */
    const std::vector<LayerRenderInfo>& getLayerMappings(int outputIndex) const;
    
private:
    /**
     * Per-output rendering state
     */
    struct OutputState {
        OutputSurface* surface = nullptr;
        OutputRegion region;                      // Canvas region for this output
        std::vector<LayerRenderInfo> layerMappings;  // Legacy: per-output layer routing
        BlendRegion blend;                        // Legacy: separate blend config
        std::shared_ptr<WarpMesh> warpMesh;
        std::unique_ptr<BlendShader> blendShader;
        bool needsBlending = false;
        bool needsWarping = false;
        bool captureEnabled = false;
    };
    
    // Virtual Canvas components
    std::unique_ptr<VirtualCanvas> canvas_;
    std::unique_ptr<OutputBlitShader> blitShader_;
    bool useVirtualCanvas_ = false;
    
    std::vector<OutputState> outputs_;
    std::vector<OutputRegion> outputRegions_;     // Output regions in canvas
    std::unique_ptr<OpenGLRenderer> renderer_;
    std::map<int, std::vector<int>> layerToOutputs_;  // layerId -> output indices (legacy)
    
    // Virtual output capture
    std::unique_ptr<FrameCapture> frameCapture_;
    OutputSinkManager* outputSinkManager_ = nullptr;  // Not owned
    GLuint compositeFBO_ = 0;
    GLuint compositeTexture_ = 0;
    GLuint compositeDepthRBO_ = 0;
    int captureWidth_ = 0;
    int captureHeight_ = 0;
    int captureSourceIndex_ = -1;  // -1 = virtual canvas
    bool captureEnabled_ = false;
    
    bool initialized_ = false;
    
    // Empty mappings for invalid queries
    static const std::vector<LayerRenderInfo> emptyMappings_;
    
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
     * Render layers to a specific output (legacy mode)
     */
    void renderLayersToOutput(OutputState& output, LayerManager* layerManager);
    
    /**
     * Apply edge blending (legacy mode)
     */
    void applyBlending(OutputState& output);
    
    /**
     * Apply geometric warping (legacy mode)
     */
    void applyWarping(OutputState& output);
    
    /**
     * Capture frame for virtual outputs
     */
    void captureForVirtualOutputs();
    
    /**
     * Render to composite FBO (legacy mode)
     */
    void renderToCompositeFBO(LayerManager* layerManager);
    
    /**
     * Create composite FBO (legacy mode)
     */
    bool createCompositeFBO(int width, int height);
    
    /**
     * Destroy composite FBO
     */
    void destroyCompositeFBO();
    
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

