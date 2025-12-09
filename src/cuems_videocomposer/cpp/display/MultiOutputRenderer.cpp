/**
 * MultiOutputRenderer.cpp - Multi-output rendering implementation
 * 
 * Uses Virtual Canvas architecture: all layers render to a single FBO,
 * then regions are blitted to physical outputs with blend/warp.
 */

#include "MultiOutputRenderer.h"
#include "../output/OutputSinkManager.h"
#include "../layer/LayerManager.h"
#include "../utils/Logger.h"

#include <GL/glew.h>
#include <GL/gl.h>
#include <cstring>
#include <algorithm>

namespace videocomposer {

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
    
    initialized_ = true;
    
    LOG_INFO << "MultiOutputRenderer: Initialized with Virtual Canvas mode";
    return true;
}
#endif

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
    if (!canvas_) {
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
    blitShader_.reset();
    renderer_.reset();
    canvas_.reset();
    
    outputs_.clear();
    outputRegions_.clear();
    
    initialized_ = false;
}

void MultiOutputRenderer::setOutputSinkManager(OutputSinkManager* sinkManager) {
    outputSinkManager_ = sinkManager;
}

void MultiOutputRenderer::setCaptureEnabled(bool enabled) {
    captureEnabled_ = enabled;
}

void MultiOutputRenderer::setCaptureResolution(int width, int height) {
    captureWidth_ = width;
    captureHeight_ = height;
}

void MultiOutputRenderer::render(LayerManager* layerManager, OSDManager* osdManager) {
    if (!initialized_ || outputs_.empty() || !canvas_) {
        return;
    }
    
    // Step 1: Render all layers to virtual canvas
    renderToCanvas(layerManager, osdManager);
    
    // Step 2: Blit canvas regions to each output
    blitToOutputs();
    
    // Step 3: Capture for virtual outputs (sinks)
    if (captureEnabled_ && outputSinkManager_) {
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
        return;
    }
    
    for (auto& output : outputs_) {
        if (output.surface && output.region.enabled) {
            blitToOutput(output);
        }
    }
}

void MultiOutputRenderer::blitToOutput(OutputState& output) {
    if (!output.surface || !blitShader_ || !canvas_) {
        return;
    }
    
    // Make output surface current
    output.surface->makeCurrent();
    
    // Set viewport to output size
    glViewport(0, 0, output.region.physicalWidth, output.region.physicalHeight);
    
    // Clear to black
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Blit canvas region to output with blend/warp
    blitShader_->blit(
        canvas_->getTexture(),
        canvas_->getWidth(),
        canvas_->getHeight(),
        output.region
    );
    
    // Swap buffers
    output.surface->swapBuffers();
    
    // NOTE: Page flips are now handled by DRMBackend for atomic modesetting support
    // This allows all outputs to flip on the same vsync for 60fps dual-output
    
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

void MultiOutputRenderer::captureForVirtualOutputs() {
    if (!outputSinkManager_ || !canvas_) {
        return;
    }
    
    // Start async capture from canvas (double-buffered PBO)
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

