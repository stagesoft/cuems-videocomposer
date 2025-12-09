/**
 * DRMBackend.cpp - DRM/KMS display backend implementation
 */

#include "DRMBackend.h"
#include "../OpenGLRenderer.h"
#include "../DisplayConfigurationManager.h"
#include "../../layer/LayerManager.h"
#include "../../layer/VideoLayer.h"
#include "../../osd/OSDManager.h"
#include "../../utils/Logger.h"

#include <cstdlib>  // for getenv
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <xf86drmMode.h>  // for atomic modesetting

#ifdef HAVE_VAAPI_INTEROP
#include <va/va.h>
#include <va/va_drm.h>
#endif

namespace videocomposer {

DRMBackend::DRMBackend() {
    outputManager_ = std::make_unique<DRMOutputManager>();
    configManager_ = std::make_unique<DisplayConfigurationManager>();
}

DRMBackend::~DRMBackend() {
    closeWindow();
}

bool DRMBackend::openWindow() {
    if (initialized_) {
        LOG_WARNING << "DRMBackend: Already initialized";
        return true;
    }
    
    LOG_INFO << "DRMBackend: Opening DRM display";
    
    // Initialize DRM output manager
    if (!outputManager_->init(devicePath_)) {
        LOG_ERROR << "DRMBackend: Failed to initialize DRM output manager";
        return false;
    }
    
    // Apply resolution mode before creating surfaces
    // This modifies the output dimensions in outputManager_
    if (configManager_) {
        ResolutionPolicy policy = configManager_->getResolutionPolicy();
        ResolutionMode resMode;
        switch (policy) {
            case ResolutionPolicy::NATIVE:   resMode = ResolutionMode::NATIVE; break;
            case ResolutionPolicy::MAXIMUM:  resMode = ResolutionMode::MAXIMUM; break;
            case ResolutionPolicy::HD_1080P: resMode = ResolutionMode::HD_1080P; break;
            case ResolutionPolicy::HD_720P:  resMode = ResolutionMode::HD_720P; break;
            case ResolutionPolicy::UHD_4K:   resMode = ResolutionMode::UHD_4K; break;
            default:                         resMode = ResolutionMode::HD_1080P; break;
        }
        outputManager_->setResolutionMode(resMode);
        outputManager_->applyResolutionMode();
    }
    
    // Get connected outputs (with resolution mode applied)
    const auto& outputs = outputManager_->getOutputs();
    if (outputs.empty()) {
        LOG_ERROR << "DRMBackend: No connected outputs found";
        outputManager_->cleanup();
        return false;
    }
    
    LOG_INFO << "DRMBackend: Found " << outputs.size() << " connected output(s)";
    
    // Create surfaces for each output (keyed by name)
    // Share EGL display and GBM device for context sharing
    EGLContext sharedContext = EGL_NO_CONTEXT;
    EGLDisplay sharedDisplay = EGL_NO_DISPLAY;
    gbm_device* sharedGbmDevice = nullptr;
    
    for (const auto& outputInfo : outputs) {
        const std::string& outputName = outputInfo.name;
        LOG_INFO << "DRMBackend: Creating surface for " << outputName;
        
        auto surface = std::make_unique<DRMSurface>(outputManager_.get(), outputName);
        
        // Pass shared resources to subsequent surfaces
        if (!surface->init(sharedContext, sharedDisplay, sharedGbmDevice)) {
            LOG_ERROR << "DRMBackend: Failed to create surface for " << outputName;
            // Continue with other outputs
            continue;
        }
        
        // Use first surface's resources for sharing
        if (sharedContext == EGL_NO_CONTEXT) {
            sharedContext = surface->getContext();
            sharedDisplay = surface->getDisplay();
            sharedGbmDevice = surface->getGbmDevice();
            LOG_INFO << "DRMBackend: First surface created shared resources (EGL display, GBM device, context)";
        }
        
        surfaces_[outputName] = std::move(surface);
    }
    
    if (surfaces_.empty()) {
        LOG_ERROR << "DRMBackend: No surfaces could be created";
        outputManager_->cleanup();
        return false;
    }
    
    // Initialize EGL extensions
    initEGLExtensions();
    
    // Initialize VAAPI
#ifdef HAVE_VAAPI_INTEROP
    initVAAPI();
#endif
    
    // Initialize rendering
    DRMSurface* primary = getPrimarySurface();
    if (primary) {
        primary->makeCurrent();
        
        // Check for environment variable to disable Virtual Canvas (for debugging)
        const char* disableVC = std::getenv("VIDEOCOMPOSER_NO_VIRTUAL_CANVAS");
        if (disableVC && (std::string(disableVC) == "1" || std::string(disableVC) == "true")) {
            LOG_INFO << "DRMBackend: Virtual Canvas disabled via VIDEOCOMPOSER_NO_VIRTUAL_CANVAS";
            useVirtualCanvas_ = false;
        }
        
        if (useVirtualCanvas_) {
            // Virtual Canvas mode: use MultiOutputRenderer
            if (!initVirtualCanvas()) {
                LOG_WARNING << "DRMBackend: Virtual Canvas init failed, falling back to legacy mode";
                useVirtualCanvas_ = false;
            }
        }
        
        if (!useVirtualCanvas_) {
            // Legacy mode: single OpenGLRenderer
            renderer_ = std::make_unique<OpenGLRenderer>();
            if (!renderer_->init()) {
                LOG_ERROR << "DRMBackend: Failed to initialize OpenGL renderer";
            }
        }
        
        primary->releaseCurrent();
    }
    
    initialized_ = true;
    LOG_INFO << "DRMBackend: Initialized with " << surfaces_.size() << " output(s)"
             << (useVirtualCanvas_ ? " (Virtual Canvas mode)" : " (Legacy mode)");
    
    return true;
}

void DRMBackend::closeWindow() {
    if (!initialized_) {
        return;
    }
    
    LOG_INFO << "DRMBackend: Closing";
    
#ifdef HAVE_VAAPI_INTEROP
    if (vaDisplay_) {
        vaTerminate(vaDisplay_);
        vaDisplay_ = nullptr;
    }
#endif
    
    // Cleanup renderers
    multiRenderer_.reset();
    renderer_.reset();
    outputRegions_.clear();
    
    // Cleanup surfaces
    for (auto& [name, surface] : surfaces_) {
        surface->cleanup();
    }
    surfaces_.clear();
    
    // Cleanup DRM
    outputManager_->cleanup();
    
    initialized_ = false;
}

bool DRMBackend::isWindowOpen() const {
    return initialized_ && !surfaces_.empty();
}

void DRMBackend::render(LayerManager* layerManager, OSDManager* osdManager) {
    if (!initialized_ || surfaces_.empty()) {
        return;
    }
    
    if (useVirtualCanvas_ && multiRenderer_) {
        renderVirtualCanvas(layerManager, osdManager);
    } else {
        renderLegacy(layerManager, osdManager);
    }
}

void DRMBackend::renderVirtualCanvas(LayerManager* layerManager, OSDManager* osdManager) {
    if (!multiRenderer_) {
        return;
    }
    
    // Make primary surface context current for canvas rendering
    DRMSurface* primary = getPrimarySurface();
    if (!primary) {
        return;
    }
    
    // Process any completed flips (non-blocking)
    for (auto& [name, surface] : surfaces_) {
        surface->processFlipEvents();
    }
    
    primary->makeCurrent();
    
    // MultiOutputRenderer::render() handles:
    // 1. Rendering all layers to VirtualCanvas
    // 2. Blitting regions to each output surface (with blend/warp)
    // 3. Swapping buffers on each surface
    multiRenderer_->render(layerManager, osdManager);
    
    primary->releaseCurrent();
    
    // Use atomic modesetting for multi-output to flip all on same vsync
    bool useAtomic = outputManager_->supportsAtomic() && surfaces_.size() > 1;
    
    if (useAtomic) {
        // ATOMIC PATH: Submit all page flips in single atomic commit
        // This allows all outputs to flip on the same vsync = 60fps for dual output
        
        // Wait for all pending flips first
        for (auto& [name, surface] : surfaces_) {
            if (surface->isFlipPending()) {
                surface->waitForFlip();
            }
        }
        
        // Check if all surfaces have mode set (first frame uses SetCrtc)
        bool allModesSet = true;
        for (auto& [name, surface] : surfaces_) {
            if (!surface->isModeSet()) {
                allModesSet = false;
                break;
            }
        }
        
        if (!allModesSet) {
            // First frame: use legacy path to set modes
            for (auto& [name, surface] : surfaces_) {
                surface->schedulePageFlip();
            }
        } else {
            // Prepare atomic request
            drmModeAtomicReq* request = outputManager_->createAtomicRequest();
            if (request) {
                bool prepareSuccess = true;
                
                // Prepare each surface for atomic flip
                for (auto& [name, surface] : surfaces_) {
                    uint32_t fbId = surface->prepareAtomicFlip();
                    if (fbId == 0) {
                        prepareSuccess = false;
                        break;
                    }
                    
                    // Get FB_ID property for CRTC
                    uint32_t crtcId = surface->getCrtcId();
                    uint32_t fbPropId = outputManager_->getPropertyId(crtcId, DRM_MODE_OBJECT_CRTC, "FB_ID");
                    
                    if (fbPropId == 0) {
                        LOG_WARNING << "DRMBackend: FB_ID property not found for CRTC " << crtcId;
                        prepareSuccess = false;
                        break;
                    }
                    
                    // Add to atomic request
                    if (drmModeAtomicAddProperty(request, crtcId, fbPropId, fbId) < 0) {
                        LOG_WARNING << "DRMBackend: Failed to add FB_ID to atomic request";
                        prepareSuccess = false;
                        break;
                    }
                }
                
                if (prepareSuccess) {
                    // Commit atomically with page flip event
                    uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
                    if (outputManager_->commitAtomic(request, flags)) {
                        // Success - finalize all surfaces
                        for (auto& [name, surface] : surfaces_) {
                            surface->finalizeAtomicFlip();
                        }
                    } else {
                        LOG_WARNING << "DRMBackend: Atomic commit failed, falling back to legacy";
                        // Fallback to legacy per-surface flips
                        for (auto& [name, surface] : surfaces_) {
                            surface->schedulePageFlip();
                        }
                    }
                } else {
                    // Fallback to legacy
                    for (auto& [name, surface] : surfaces_) {
                        surface->schedulePageFlip();
                    }
                }
                
                drmModeAtomicFree(request);
            } else {
                // Fallback to legacy
                for (auto& [name, surface] : surfaces_) {
                    if (surface->isFlipPending()) {
                        surface->waitForFlip();
                    }
                    surface->schedulePageFlip();
                }
            }
        }
    } else {
        // LEGACY PATH: Sequential page flips (single output or no atomic support)
        for (auto& [name, surface] : surfaces_) {
            if (surface->isFlipPending()) {
                surface->waitForFlip();
            }
            surface->schedulePageFlip();
        }
    }
}
    
void DRMBackend::renderLegacy(LayerManager* layerManager, OSDManager* osdManager) {
    (void)osdManager;  // OSD rendering handled separately
    
    // MPV-STYLE: Process flip events first (non-blocking), render, THEN wait
    for (auto& [name, surface] : surfaces_) {
        surface->processFlipEvents();
    }
    
    // Render to each output
    for (auto& [name, surface] : surfaces_) {
        if (!surface->isInitialized()) {
            continue;
        }
        
        // Begin frame
        if (!surface->beginFrame()) {
            continue;
        }
        
        // Set viewport for renderer (critical for aspect ratio calculations)
        if (renderer_) {
            renderer_->setViewport(0, 0, surface->getWidth(), surface->getHeight());
        }
        
        // Clear
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        
        // Render layers (use getLayersSortedByZOrder like X11/Wayland backends do)
        if (renderer_ && layerManager) {
            auto layers = layerManager->getLayersSortedByZOrder();
            
            for (size_t i = 0; i < layers.size(); ++i) {
                VideoLayer* layer = layers[i];
                if (layer) {
                    bool visible = layer->properties().visible;
                    bool ready = layer->isReady();
                    
                    if (visible && ready) {
                        renderer_->renderLayer(layer);
                    }
                }
            }
        }
        
        // End frame
        surface->endFrame();
        
        // Wait for pending flip before scheduling a new one
        // (can only have one flip pending at a time)
        if (surface->isFlipPending()) {
            surface->waitForFlip();
        }
        
        // Schedule page flip (non-blocking)
        surface->schedulePageFlip();
    }
}

void DRMBackend::handleEvents() {
    // Poll for hotplug events
    if (outputManager_) {
        outputManager_->pollHotplug();
    }
}

void DRMBackend::resize(unsigned int width, unsigned int height) {
    // DRM doesn't resize windows - mode must be changed
    LOG_WARNING << "DRMBackend: resize() not supported - use setOutputMode()";
}

void DRMBackend::getWindowSize(unsigned int* width, unsigned int* height) const {
    DRMSurface* primary = const_cast<DRMBackend*>(this)->getPrimarySurface();
    if (primary) {
        *width = primary->getWidth();
        *height = primary->getHeight();
    } else {
        *width = 0;
        *height = 0;
    }
}

void DRMBackend::setPosition(int x, int y) {
    // DRM doesn't support window positioning
    (void)x;
    (void)y;
}

void DRMBackend::getWindowPos(int* x, int* y) const {
    *x = 0;
    *y = 0;
}

void DRMBackend::setFullscreen(int action) {
    // DRM is always fullscreen
    (void)action;
}

bool DRMBackend::getFullscreen() const {
    return true;  // Always fullscreen
}

void DRMBackend::setOnTop(int action) {
    // DRM is always on top (no compositor)
    (void)action;
}

bool DRMBackend::getOnTop() const {
    return true;
}

void* DRMBackend::getContext() {
    DRMSurface* primary = getPrimarySurface();
    return primary ? primary->getContext() : nullptr;
}

void DRMBackend::makeCurrent() {
    DRMSurface* primary = getPrimarySurface();
    if (primary) {
        primary->makeCurrent();
    }
}

void DRMBackend::clearCurrent() {
    DRMSurface* primary = getPrimarySurface();
    if (primary) {
        primary->releaseCurrent();
    }
}

OpenGLRenderer* DRMBackend::getRenderer() {
    return renderer_.get();
}

#ifdef HAVE_EGL
EGLDisplay DRMBackend::getEGLDisplay() const {
    DRMSurface* primary = const_cast<DRMBackend*>(this)->getPrimarySurface();
    return primary ? primary->getDisplay() : EGL_NO_DISPLAY;
}

PFNEGLCREATEIMAGEKHRPROC DRMBackend::getEglCreateImageKHR() const {
    return eglCreateImageKHR_;
}

PFNEGLDESTROYIMAGEKHRPROC DRMBackend::getEglDestroyImageKHR() const {
    return eglDestroyImageKHR_;
}

PFNGLEGLIMAGETARGETTEXTURE2DOESPROC DRMBackend::getGlEGLImageTargetTexture2DOES() const {
    return glEGLImageTargetTexture2DOES_;
}

PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC DRMBackend::getGlEGLImageTargetTexStorageEXT() const {
    return glEGLImageTargetTexStorageEXT_;
}
#endif

#ifdef HAVE_VAAPI_INTEROP
VADisplay DRMBackend::getVADisplay() const {
    return vaDisplay_;
}
#endif

std::vector<OutputInfo> DRMBackend::getOutputs() const {
    return outputManager_->getOutputs();
}

size_t DRMBackend::getOutputCount() const {
    return surfaces_.size();
}

bool DRMBackend::configureOutputRegion(const std::string& outputName, 
                                        int canvasX, int canvasY,
                                        int canvasWidth, int canvasHeight) {
    // Find the output region by name
    OutputRegion* region = nullptr;
    for (auto& r : outputRegions_) {
        if (r.name == outputName) {
            region = &r;
            break;
        }
    }
    
    if (!region) {
        LOG_ERROR << "DRMBackend::configureOutputRegion: Unknown output " << outputName;
        return false;
    }
    
    // Get output native dimensions if not specified
    if (canvasWidth <= 0 || canvasHeight <= 0) {
        auto it = surfaces_.find(outputName);
        if (it != surfaces_.end()) {
            const auto& info = it->second->getOutputInfo();
            if (canvasWidth <= 0) canvasWidth = info.width;
            if (canvasHeight <= 0) canvasHeight = info.height;
        }
    }
    
    // Update the output region
    region->canvasX = canvasX;
    region->canvasY = canvasY;
    region->canvasWidth = canvasWidth;
    region->canvasHeight = canvasHeight;
    
    LOG_INFO << "DRMBackend: Configured " << outputName 
             << " region: " << canvasX << "," << canvasY
             << " " << canvasWidth << "x" << canvasHeight;
    
    // Update MultiOutputRenderer if initialized
    if (multiRenderer_) {
        // Make GL context current before any GL operations (FBO creation, etc.)
        if (!surfaces_.empty()) {
            surfaces_.begin()->second->makeCurrent();
        }
        
        std::vector<OutputSurface*> surfacePtrs;
        for (auto& [name, surface] : surfaces_) {
            surfacePtrs.push_back(surface.get());
        }
        multiRenderer_->configureOutputs(outputRegions_, surfacePtrs);
    }
    
    return true;
}

bool DRMBackend::configureOutputBlend(const std::string& outputName,
                                       float left, float right,
                                       float top, float bottom, float gamma) {
    // Find the output region by name
    OutputRegion* region = nullptr;
    for (auto& r : outputRegions_) {
        if (r.name == outputName) {
            region = &r;
            break;
        }
    }
    
    if (!region) {
        LOG_ERROR << "DRMBackend::configureOutputBlend: Unknown output " << outputName;
        return false;
    }
    
    // Update blend configuration
    region->blend.left = left;
    region->blend.right = right;
    region->blend.top = top;
    region->blend.bottom = bottom;
    region->blend.gamma = gamma;
    
    LOG_INFO << "DRMBackend: Configured " << outputName 
             << " blend: L=" << left << " R=" << right
             << " T=" << top << " B=" << bottom << " gamma=" << gamma;
    
    // Update MultiOutputRenderer if initialized
    if (multiRenderer_) {
        // Make GL context current before any GL operations (FBO creation, etc.)
        if (!surfaces_.empty()) {
            surfaces_.begin()->second->makeCurrent();
        }
        
        std::vector<OutputSurface*> surfacePtrs;
        for (auto& [name, surface] : surfaces_) {
            surfacePtrs.push_back(surface.get());
        }
        multiRenderer_->configureOutputs(outputRegions_, surfacePtrs);
    }
    
    return true;
}

DRMSurface* DRMBackend::getSurface(const std::string& name) {
    auto it = surfaces_.find(name);
    if (it != surfaces_.end()) {
        return it->second.get();
    }
    return nullptr;
}

DRMSurface* DRMBackend::getPrimarySurface() {
    // Return first surface in map (consistent ordering)
    if (!surfaces_.empty()) {
        return surfaces_.begin()->second.get();
    }
    return nullptr;
}

bool DRMBackend::setOutputMode(const std::string& outputName, int width, int height, double refresh) {
    LOG_INFO << "DRMBackend::setOutputMode called: " << outputName 
             << " width=" << width << " height=" << height << " refresh=" << refresh;
    
    auto it = surfaces_.find(outputName);
    if (it == surfaces_.end()) {
        LOG_ERROR << "DRMBackend::setOutputMode: No surface for output " << outputName;
        return false;
    }
    
    DRMSurface* surface = it->second.get();
    if (!surface) {
        LOG_ERROR << "DRMBackend::setOutputMode: Null surface for " << outputName;
        return false;
    }
    
    // Check if resolution is actually changing
    int oldWidth = static_cast<int>(surface->getWidth());
    int oldHeight = static_cast<int>(surface->getHeight());
    
    LOG_INFO << "  Current surface size: " << oldWidth << "x" << oldHeight;
    
    if (width == oldWidth && height == oldHeight) {
        LOG_INFO << "DRMBackend::setOutputMode: Already at " << width << "x" << height;
        return true;
    }
    
    LOG_INFO << "DRMBackend: Changing " << outputName << " from "
             << oldWidth << "x" << oldHeight << " to " << width << "x" << height;
    
    // Get connector index for DRMOutputManager
    const DRMConnector* conn = outputManager_->getConnectorByName(outputName);
    if (!conn) {
        LOG_ERROR << "DRMBackend::setOutputMode: Unknown connector " << outputName;
        return false;
    }
    int connectorIndex = conn->info.index;
    
    // Step 1: Update the mode in DRMOutputManager (stores mode for later use)
    if (!outputManager_->prepareMode(connectorIndex, width, height, refresh)) {
        LOG_ERROR << "DRMBackend::setOutputMode: Mode not available";
        return false;
    }
    
    // Step 2: Resize the GBM/EGL surface (creates new buffers, resets modeSet_=false)
    if (!surface->resize(width, height)) {
        LOG_ERROR << "DRMBackend::setOutputMode: Failed to resize surface";
        return false;
    }
    
    // Step 3: Update output region by name
    for (auto& region : outputRegions_) {
        if (region.name == outputName) {
            region.physicalWidth = width;
            region.physicalHeight = height;
            if (region.canvasWidth == oldWidth) region.canvasWidth = width;
            if (region.canvasHeight == oldHeight) region.canvasHeight = height;
            break;
        }
    }
    
    // Step 4: Recalculate canvas size and reconfigure renderer
    if (multiRenderer_) {
        // Make GL context current before any GL operations
        if (!surfaces_.empty()) {
            surfaces_.begin()->second->makeCurrent();
        }
        
        // Recalculate total canvas size
        int canvasWidth = 0, canvasHeight = 0;
        for (const auto& region : outputRegions_) {
            int right = region.canvasX + region.canvasWidth;
            int bottom = region.canvasY + region.canvasHeight;
            canvasWidth = std::max(canvasWidth, right);
            canvasHeight = std::max(canvasHeight, bottom);
        }
        
        // Resize virtual canvas if needed
        VirtualCanvas* canvas = multiRenderer_->getCanvas();
        if (canvas && (canvasWidth != canvas->getWidth() || 
                       canvasHeight != canvas->getHeight())) {
            LOG_INFO << "DRMBackend: Resizing virtual canvas to " 
                     << canvasWidth << "x" << canvasHeight;
            canvas->configure(canvasWidth, canvasHeight);
        }
        
        // Reconfigure renderer with new regions
        std::vector<OutputSurface*> surfacePtrs;
        for (auto& [name, surf] : surfaces_) {
            surfacePtrs.push_back(surf.get());
        }
        multiRenderer_->configureOutputs(outputRegions_, surfacePtrs);
    }
    
    LOG_INFO << "DRMBackend: " << outputName << " successfully changed to "
             << width << "x" << height;
    
    return true;
}

void DRMBackend::setCaptureEnabled(bool enabled, int width, int height) {
    if (!multiRenderer_) {
        LOG_WARNING << "DRMBackend::setCaptureEnabled: No MultiOutputRenderer";
        return;
    }
    
    if (width > 0 && height > 0) {
        multiRenderer_->setCaptureResolution(width, height);
    }
    
    multiRenderer_->setCaptureEnabled(enabled);
    
    LOG_INFO << "DRMBackend: Capture " << (enabled ? "enabled" : "disabled");
    if (enabled && width > 0 && height > 0) {
        LOG_INFO << "  Resolution: " << width << "x" << height;
    }
}

bool DRMBackend::isCaptureEnabled() const {
    if (!multiRenderer_) {
        return false;
    }
    return multiRenderer_->isCaptureEnabled();
}

void DRMBackend::setOutputSinkManager(OutputSinkManager* sinkManager) {
    if (multiRenderer_) {
        multiRenderer_->setOutputSinkManager(sinkManager);
        LOG_INFO << "DRMBackend: Output sink manager " 
                 << (sinkManager ? "connected" : "disconnected");
    }
}

bool DRMBackend::setResolutionMode(const std::string& mode) {
    if (!configManager_) {
        return false;
    }
    
    // Use config manager to parse and store the mode
    if (!configManager_->setResolutionPolicyFromString(mode)) {
        return false;
    }
    
    // If not initialized yet, just store the mode - it will be applied in openWindow()
    if (!initialized_ || !outputManager_ || surfaces_.empty()) {
        LOG_INFO << "Resolution mode set to: " << mode << " (will be applied when display is initialized)";
        return true;
    }
    
    LOG_INFO << "Resolution mode: " << mode;
    
    // Map to DRMOutputManager's ResolutionMode
    ResolutionPolicy policy = configManager_->getResolutionPolicy();
    ResolutionMode resMode;
    
    switch (policy) {
        case ResolutionPolicy::NATIVE:
            resMode = ResolutionMode::NATIVE;
            break;
        case ResolutionPolicy::MAXIMUM:
            resMode = ResolutionMode::MAXIMUM;
            break;
        case ResolutionPolicy::HD_1080P:
            resMode = ResolutionMode::HD_1080P;
            break;
        case ResolutionPolicy::HD_720P:
            resMode = ResolutionMode::HD_720P;
            break;
        case ResolutionPolicy::UHD_4K:
            resMode = ResolutionMode::UHD_4K;
            break;
        default:
            resMode = ResolutionMode::HD_1080P;
            break;
    }
    
    // Set the mode in output manager (updates internal state)
    outputManager_->setResolutionMode(resMode);
    
    // Apply mode to determine target resolutions for each output
    outputManager_->applyResolutionMode();
    
    // Now actually change each surface to its target resolution
    bool success = true;
    auto outputs = outputManager_->getOutputs();
    
    for (auto& [surfName, surface] : surfaces_) {
        if (!surface) continue;
        
        // Find the matching output info by name
        const OutputInfo* info = nullptr;
        for (const auto& out : outputs) {
            if (out.name == surfName) {
                info = &out;
                break;
            }
        }
        
        if (!info || !info->connected || !info->enabled) {
            continue;
        }
        
        int currentW = static_cast<int>(surface->getWidth());
        int currentH = static_cast<int>(surface->getHeight());
        
        // If the target resolution differs, apply the change
        if (info->width != currentW || info->height != currentH) {
            LOG_INFO << "Applying resolution mode to " << surfName << ": "
                     << currentW << "x" << currentH << " -> " 
                     << info->width << "x" << info->height;
            
            if (!setOutputMode(surfName, info->width, info->height, info->refreshRate)) {
                LOG_ERROR << "Failed to apply resolution mode to " << surfName;
                success = false;
            }
        }
    }
    
    return success;
}

bool DRMBackend::saveConfiguration(const std::string& path) {
    if (!configManager_) {
        return false;
    }
    
    std::string configPath = path.empty() ? 
        DisplayConfigurationManager::getDefaultConfigPath() : path;
    
    return configManager_->saveToFile(configPath);
}

bool DRMBackend::loadConfiguration(const std::string& path) {
    if (!configManager_) {
        return false;
    }
    
    std::string configPath = path.empty() ? 
        DisplayConfigurationManager::getDefaultConfigPath() : path;
    
    if (!configManager_->loadFromFile(configPath)) {
        return false;
    }
    
    // Apply loaded configuration
    return setResolutionMode(configManager_->getResolutionPolicyString());
}

void DRMBackend::initEGLExtensions() {
#ifdef HAVE_EGL
    DRMSurface* primary = getPrimarySurface();
    if (!primary) {
        return;
    }
    
    EGLDisplay display = primary->getDisplay();
    if (display == EGL_NO_DISPLAY) {
        return;
    }
    
    // Get extension function pointers
    eglCreateImageKHR_ = (PFNEGLCREATEIMAGEKHRPROC)
        eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR_ = (PFNEGLDESTROYIMAGEKHRPROC)
        eglGetProcAddress("eglDestroyImageKHR");
    glEGLImageTargetTexture2DOES_ = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    // Desktop GL extension for EGL image binding (mpv approach for DRM/KMS)
    glEGLImageTargetTexStorageEXT_ = (PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC)
        eglGetProcAddress("glEGLImageTargetTexStorageEXT");
    
    if (eglCreateImageKHR_ && eglDestroyImageKHR_) {
        if (glEGLImageTargetTexStorageEXT_) {
            LOG_INFO << "DRMBackend: EGL image extensions available (TexStorageEXT for Desktop GL)";
        } else if (glEGLImageTargetTexture2DOES_) {
            LOG_INFO << "DRMBackend: EGL image extensions available (Texture2DOES fallback)";
        } else {
            LOG_WARNING << "DRMBackend: No EGL image target function available";
        }
    } else {
        LOG_WARNING << "DRMBackend: EGL image extensions not available";
    }
#endif
}

void DRMBackend::initVAAPI() {
#ifdef HAVE_VAAPI_INTEROP
    if (!outputManager_) {
        return;
    }
    
    int fd = outputManager_->getFd();
    if (fd < 0) {
        return;
    }
    
    vaDisplay_ = vaGetDisplayDRM(fd);
    if (!vaDisplay_) {
        LOG_WARNING << "DRMBackend: Failed to get VAAPI display";
        return;
    }
    
    int major, minor;
    VAStatus status = vaInitialize(vaDisplay_, &major, &minor);
    if (status != VA_STATUS_SUCCESS) {
        LOG_WARNING << "DRMBackend: Failed to initialize VAAPI: " << vaErrorStr(status);
        vaDisplay_ = nullptr;
        return;
    }
    
    LOG_INFO << "DRMBackend: VAAPI initialized (version " << major << "." << minor << ")";
#endif
}

bool DRMBackend::initVirtualCanvas() {
    if (surfaces_.empty()) {
        LOG_ERROR << "DRMBackend: No surfaces for Virtual Canvas";
        return false;
    }
    
    DRMSurface* primary = getPrimarySurface();
    if (!primary) {
        LOG_ERROR << "DRMBackend: No primary surface for Virtual Canvas";
        return false;
    }
    
    // Create MultiOutputRenderer with EGL context
    multiRenderer_ = std::make_unique<MultiOutputRenderer>();
    
#ifdef HAVE_EGL
    if (!multiRenderer_->init(primary->getDisplay(), primary->getContext())) {
        LOG_ERROR << "DRMBackend: Failed to initialize MultiOutputRenderer";
        multiRenderer_.reset();
        return false;
    }
#else
    LOG_ERROR << "DRMBackend: Virtual Canvas requires EGL";
    multiRenderer_.reset();
    return false;
#endif
    
    // Build default output regions
    buildOutputRegions();
    
    // Configure MultiOutputRenderer with surfaces and regions
    std::vector<OutputSurface*> surfacePtrs;
    for (auto& [name, surface] : surfaces_) {
        surfacePtrs.push_back(surface.get());
    }
    
    multiRenderer_->configureOutputs(outputRegions_, surfacePtrs);
    
    LOG_INFO << "DRMBackend: Virtual Canvas initialized with " 
             << outputRegions_.size() << " output(s)";
    
    return true;
}

void DRMBackend::buildOutputRegions() {
    outputRegions_.clear();
    
    int canvasX = 0;
    
    for (auto& [outputName, surface] : surfaces_) {
        if (!surface) continue;
        
        const OutputInfo& info = surface->getOutputInfo();
        
        OutputRegion region = OutputRegion::createDefault(
            info.name,
            info.width,
            info.height,
            canvasX,
            0  // All outputs at Y=0 (horizontal arrangement)
        );
        
        outputRegions_.push_back(region);
        
        // Next output starts after this one
        canvasX += info.width;
        
        LOG_INFO << "DRMBackend: Output region (" << info.name << "): "
                 << region.canvasX << "," << region.canvasY << " "
                 << region.canvasWidth << "x" << region.canvasHeight;
    }
}

void DRMBackend::autoConfigureOutputs(const std::string& arrangement, int overlap) {
    outputRegions_.clear();
    
    int canvasX = 0;
    int canvasY = 0;
    
    for (auto& [outputName, surface] : surfaces_) {
        if (!surface) continue;
        
        const OutputInfo& info = surface->getOutputInfo();
        
        OutputRegion region;
        region.name = info.name;
        region.canvasWidth = info.width;
        region.canvasHeight = info.height;
        region.physicalWidth = info.width;
        region.physicalHeight = info.height;
        region.enabled = true;
        
        if (arrangement == "vertical") {
            region.canvasX = 0;
            region.canvasY = canvasY;
            
            // Blend with previous output
            if (!outputRegions_.empty() && overlap > 0) {
                region.canvasY -= overlap;
                region.blend.top = static_cast<float>(overlap);
                outputRegions_.back().blend.bottom = static_cast<float>(overlap);
            }
            
            canvasY += info.height;
        } else {
            // Default: horizontal
            region.canvasX = canvasX;
            region.canvasY = 0;
            
            // Blend with previous output
            if (!outputRegions_.empty() && overlap > 0) {
                region.canvasX -= overlap;
                region.blend.left = static_cast<float>(overlap);
                outputRegions_.back().blend.right = static_cast<float>(overlap);
            }
            
            canvasX += info.width;
        }
        
        outputRegions_.push_back(region);
    }
    
    // Reconfigure if already initialized
    if (multiRenderer_ && multiRenderer_->isInitialized()) {
        // Make GL context current before any GL operations (FBO creation, etc.)
        if (!surfaces_.empty()) {
            surfaces_.begin()->second->makeCurrent();
        }
        
        std::vector<OutputSurface*> surfacePtrs;
        for (auto& [name, surf] : surfaces_) {
            surfacePtrs.push_back(surf.get());
        }
        multiRenderer_->configureOutputs(outputRegions_, surfacePtrs);
    }
    
    LOG_INFO << "DRMBackend: Auto-configured " << outputRegions_.size() 
             << " outputs (" << arrangement << ", overlap=" << overlap << ")";
}

bool DRMBackend::configureOutputRegion(const std::string& outputName, const OutputRegion& region) {
    // Find the output
    for (auto& r : outputRegions_) {
        if (r.name == outputName) {
            r = region;
            r.name = outputName;  // Preserve name
            
            // Reconfigure if initialized
            if (multiRenderer_ && multiRenderer_->isInitialized()) {
                // Make GL context current before any GL operations (FBO creation, etc.)
                if (!surfaces_.empty()) {
                    surfaces_.begin()->second->makeCurrent();
                }
                
                std::vector<OutputSurface*> surfacePtrs;
                for (auto& [name, surf] : surfaces_) {
                    surfacePtrs.push_back(surf.get());
                }
                multiRenderer_->configureOutputs(outputRegions_, surfacePtrs);
            }
            
            LOG_INFO << "DRMBackend: Configured output region " << outputName;
            return true;
        }
    }
    
    LOG_WARNING << "DRMBackend: Output not found: " << outputName;
    return false;
}

int64_t DRMBackend::getTotalDroppedFrames() const {
    int64_t total = 0;
    for (const auto& [name, surface] : surfaces_) {
        if (surface) {
            total += surface->getPresentationTiming().getTotalDroppedFrames();
        }
    }
    return total;
}

void DRMBackend::setVideoFramerate(double fps) {
    // Set video framerate on all surfaces' presentation timing
    // This tells them to expect vsync skips (e.g., 25fps on 60Hz display)
    for (auto& [name, surface] : surfaces_) {
        if (surface) {
            surface->getPresentationTiming().setVideoFramerate(fps);
        }
    }
}

} // namespace videocomposer

