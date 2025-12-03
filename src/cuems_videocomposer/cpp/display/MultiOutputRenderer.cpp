/**
 * MultiOutputRenderer.cpp - Multi-output rendering implementation
 * 
 * Uses Virtual Canvas architecture: all layers render to a single FBO,
 * then regions are blitted to physical outputs with blend/warp.
 */

#include "MultiOutputRenderer.h"
#include "../output/FrameCapture.h"
#include "../output/OutputSinkManager.h"
#include "../layer/LayerManager.h"
#include "../utils/Logger.h"

#include <GL/glew.h>
#include <GL/gl.h>
#include <cstring>
#include <algorithm>

namespace videocomposer {

// Static empty mappings
const std::vector<LayerRenderInfo> MultiOutputRenderer::emptyMappings_;

MultiOutputRenderer::MultiOutputRenderer() {
}

MultiOutputRenderer::~MultiOutputRenderer() {
    cleanup();
}

#ifdef HAVE_EGL
bool MultiOutputRenderer::init(EGLDisplay eglDisplay, EGLContext eglContext) {
    if (initialized_) {
        LOG_WARNING << "MultiOutputRenderer: Already initialized";
        return true;
    }
    
    // Create Virtual Canvas
    canvas_ = std::make_unique<VirtualCanvas>();
    if (!canvas_->init(eglDisplay, eglContext)) {
        LOG_ERROR << "MultiOutputRenderer: Failed to initialize VirtualCanvas";
        return false;
    }
    
    // Create OpenGL renderer for layer compositing
    renderer_ = std::make_unique<OpenGLRenderer>();
    if (!renderer_->init()) {
        LOG_ERROR << "MultiOutputRenderer: Failed to initialize OpenGLRenderer";
        canvas_.reset();
        return false;
    }
    
    // Create blit shader for output rendering
    blitShader_ = std::make_unique<OutputBlitShader>();
    if (!blitShader_->init()) {
        LOG_ERROR << "MultiOutputRenderer: Failed to initialize OutputBlitShader";
        renderer_.reset();
        canvas_.reset();
        return false;
    }
    
    useVirtualCanvas_ = true;
    initialized_ = true;
    
    LOG_INFO << "MultiOutputRenderer: Initialized with Virtual Canvas mode";
    return true;
}
#endif

bool MultiOutputRenderer::init(const std::vector<OutputSurface*>& surfaces) {
    if (surfaces.empty()) {
        LOG_WARNING << "MultiOutputRenderer: No surfaces provided";
        return false;
    }
    
    outputs_.clear();
    layerToOutputs_.clear();
    
    // Create output state for each surface
    for (size_t i = 0; i < surfaces.size(); ++i) {
        OutputState state;
        state.surface = surfaces[i];
        
        // Create default region from surface info
        const OutputInfo& info = surfaces[i]->getOutputInfo();
        state.region = OutputRegion::createDefault(
            info.name, static_cast<int>(i),
            info.width, info.height, info.x, info.y
        );
        
        outputs_.push_back(std::move(state));
        
        LOG_INFO << "MultiOutputRenderer: Added output " << i
                 << " (" << surfaces[i]->getOutputInfo().name << ")";
    }
    
    // Create renderer (using first surface's context)
    if (!outputs_.empty() && outputs_[0].surface) {
        outputs_[0].surface->makeCurrent();
        
        renderer_ = std::make_unique<OpenGLRenderer>();
        if (!renderer_->init()) {
            LOG_WARNING << "MultiOutputRenderer: OpenGLRenderer init failed";
        }
        
        outputs_[0].surface->releaseCurrent();
    }
    
    useVirtualCanvas_ = false;  // Legacy mode
    initialized_ = true;
    LOG_INFO << "MultiOutputRenderer: Initialized with " << outputs_.size() << " outputs (legacy mode)";
    
    return true;
}

void MultiOutputRenderer::configureOutputs(const std::vector<OutputRegion>& regions,
                                           const std::vector<OutputSurface*>& surfaces) {
    if (regions.size() != surfaces.size()) {
        LOG_ERROR << "MultiOutputRenderer: Region/surface count mismatch";
        return;
    }
    
    outputs_.clear();
    outputRegions_ = regions;
    
    for (size_t i = 0; i < surfaces.size(); ++i) {
        OutputState state;
        state.surface = surfaces[i];
        state.region = regions[i];
        outputs_.push_back(std::move(state));
        
        LOG_INFO << "MultiOutputRenderer: Configured output " << i
                 << " (" << regions[i].name << ")"
                 << " canvas: " << regions[i].canvasX << "," << regions[i].canvasY
                 << " " << regions[i].canvasWidth << "x" << regions[i].canvasHeight;
    }
    
    // Reconfigure canvas size
    reconfigureCanvas();
}

void MultiOutputRenderer::reconfigureCanvas() {
    if (!useVirtualCanvas_ || !canvas_) {
        return;
    }
    
    int width, height;
    calculateCanvasSize(width, height);
    
    if (width > 0 && height > 0) {
        canvas_->configure(width, height);
        LOG_INFO << "MultiOutputRenderer: Canvas configured to " << width << "x" << height;
    }
}

void MultiOutputRenderer::calculateCanvasSize(int& width, int& height) const {
    width = 0;
    height = 0;
    
    for (const auto& output : outputs_) {
        int right = output.region.canvasX + output.region.canvasWidth;
        int bottom = output.region.canvasY + output.region.canvasHeight;
        
        if (right > width) width = right;
        if (bottom > height) height = bottom;
    }
}

void MultiOutputRenderer::cleanup() {
    destroyCompositeFBO();
    
    frameCapture_.reset();
    blitShader_.reset();
    renderer_.reset();
    canvas_.reset();
    
    outputs_.clear();
    outputRegions_.clear();
    layerToOutputs_.clear();
    
    useVirtualCanvas_ = false;
    initialized_ = false;
}

void MultiOutputRenderer::setLayerMapping(int layerId, const std::string& outputName,
                                          const Rect& source, const Rect& dest) {
    int index = findOutputByName(outputName);
    if (index >= 0) {
        setLayerMapping(layerId, index, source, dest);
    } else {
        LOG_WARNING << "MultiOutputRenderer: Output not found: " << outputName;
    }
}

void MultiOutputRenderer::setLayerMapping(int layerId, int outputIndex,
                                          const Rect& source, const Rect& dest) {
    if (outputIndex < 0 || outputIndex >= static_cast<int>(outputs_.size())) {
        LOG_WARNING << "MultiOutputRenderer: Invalid output index: " << outputIndex;
        return;
    }
    
    auto& output = outputs_[outputIndex];
    
    // Check if mapping already exists
    for (auto& mapping : output.layerMappings) {
        if (mapping.layerId == layerId) {
            mapping.sourceRegion = source;
            mapping.destRegion = dest;
            return;
        }
    }
    
    // Add new mapping
    LayerRenderInfo info;
    info.layerId = layerId;
    info.sourceRegion = source;
    info.destRegion = dest;
    output.layerMappings.push_back(info);
    
    // Update layer-to-outputs map
    auto& outputList = layerToOutputs_[layerId];
    if (std::find(outputList.begin(), outputList.end(), outputIndex) == outputList.end()) {
        outputList.push_back(outputIndex);
    }
}

void MultiOutputRenderer::clearLayerMapping(int layerId) {
    // Remove from all outputs
    for (auto& output : outputs_) {
        output.layerMappings.erase(
            std::remove_if(output.layerMappings.begin(), output.layerMappings.end(),
                          [layerId](const LayerRenderInfo& info) {
                              return info.layerId == layerId;
                          }),
            output.layerMappings.end()
        );
    }
    
    // Remove from map
    layerToOutputs_.erase(layerId);
}

void MultiOutputRenderer::assignLayerToAllOutputs(int layerId) {
    for (size_t i = 0; i < outputs_.size(); ++i) {
        setLayerMapping(layerId, static_cast<int>(i), Rect::fullFrame(), Rect::fullFrame());
    }
}

void MultiOutputRenderer::setLayerOpacity(int layerId, int outputIndex, float opacity) {
    if (outputIndex < 0 || outputIndex >= static_cast<int>(outputs_.size())) {
        return;
    }
    
    for (auto& mapping : outputs_[outputIndex].layerMappings) {
        if (mapping.layerId == layerId) {
            mapping.opacity = opacity;
            return;
        }
    }
}

void MultiOutputRenderer::setLayerBlendMode(int layerId, int outputIndex, BlendMode mode) {
    if (outputIndex < 0 || outputIndex >= static_cast<int>(outputs_.size())) {
        return;
    }
    
    for (auto& mapping : outputs_[outputIndex].layerMappings) {
        if (mapping.layerId == layerId) {
            mapping.blendMode = mode;
            return;
        }
    }
}

void MultiOutputRenderer::setBlendRegion(int outputIndex, const BlendRegion& blend) {
    if (outputIndex < 0 || outputIndex >= static_cast<int>(outputs_.size())) {
        return;
    }
    
    outputs_[outputIndex].blend = blend;
    outputs_[outputIndex].needsBlending = blend.isEnabled();
}

void MultiOutputRenderer::setBlendEnabled(int outputIndex, bool enabled) {
    if (outputIndex < 0 || outputIndex >= static_cast<int>(outputs_.size())) {
        return;
    }
    
    outputs_[outputIndex].needsBlending = enabled;
}

void MultiOutputRenderer::setWarpMesh(int outputIndex, std::shared_ptr<WarpMesh> mesh) {
    if (outputIndex < 0 || outputIndex >= static_cast<int>(outputs_.size())) {
        return;
    }
    
    outputs_[outputIndex].warpMesh = mesh;
    outputs_[outputIndex].needsWarping = (mesh != nullptr);
}

void MultiOutputRenderer::setWarpEnabled(int outputIndex, bool enabled) {
    if (outputIndex < 0 || outputIndex >= static_cast<int>(outputs_.size())) {
        return;
    }
    
    outputs_[outputIndex].needsWarping = enabled && (outputs_[outputIndex].warpMesh != nullptr);
}

void MultiOutputRenderer::setOutputSinkManager(OutputSinkManager* sinkManager) {
    outputSinkManager_ = sinkManager;
}

void MultiOutputRenderer::setCaptureEnabled(bool enabled) {
    captureEnabled_ = enabled;
    
    if (enabled && !frameCapture_) {
        frameCapture_ = std::make_unique<FrameCapture>();
    }
}

void MultiOutputRenderer::setCaptureSource(int outputIndex) {
    captureSourceIndex_ = outputIndex;
}

void MultiOutputRenderer::setCaptureResolution(int width, int height) {
    captureWidth_ = width;
    captureHeight_ = height;
    
    // Recreate composite FBO if needed
    if (captureSourceIndex_ < 0 && captureEnabled_) {
        createCompositeFBO(width, height);
    }
}

void MultiOutputRenderer::render(LayerManager* layerManager, OSDManager* osdManager) {
    if (!initialized_ || outputs_.empty()) {
        LOG_ERROR << "MultiOutputRenderer::render: Not initialized or no outputs";
        return;
    }
    
    static int renderFrameCount = 0;
    bool debug = (renderFrameCount++ < 5);
    
    // ===== Virtual Canvas Mode =====
    if (useVirtualCanvas_ && canvas_) {
        if (debug) {
            LOG_INFO << "MultiOutputRenderer::render: Virtual Canvas mode, outputs=" << outputs_.size();
        }
        
        // Step 1: Render all layers to virtual canvas
        renderToCanvas(layerManager, osdManager);
        
        if (debug) {
            LOG_INFO << "MultiOutputRenderer::render: Canvas rendered, now blitting to outputs";
        }
        
        // Step 2: Blit canvas regions to each output
        blitToOutputs();
        
        if (debug) {
            LOG_INFO << "MultiOutputRenderer::render: Blit complete";
        }
        
        // Step 3: Capture for virtual outputs (sinks)
        if (captureEnabled_ && outputSinkManager_) {
            captureForVirtualOutputs();
        }
        
        return;
    }
    
    // ===== Legacy Mode (per-output rendering) =====
    
    // Step 1: Render to composite FBO if needed for virtual outputs
    if (captureEnabled_ && captureSourceIndex_ < 0 && outputSinkManager_) {
        if (!compositeFBO_ && captureWidth_ > 0 && captureHeight_ > 0) {
            createCompositeFBO(captureWidth_, captureHeight_);
        }
        if (compositeFBO_) {
            renderToCompositeFBO(layerManager);
        }
    }
    
    // Step 2: Render to each physical output
    for (size_t i = 0; i < outputs_.size(); ++i) {
        renderOutput(static_cast<int>(i), layerManager, osdManager);
    }
    
    // Step 3: Capture from composite FBO if that's the source
    if (captureEnabled_ && captureSourceIndex_ < 0 && outputSinkManager_) {
        captureForVirtualOutputs();
    }
}

void MultiOutputRenderer::renderToCanvas(LayerManager* layerManager, OSDManager* osdManager) {
    if (!canvas_ || !renderer_) {
        return;
    }
    
    // Begin rendering to canvas
    canvas_->beginFrame();
    
    // Set viewport to full canvas
    renderer_->setViewport(0, 0, canvas_->getWidth(), canvas_->getHeight());
    
    // Get all layers sorted by z-order
    auto layers = layerManager->getLayersSortedByZOrder();
    
    // Convert to const vector
    std::vector<const VideoLayer*> constLayers;
    constLayers.reserve(layers.size());
    for (auto* layer : layers) {
        constLayers.push_back(layer);
    }
    
    // Composite all layers
    renderer_->compositeLayers(constLayers);
    
    // Render OSD if available
    // TODO: OSD rendering to canvas
    (void)osdManager;
    
    // End canvas rendering
    canvas_->endFrame();
}

void MultiOutputRenderer::blitToOutputs() {
    if (!blitShader_ || !canvas_) {
        LOG_ERROR << "MultiOutputRenderer: Cannot blit - shader or canvas not ready";
        return;
    }
    
    static int blitLogCount = 0;
    
    for (size_t i = 0; i < outputs_.size(); ++i) {
        auto& output = outputs_[i];
        if (output.surface && output.region.enabled) {
            if (blitLogCount < 5) {
                LOG_INFO << "MultiOutputRenderer: Blitting to output " << i 
                         << " (" << output.region.name << ")"
                         << " canvas region: " << output.region.canvasX << "," << output.region.canvasY
                         << " " << output.region.canvasWidth << "x" << output.region.canvasHeight;
            }
            blitToOutput(output);
        }
    }
    
    if (blitLogCount < 5) {
        blitLogCount++;
    }
}

void MultiOutputRenderer::blitToOutput(OutputState& output) {
    if (!output.surface || !blitShader_ || !canvas_) {
        LOG_ERROR << "MultiOutputRenderer::blitToOutput: Missing surface, shader, or canvas";
        return;
    }
    
    static int debugCount = 0;
    bool debug = debugCount < 10;
    
    if (debug) {
        LOG_INFO << "blitToOutput: " << output.region.name 
                 << " surface=" << output.surface
                 << " canvas tex=" << canvas_->getTexture()
                 << " canvas size=" << canvas_->getWidth() << "x" << canvas_->getHeight();
    }
    
    // Make output surface current
    output.surface->makeCurrent();
    
    if (debug) {
        GLenum err = glGetError();
        LOG_INFO << "blitToOutput: After makeCurrent, GL error=" << err;
    }
    
    // Set viewport to output size
    glViewport(0, 0, output.region.physicalWidth, output.region.physicalHeight);
    
    // Clear to bright color for debugging (RED for display 2)
    if (output.region.canvasX > 0) {
        glClearColor(0.5f, 0.0f, 0.0f, 1.0f);  // Dark red for display 2
    } else {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);  // Black for display 1
    }
    glClear(GL_COLOR_BUFFER_BIT);
    
    if (debug) {
        LOG_INFO << "blitToOutput: Blitting region "
                 << output.region.canvasX << "," << output.region.canvasY
                 << " size " << output.region.canvasWidth << "x" << output.region.canvasHeight
                 << " to physical " << output.region.physicalWidth << "x" << output.region.physicalHeight;
    }
    
    // Blit canvas region to output with blend/warp
    blitShader_->blit(
        canvas_->getTexture(),
        canvas_->getWidth(),
        canvas_->getHeight(),
        output.region
    );
    
    if (debug) {
        GLenum err = glGetError();
        LOG_INFO << "blitToOutput: After blit, GL error=" << err;
    }
    
    // Swap buffers (EGL swap for DRM surfaces)
    output.surface->swapBuffers();
    
    if (debug) {
        LOG_INFO << "blitToOutput: After swapBuffers for " << output.region.name;
        debugCount++;
    }
    
    output.surface->releaseCurrent();
}

void MultiOutputRenderer::renderOutput(int outputIndex, LayerManager* layerManager,
                                        OSDManager* osdManager) {
    if (outputIndex < 0 || outputIndex >= static_cast<int>(outputs_.size())) {
        return;
    }
    
    auto& output = outputs_[outputIndex];
    if (!output.surface) {
        return;
    }
    
    // Make this output's context current
    output.surface->makeCurrent();
    
    uint32_t width = output.surface->getWidth();
    uint32_t height = output.surface->getHeight();
    
    // Set viewport
    glViewport(0, 0, width, height);
    
    // Clear to black
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Render layers assigned to this output
    renderLayersToOutput(output, layerManager);
    
    // Apply edge blending if configured
    if (output.needsBlending) {
        applyBlending(output);
    }
    
    // Apply warping if configured
    if (output.needsWarping) {
        applyWarping(output);
    }
    
    // Render OSD (usually only on primary output)
    if (osdManager && outputIndex == 0 && renderer_) {
        // OSD rendering would go here
    }
    
    // Capture this output if configured for virtual outputs
    if (captureEnabled_ && captureSourceIndex_ == outputIndex) {
        captureForVirtualOutputs();
    }
    
    output.surface->releaseCurrent();
}

void MultiOutputRenderer::presentAll() {
    for (auto& output : outputs_) {
        if (output.surface) {
            output.surface->swapBuffers();
        }
    }
}

const OutputInfo* MultiOutputRenderer::getOutputInfo(int index) const {
    if (index < 0 || index >= static_cast<int>(outputs_.size())) {
        return nullptr;
    }
    if (!outputs_[index].surface) {
        return nullptr;
    }
    return &outputs_[index].surface->getOutputInfo();
}

const std::vector<LayerRenderInfo>& MultiOutputRenderer::getLayerMappings(int outputIndex) const {
    if (outputIndex < 0 || outputIndex >= static_cast<int>(outputs_.size())) {
        return emptyMappings_;
    }
    return outputs_[outputIndex].layerMappings;
}

void MultiOutputRenderer::renderLayersToOutput(OutputState& output, LayerManager* layerManager) {
    if (!layerManager || !renderer_) {
        return;
    }
    
    // If no explicit mappings, render all visible layers using LayerManager
    if (output.layerMappings.empty()) {
        // Get all layers and render them
        std::vector<const VideoLayer*> visibleLayers;
        for (size_t i = 0; i < layerManager->getLayerCount(); ++i) {
            VideoLayer* layer = layerManager->getLayer(static_cast<int>(i));
            if (layer && layer->properties().visible && layer->isReady()) {
                visibleLayers.push_back(layer);
            }
        }
        
        // Sort by z-order (layer ID for now)
        std::sort(visibleLayers.begin(), visibleLayers.end(),
            [](const VideoLayer* a, const VideoLayer* b) {
                return a->getLayerId() < b->getLayerId();
            });
        
        // Render each layer
        for (const VideoLayer* layer : visibleLayers) {
            renderer_->renderLayer(layer);
        }
        return;
    }
    
    // Render each mapped layer
    for (const auto& mapping : output.layerMappings) {
        VideoLayer* layer = layerManager->getLayer(mapping.layerId);
        if (!layer || !layer->properties().visible || !layer->isReady()) {
            continue;
        }
        
        // Check if we have a frame
        const FrameBuffer* cpuBuffer = nullptr;
        const GPUTextureFrameBuffer* gpuBuffer = nullptr;
        if (!layer->getPreparedFrame(cpuBuffer, gpuBuffer)) {
            continue;
        }
        
        // Set layer opacity if needed
        if (mapping.opacity < 1.0f) {
            // Would need to modify renderer to support per-layer opacity
        }
        
        // Render the layer
        renderer_->renderLayer(layer);
    }
}

void MultiOutputRenderer::applyBlending(OutputState& output) {
    // Edge blending shader implementation would go here
    // This creates smooth gradients at the edges of the output
    // for overlapping projector setups
    
    if (!output.blendShader) {
        // Create blend shader on first use
        // output.blendShader = std::make_unique<BlendShader>();
    }
    
    // Apply blend shader
    // const auto& blend = output.blend;
    // Render gradient overlays at left/right/top/bottom edges
}

void MultiOutputRenderer::applyWarping(OutputState& output) {
    // Geometric warping implementation would go here
    // This applies a mesh-based warp for keystone correction
    // or projection onto curved surfaces
    
    if (!output.warpMesh) {
        return;
    }
    
    // Render through warp mesh
    // This would typically involve rendering to a texture
    // then drawing that texture through the warp mesh
}

void MultiOutputRenderer::captureForVirtualOutputs() {
    if (!outputSinkManager_) {
        return;
    }
    
    // ===== Virtual Canvas Mode: Use canvas async capture =====
    if (useVirtualCanvas_ && canvas_) {
        // Start async capture from canvas
        canvas_->startAsyncCapture();
        
        // Check if previous capture is ready
        if (canvas_->isAsyncCaptureReady()) {
            // Allocate frame data
            int width = canvas_->getWidth();
            int height = canvas_->getHeight();
            size_t bufferSize = static_cast<size_t>(width * height * 4);
            
            FrameData frame;
            frame.width = width;
            frame.height = height;
            frame.format = PixelFormat::RGBA32;
            frame.size = bufferSize;
            frame.data = new uint8_t[bufferSize];
            frame.ownsData = true;
            
            if (canvas_->getAsyncCaptureResult(frame.data, bufferSize)) {
                outputSinkManager_->writeFrameToAll(frame);
            }
        }
        return;
    }
    
    // ===== Legacy Mode: Use FrameCapture =====
    if (!frameCapture_) {
        return;
    }
    
    // Initialize frame capture if needed
    if (!frameCapture_->isInitialized()) {
        int width = captureWidth_;
        int height = captureHeight_;
        
        if (width <= 0 || height <= 0) {
            // Use primary output size
            if (!outputs_.empty() && outputs_[0].surface) {
                width = outputs_[0].surface->getWidth();
                height = outputs_[0].surface->getHeight();
            }
        }
        
        if (width > 0 && height > 0) {
            frameCapture_->initialize(width, height);
        }
    }
    
    // Start async capture (non-blocking)
    frameCapture_->startCapture();
    
    // Get previously completed frame and send to virtual outputs
    FrameData completedFrame;
    if (frameCapture_->getCompletedFrame(completedFrame)) {
        outputSinkManager_->writeFrameToAll(completedFrame);
    }
}

void MultiOutputRenderer::renderToCompositeFBO(LayerManager* layerManager) {
    if (!compositeFBO_ || !renderer_ || !layerManager) {
        return;
    }
    
    // Bind composite FBO
    glBindFramebuffer(GL_FRAMEBUFFER, compositeFBO_);
    glViewport(0, 0, captureWidth_, captureHeight_);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Get all visible layers and render them
    std::vector<const VideoLayer*> visibleLayers;
    for (size_t i = 0; i < layerManager->getLayerCount(); ++i) {
        VideoLayer* layer = layerManager->getLayer(static_cast<int>(i));
        if (layer && layer->properties().visible && layer->isReady()) {
            visibleLayers.push_back(layer);
        }
    }
    
    // Sort by z-order
    std::sort(visibleLayers.begin(), visibleLayers.end(),
        [](const VideoLayer* a, const VideoLayer* b) {
            return a->getLayerId() < b->getLayerId();
        });
    
    // Render each layer
    for (const VideoLayer* layer : visibleLayers) {
        renderer_->renderLayer(layer);
    }
    
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool MultiOutputRenderer::createCompositeFBO(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    
    destroyCompositeFBO();
    
    // Create texture
    glGenTextures(1, &compositeTexture_);
    glBindTexture(GL_TEXTURE_2D, compositeTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    
    // Create depth renderbuffer
    glGenRenderbuffers(1, &compositeDepthRBO_);
    glBindRenderbuffer(GL_RENDERBUFFER, compositeDepthRBO_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    
    // Create FBO
    glGenFramebuffers(1, &compositeFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, compositeFBO_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, compositeTexture_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, compositeDepthRBO_);
    
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR << "MultiOutputRenderer: Composite FBO incomplete: " << status;
        destroyCompositeFBO();
        return false;
    }
    
    LOG_INFO << "MultiOutputRenderer: Created composite FBO " << width << "x" << height;
    return true;
}

void MultiOutputRenderer::destroyCompositeFBO() {
    if (compositeFBO_) {
        glDeleteFramebuffers(1, &compositeFBO_);
        compositeFBO_ = 0;
    }
    if (compositeTexture_) {
        glDeleteTextures(1, &compositeTexture_);
        compositeTexture_ = 0;
    }
    if (compositeDepthRBO_) {
        glDeleteRenderbuffers(1, &compositeDepthRBO_);
        compositeDepthRBO_ = 0;
    }
}

int MultiOutputRenderer::findOutputByName(const std::string& name) const {
    for (size_t i = 0; i < outputs_.size(); ++i) {
        if (outputs_[i].surface && 
            outputs_[i].surface->getOutputInfo().name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace videocomposer

