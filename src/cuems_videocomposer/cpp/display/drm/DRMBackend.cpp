/**
 * DRMBackend.cpp - DRM/KMS display backend implementation
 */

#include "DRMBackend.h"
#include "../OpenGLRenderer.h"
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
    
    // Create OpenGL renderer using primary surface
    DRMSurface* primary = getPrimarySurface();
    if (primary) {
        primary->makeCurrent();
        
        renderer_ = std::make_unique<OpenGLRenderer>();
        if (!renderer_->init()) {
            LOG_ERROR << "DRMBackend: Failed to initialize OpenGL renderer";
            // Continue anyway - rendering might still work
        }
        
        primary->releaseCurrent();
    }
    
    initialized_ = true;
    LOG_INFO << "DRMBackend: Initialized with " << surfaces_.size() << " output(s)";
    
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
    
    renderer_.reset();
    
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
    (void)osdManager;  // OSD rendering handled separately
    
    if (!initialized_ || surfaces_.empty()) {
        return;
    }
    
    // Render to each output
    for (auto& surface : surfaces_) {
        if (!surface->isInitialized()) {
            continue;
        }
        
        // Begin frame
        if (!surface->beginFrame()) {
            continue;
        }
        
        // Clear with visible test color for first few frames
        static int frameCount = 0;
        if (frameCount < 60) {
            // Cycle through colors: Red -> Green -> Blue
            float r = (frameCount % 3 == 0) ? 1.0f : 0.0f;
            float g = (frameCount % 3 == 1) ? 1.0f : 0.0f;
            float b = (frameCount % 3 == 2) ? 1.0f : 0.0f;
            glClearColor(r, g, b, 1.0f);
            frameCount++;
        } else {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        }
        glClear(GL_COLOR_BUFFER_BIT);
        
        // Render layers (use getLayersSortedByZOrder like X11/Wayland backends do)
        if (renderer_ && layerManager) {
            auto layers = layerManager->getLayersSortedByZOrder();
            size_t renderedCount = 0;
            
            for (size_t i = 0; i < layers.size(); ++i) {
                VideoLayer* layer = layers[i];
                if (layer) {
                    bool visible = layer->properties().visible;
                    bool ready = layer->isReady();
                    
                    // Debug: log first few frames
                    static int debugCounter = 0;
                    if (debugCounter < 10) {
                        LOG_INFO << "DRMBackend: Layer " << layer->getLayerId() << " - visible=" << visible 
                                << ", ready=" << ready << ", frame=" << layer->getCurrentFrame();
                        debugCounter++;
                    }
                    
                    if (visible && ready) {
                        bool renderOk = renderer_->renderLayer(layer);
                        if (renderOk) {
                            renderedCount++;
                        } else {
                            static int renderFailCounter = 0;
                            if (renderFailCounter++ < 5) {
                                LOG_ERROR << "DRMBackend: renderLayer() returned false for layer " << layer->getLayerId();
                            }
                        }
                    }
                }
            }
            
            static bool firstRender = true;
            if (firstRender && !layers.empty()) {
                LOG_INFO << "DRMBackend: First render pass - " << layers.size() << " layer(s), " 
                        << renderedCount << " rendered";
                firstRender = false;
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
#endif

#ifdef HAVE_VAAPI_INTEROP
VADisplay DRMBackend::getVADisplay() const {
    return vaDisplay_;
}
#endif

const std::vector<OutputInfo>& DRMBackend::getOutputs() const {
    return outputManager_->getOutputs();
}

size_t DRMBackend::getOutputCount() const {
    return surfaces_.size();
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

bool DRMBackend::setOutputMode(int index, int width, int height, double refresh) {
    return outputManager_->setMode(index, width, height, refresh);
}

bool DRMBackend::setOutputMode(const std::string& name, int width, int height, double refresh) {
    return outputManager_->setMode(name, width, height, refresh);
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
    
    if (eglCreateImageKHR_ && eglDestroyImageKHR_ && glEGLImageTargetTexture2DOES_) {
        LOG_INFO << "DRMBackend: EGL image extensions available";
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

} // namespace videocomposer

