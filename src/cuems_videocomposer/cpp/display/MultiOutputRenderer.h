/**
 * MultiOutputRenderer.h - Multi-output rendering orchestration
 * 
 * Part of the Multi-Display Implementation for cuems-videocomposer.
 * Manages rendering to multiple physical outputs with layer routing,
 * edge blending, and integration with virtual outputs (NDI, streaming).
 * 
 * Features:
 * - Route specific layers to specific outputs
 * - Edge blending for projector setups
 * - Geometric warping support
 * - Frame capture for virtual outputs
 * - Synchronized multi-output presentation
 */

#ifndef VIDEOCOMPOSER_MULTIOUTPUTRENDERER_H
#define VIDEOCOMPOSER_MULTIOUTPUTRENDERER_H

#include "OutputInfo.h"
#include "OutputConfig.h"
#include "OpenGLRenderer.h"
#include "../layer/LayerManager.h"
#include "../osd/OSDManager.h"
#include "../output/FrameCapture.h"
#include <vector>
#include <memory>
#include <map>
#include <functional>

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
 */
class MultiOutputRenderer {
public:
    MultiOutputRenderer();
    ~MultiOutputRenderer();
    
    // ===== Initialization =====
    
    /**
     * Initialize with list of output surfaces
     * @param surfaces Vector of surfaces to render to (not owned)
     * @return true on success
     */
    bool init(const std::vector<OutputSurface*>& surfaces);
    
    /**
     * Cleanup resources
     */
    void cleanup();
    
    /**
     * Check if initialized
     */
    bool isInitialized() const { return initialized_; }
    
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
        std::vector<LayerRenderInfo> layerMappings;
        BlendRegion blend;
        std::shared_ptr<WarpMesh> warpMesh;
        std::unique_ptr<BlendShader> blendShader;
        bool needsBlending = false;
        bool needsWarping = false;
        bool captureEnabled = false;
    };
    
    std::vector<OutputState> outputs_;
    std::unique_ptr<OpenGLRenderer> renderer_;
    std::map<int, std::vector<int>> layerToOutputs_;  // layerId -> output indices
    
    // Virtual output capture
    std::unique_ptr<FrameCapture> frameCapture_;
    OutputSinkManager* outputSinkManager_ = nullptr;  // Not owned
    GLuint compositeFBO_ = 0;
    GLuint compositeTexture_ = 0;
    GLuint compositeDepthRBO_ = 0;
    int captureWidth_ = 0;
    int captureHeight_ = 0;
    int captureSourceIndex_ = -1;  // -1 = composite FBO
    bool captureEnabled_ = false;
    
    bool initialized_ = false;
    
    // Empty mappings for invalid queries
    static const std::vector<LayerRenderInfo> emptyMappings_;
    
    // ===== Private Methods =====
    
    /**
     * Render layers to a specific output
     */
    void renderLayersToOutput(OutputState& output, LayerManager* layerManager);
    
    /**
     * Apply edge blending
     */
    void applyBlending(OutputState& output);
    
    /**
     * Apply geometric warping
     */
    void applyWarping(OutputState& output);
    
    /**
     * Capture frame for virtual outputs
     */
    void captureForVirtualOutputs();
    
    /**
     * Render to composite FBO
     */
    void renderToCompositeFBO(LayerManager* layerManager);
    
    /**
     * Create composite FBO
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
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_MULTIOUTPUTRENDERER_H

