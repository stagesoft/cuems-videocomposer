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

#include <EGL/egl.h>
#include <EGL/eglext.h>

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
    
    // Get connected outputs
    const auto& outputs = outputManager_->getOutputs();
    if (outputs.empty()) {
        LOG_ERROR << "DRMBackend: No connected outputs found";
        outputManager_->cleanup();
        return false;
    }
    
    LOG_INFO << "DRMBackend: Found " << outputs.size() << " connected output(s)";
    
    // Create surfaces for each output
    // Share EGL display and GBM device for context sharing
    EGLContext sharedContext = EGL_NO_CONTEXT;
    EGLDisplay sharedDisplay = EGL_NO_DISPLAY;
    gbm_device* sharedGbmDevice = nullptr;
    
    for (size_t i = 0; i < outputs.size(); ++i) {
        LOG_INFO << "DRMBackend: Creating surface for output " << i 
                 << " (" << outputs[i].name << ")";
        
        auto surface = std::make_unique<DRMSurface>(outputManager_.get(), static_cast<int>(i));
        
        // Pass shared resources to subsequent surfaces
        if (!surface->init(sharedContext, sharedDisplay, sharedGbmDevice)) {
            LOG_ERROR << "DRMBackend: Failed to create surface for output " << i;
            // Continue with other outputs
            continue;
        }
        
        // Use first surface's resources for sharing
        if (sharedContext == EGL_NO_CONTEXT) {
            sharedContext = surface->getContext();
            sharedDisplay = surface->getDisplay();
            sharedGbmDevice = surface->getGbmDevice();
            LOG_INFO << "DRMBackend: Output 0 created shared resources (EGL display, GBM device, context)";
        }
        
        surfaces_.push_back(std::move(surface));
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
    for (auto& surface : surfaces_) {
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
    
    primary->makeCurrent();
    
    // MultiOutputRenderer::render() handles:
    // 1. Rendering all layers to VirtualCanvas
    // 2. Blitting regions to each output surface (with blend/warp)
    // 3. Swapping buffers on each surface
    multiRenderer_->render(layerManager, osdManager);
    
    primary->releaseCurrent();
    
    // Schedule page flips for all surfaces
    for (auto& surface : surfaces_) {
        surface->schedulePageFlip();
    }
    
    // Wait for all flips to complete
    for (auto& surface : surfaces_) {
        if (surface->isFlipPending()) {
            surface->waitForFlip();
        }
    }
}

void DRMBackend::renderLegacy(LayerManager* layerManager, OSDManager* osdManager) {
    (void)osdManager;  // OSD rendering handled separately
    
    // Render to each output
    for (auto& surface : surfaces_) {
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
        
        // Schedule page flip
        surface->schedulePageFlip();
    }
    
    // Wait for all flips to complete
    for (auto& surface : surfaces_) {
        if (surface->isFlipPending()) {
            surface->waitForFlip();
        }
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

int DRMBackend::getOutputIndexByName(const std::string& name) const {
    for (size_t i = 0; i < surfaces_.size(); ++i) {
        if (surfaces_[i]->getOutputInfo().name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool DRMBackend::configureOutputRegion(int outputIndex, int canvasX, int canvasY,
                                        int canvasWidth, int canvasHeight) {
    if (outputIndex < 0 || outputIndex >= static_cast<int>(outputRegions_.size())) {
        LOG_ERROR << "DRMBackend::configureOutputRegion: Invalid output index " << outputIndex;
        return false;
    }
    
    // Get output native dimensions if not specified
    if (canvasWidth <= 0 || canvasHeight <= 0) {
        const auto& info = surfaces_[outputIndex]->getOutputInfo();
        if (canvasWidth <= 0) canvasWidth = info.width;
        if (canvasHeight <= 0) canvasHeight = info.height;
    }
    
    // Update the output region
    outputRegions_[outputIndex].canvasX = canvasX;
    outputRegions_[outputIndex].canvasY = canvasY;
    outputRegions_[outputIndex].canvasWidth = canvasWidth;
    outputRegions_[outputIndex].canvasHeight = canvasHeight;
    
    LOG_INFO << "DRMBackend: Configured output " << outputIndex 
             << " region: " << canvasX << "," << canvasY
             << " " << canvasWidth << "x" << canvasHeight;
    
    // Update MultiOutputRenderer if initialized
    if (multiRenderer_) {
        std::vector<OutputSurface*> surfacePtrs;
        for (auto& surface : surfaces_) {
            surfacePtrs.push_back(surface.get());
        }
        multiRenderer_->configureOutputs(outputRegions_, surfacePtrs);
    }
    
    return true;
}

bool DRMBackend::configureOutputBlend(int outputIndex, float left, float right,
                                       float top, float bottom, float gamma) {
    if (outputIndex < 0 || outputIndex >= static_cast<int>(outputRegions_.size())) {
        LOG_ERROR << "DRMBackend::configureOutputBlend: Invalid output index " << outputIndex;
        return false;
    }
    
    // Update blend configuration
    outputRegions_[outputIndex].blend.left = left;
    outputRegions_[outputIndex].blend.right = right;
    outputRegions_[outputIndex].blend.top = top;
    outputRegions_[outputIndex].blend.bottom = bottom;
    outputRegions_[outputIndex].blend.gamma = gamma;
    
    LOG_INFO << "DRMBackend: Configured output " << outputIndex 
             << " blend: L=" << left << " R=" << right
             << " T=" << top << " B=" << bottom << " gamma=" << gamma;
    
    // Update MultiOutputRenderer if initialized
    if (multiRenderer_) {
        std::vector<OutputSurface*> surfacePtrs;
        for (auto& surface : surfaces_) {
            surfacePtrs.push_back(surface.get());
        }
        multiRenderer_->configureOutputs(outputRegions_, surfacePtrs);
    }
    
    return true;
}

DRMSurface* DRMBackend::getSurface(int index) {
    if (index < 0 || index >= static_cast<int>(surfaces_.size())) {
        return nullptr;
    }
    return surfaces_[index].get();
}

DRMSurface* DRMBackend::getSurface(const std::string& name) {
    for (auto& surface : surfaces_) {
        if (surface->getOutputInfo().name == name) {
            return surface.get();
        }
    }
    return nullptr;
}

DRMSurface* DRMBackend::getPrimarySurface() {
    if (primaryOutput_ >= 0 && primaryOutput_ < static_cast<int>(surfaces_.size())) {
        return surfaces_[primaryOutput_].get();
    }
    if (!surfaces_.empty()) {
        return surfaces_[0].get();
    }
    return nullptr;
}

bool DRMBackend::setOutputMode(int outputIndex, int width, int height, double refresh) {
    LOG_INFO << "DRMBackend::setOutputMode called: index=" << outputIndex 
             << " width=" << width << " height=" << height << " refresh=" << refresh;
    LOG_INFO << "  surfaces_.size()=" << surfaces_.size();
    
    if (outputIndex < 0 || outputIndex >= static_cast<int>(surfaces_.size())) {
        LOG_ERROR << "DRMBackend::setOutputMode: Invalid output index " << outputIndex;
        return false;
    }
    
    DRMSurface* surface = surfaces_[outputIndex].get();
    if (!surface) {
        LOG_ERROR << "DRMBackend::setOutputMode: No surface for output " << outputIndex;
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
    
    LOG_INFO << "DRMBackend: Changing output " << outputIndex << " from "
             << oldWidth << "x" << oldHeight << " to " << width << "x" << height;
    
    // Step 1: Update the mode in DRMOutputManager (stores mode for later use)
    // Don't call drmModeSetCrtc yet - we need a valid framebuffer first
    if (!outputManager_->prepareMode(outputIndex, width, height, refresh)) {
        LOG_ERROR << "DRMBackend::setOutputMode: Mode not available";
        return false;
    }
    
    // Step 2: Resize the GBM/EGL surface (creates new buffers, resets modeSet_=false)
    // This makes the next schedulePageFlip do a full modeset with valid framebuffer
    if (!surface->resize(width, height)) {
        LOG_ERROR << "DRMBackend::setOutputMode: Failed to resize surface";
        return false;
    }
    
    // Step 3: Update output regions
    if (outputIndex < static_cast<int>(outputRegions_.size())) {
        OutputRegion& region = outputRegions_[outputIndex];
        
        // Update physical size
        region.physicalWidth = width;
        region.physicalHeight = height;
        
        // Update canvas region if it was matching physical size
        if (region.canvasWidth == oldWidth) {
            region.canvasWidth = width;
        }
        if (region.canvasHeight == oldHeight) {
            region.canvasHeight = height;
        }
    }
    
    // Step 4: Recalculate canvas size and reconfigure renderer
    if (multiRenderer_) {
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
        for (auto& s : surfaces_) {
            surfacePtrs.push_back(s.get());
        }
        multiRenderer_->configureOutputs(outputRegions_, surfacePtrs);
    }
    
    LOG_INFO << "DRMBackend: Output " << outputIndex << " successfully changed to "
             << width << "x" << height;
    
    return true;
}

bool DRMBackend::setOutputModeByName(const std::string& name, int width, int height, double refresh) {
    int index = getOutputIndexByName(name);
    if (index < 0) {
        LOG_ERROR << "DRMBackend::setOutputModeByName: Unknown output '" << name << "'";
        return false;
    }
    return setOutputMode(index, width, height, refresh);
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
    if (!configManager_ || !outputManager_) {
        return false;
    }
    
    // Use config manager to parse and store the mode
    if (!configManager_->setResolutionPolicyFromString(mode)) {
        return false;
    }
    
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
    
    outputManager_->setResolutionMode(resMode);
    outputManager_->applyResolutionMode();
    
    return true;
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
    for (auto& surface : surfaces_) {
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
    
    for (size_t i = 0; i < surfaces_.size(); ++i) {
        DRMSurface* surface = surfaces_[i].get();
        if (!surface) continue;
        
        const OutputInfo& info = surface->getOutputInfo();
        
        OutputRegion region = OutputRegion::createDefault(
            info.name,
            static_cast<int>(i),
            info.width,
            info.height,
            canvasX,
            0  // All outputs at Y=0 (horizontal arrangement)
        );
        
        outputRegions_.push_back(region);
        
        // Next output starts after this one
        canvasX += info.width;
        
        LOG_INFO << "DRMBackend: Output region " << i << " (" << info.name << "): "
                 << region.canvasX << "," << region.canvasY << " "
                 << region.canvasWidth << "x" << region.canvasHeight;
    }
}

void DRMBackend::autoConfigureOutputs(const std::string& arrangement, int overlap) {
    outputRegions_.clear();
    
    int canvasX = 0;
    int canvasY = 0;
    
    for (size_t i = 0; i < surfaces_.size(); ++i) {
        DRMSurface* surface = surfaces_[i].get();
        if (!surface) continue;
        
        const OutputInfo& info = surface->getOutputInfo();
        
        OutputRegion region;
        region.name = info.name;
        region.index = static_cast<int>(i);
        region.canvasWidth = info.width;
        region.canvasHeight = info.height;
        region.physicalWidth = info.width;
        region.physicalHeight = info.height;
        region.enabled = true;
        
        if (arrangement == "vertical") {
            region.canvasX = 0;
            region.canvasY = canvasY;
            
            // Blend with previous output
            if (i > 0 && overlap > 0) {
                region.canvasY -= overlap;
                region.blend.top = static_cast<float>(overlap);
                outputRegions_[i-1].blend.bottom = static_cast<float>(overlap);
            }
            
            canvasY += info.height;
        } else {
            // Default: horizontal
            region.canvasX = canvasX;
            region.canvasY = 0;
            
            // Blend with previous output
            if (i > 0 && overlap > 0) {
                region.canvasX -= overlap;
                region.blend.left = static_cast<float>(overlap);
                outputRegions_[i-1].blend.right = static_cast<float>(overlap);
            }
            
            canvasX += info.width;
        }
        
        outputRegions_.push_back(region);
    }
    
    // Reconfigure if already initialized
    if (multiRenderer_ && multiRenderer_->isInitialized()) {
        std::vector<OutputSurface*> surfacePtrs;
        for (auto& surface : surfaces_) {
            surfacePtrs.push_back(surface.get());
        }
        multiRenderer_->configureOutputs(outputRegions_, surfacePtrs);
    }
    
    LOG_INFO << "DRMBackend: Auto-configured " << outputRegions_.size() 
             << " outputs (" << arrangement << ", overlap=" << overlap << ")";
}

bool DRMBackend::configureOutputRegion(const std::string& outputName, const OutputRegion& region) {
    // Find the output
    for (size_t i = 0; i < outputRegions_.size(); ++i) {
        if (outputRegions_[i].name == outputName) {
            outputRegions_[i] = region;
            outputRegions_[i].name = outputName;  // Preserve name
            outputRegions_[i].index = static_cast<int>(i);
            
            // Reconfigure if initialized
            if (multiRenderer_ && multiRenderer_->isInitialized()) {
                std::vector<OutputSurface*> surfacePtrs;
                for (auto& surface : surfaces_) {
                    surfacePtrs.push_back(surface.get());
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

} // namespace videocomposer

