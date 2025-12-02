/**
 * HeadlessDisplay.cpp - Headless rendering implementation
 */

#include "HeadlessDisplay.h"

#ifdef HAVE_DRM_BACKEND

#include "OpenGLRenderer.h"
#include "../output/FrameCapture.h"
#include "../layer/LayerManager.h"
#include "../layer/VideoLayer.h"
#include "../osd/OSDManager.h"
#include "../utils/Logger.h"

#include <GL/glew.h>
#include <gbm.h>

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <cstring>

#ifdef HAVE_VAAPI_INTEROP
#include <va/va.h>
#include <va/va_drm.h>
#endif

namespace videocomposer {

HeadlessDisplay::HeadlessDisplay() {
}

HeadlessDisplay::~HeadlessDisplay() {
    closeWindow();
}

bool HeadlessDisplay::openWindow() {
    if (initialized_) {
        LOG_WARNING << "HeadlessDisplay: Already initialized";
        return true;
    }
    
    LOG_INFO << "HeadlessDisplay: Initializing " << width_ << "x" << height_;
    
    // Initialize DRM
    if (!initDRM()) {
        LOG_ERROR << "HeadlessDisplay: Failed to initialize DRM";
        return false;
    }
    
    // Initialize EGL
    if (!initEGL()) {
        LOG_ERROR << "HeadlessDisplay: Failed to initialize EGL";
        closeWindow();
        return false;
    }
    
    // Make context current
    makeCurrent();
    
    // Initialize GLEW
    glewExperimental = GL_TRUE;
    GLenum glewErr = glewInit();
    if (glewErr != GLEW_OK) {
        LOG_ERROR << "HeadlessDisplay: GLEW init failed: " << glewGetErrorString(glewErr);
        closeWindow();
        return false;
    }
    
    // Create offscreen surface
    if (!createOffscreenSurface(width_, height_)) {
        LOG_ERROR << "HeadlessDisplay: Failed to create offscreen surface";
        closeWindow();
        return false;
    }
    
    // Initialize EGL extensions
    initEGLExtensions();
    
    // Initialize VAAPI
#ifdef HAVE_VAAPI_INTEROP
    initVAAPI();
#endif
    
    // Create renderer
    renderer_ = std::make_unique<OpenGLRenderer>();
    if (!renderer_->init()) {
        LOG_WARNING << "HeadlessDisplay: OpenGLRenderer init failed";
    }
    
    // Create frame capture
    frameCapture_ = std::make_unique<FrameCapture>();
    
    clearCurrent();
    
    initialized_ = true;
    LOG_INFO << "HeadlessDisplay: Initialized successfully";
    LOG_INFO << "HeadlessDisplay: GL Vendor: " << (const char*)glGetString(GL_VENDOR);
    LOG_INFO << "HeadlessDisplay: GL Renderer: " << (const char*)glGetString(GL_RENDERER);
    
    return true;
}

void HeadlessDisplay::closeWindow() {
    if (!initialized_ && drmFd_ < 0) {
        return;
    }
    
    LOG_INFO << "HeadlessDisplay: Closing";
    
#ifdef HAVE_VAAPI_INTEROP
    if (vaDisplay_) {
        vaTerminate(vaDisplay_);
        vaDisplay_ = nullptr;
    }
#endif
    
    frameCapture_.reset();
    renderer_.reset();
    
    destroyOffscreenSurface();
    
    // Cleanup EGL
    if (eglDisplay_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        
        if (eglSurface_ != EGL_NO_SURFACE) {
            eglDestroySurface(eglDisplay_, eglSurface_);
            eglSurface_ = EGL_NO_SURFACE;
        }
        
        if (eglContext_ != EGL_NO_CONTEXT) {
            eglDestroyContext(eglDisplay_, eglContext_);
            eglContext_ = EGL_NO_CONTEXT;
        }
        
        eglTerminate(eglDisplay_);
        eglDisplay_ = EGL_NO_DISPLAY;
    }
    
    // Cleanup GBM
    if (gbmDevice_) {
        gbm_device_destroy(gbmDevice_);
        gbmDevice_ = nullptr;
    }
    
    // Close DRM
    if (drmFd_ >= 0) {
        close(drmFd_);
        drmFd_ = -1;
    }
    
    initialized_ = false;
}

void HeadlessDisplay::render(LayerManager* layerManager, OSDManager* osdManager) {
    (void)osdManager;  // OSD rendering handled separately
    
    if (!initialized_) {
        return;
    }
    
    makeCurrent();
    
    // Bind offscreen FBO
    glBindFramebuffer(GL_FRAMEBUFFER, offscreenFBO_);
    
    // Set viewport
    glViewport(0, 0, width_, height_);
    
    // Clear
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Render layers
    if (renderer_ && layerManager) {
        // Render each visible layer
        for (size_t i = 0; i < layerManager->getLayerCount(); ++i) {
            VideoLayer* layer = layerManager->getLayer(static_cast<int>(i));
            if (layer && layer->properties().visible && layer->isReady()) {
                renderer_->renderLayer(layer);
            }
        }
    }
    
    // Unbind FBO
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    clearCurrent();
}

void HeadlessDisplay::resize(unsigned int width, unsigned int height) {
    if (width == 0 || height == 0) {
        return;
    }
    
    if (static_cast<int>(width) == width_ && static_cast<int>(height) == height_) {
        return;
    }
    
    width_ = static_cast<int>(width);
    height_ = static_cast<int>(height);
    
    if (initialized_) {
        makeCurrent();
        destroyOffscreenSurface();
        createOffscreenSurface(width_, height_);
        clearCurrent();
    }
}

void HeadlessDisplay::getWindowSize(unsigned int* width, unsigned int* height) const {
    *width = static_cast<unsigned int>(width_);
    *height = static_cast<unsigned int>(height_);
}

void* HeadlessDisplay::getContext() {
    return eglContext_;
}

void HeadlessDisplay::makeCurrent() {
    if (eglDisplay_ != EGL_NO_DISPLAY && eglContext_ != EGL_NO_CONTEXT) {
        // For surfaceless, we use EGL_NO_SURFACE
        // For pbuffer, we use eglSurface_
        eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_);
    }
}

void HeadlessDisplay::clearCurrent() {
    if (eglDisplay_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
}

OpenGLRenderer* HeadlessDisplay::getRenderer() {
    return renderer_.get();
}

#ifdef HAVE_EGL
bool HeadlessDisplay::hasVaapiSupport() const {
#ifdef HAVE_VAAPI_INTEROP
    return vaDisplay_ != nullptr;
#else
    return false;
#endif
}
#endif

bool HeadlessDisplay::initDRM() {
    // Try to find a render node
    DIR* dir = opendir("/dev/dri");
    if (!dir) {
        LOG_ERROR << "HeadlessDisplay: Cannot open /dev/dri";
        return false;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Prefer renderD* nodes for headless
        if (strncmp(entry->d_name, "renderD", 7) == 0) {
            std::string path = std::string("/dev/dri/") + entry->d_name;
            drmFd_ = open(path.c_str(), O_RDWR | O_CLOEXEC);
            if (drmFd_ >= 0) {
                LOG_INFO << "HeadlessDisplay: Opened render node " << path;
                break;
            }
        }
    }
    
    // Fallback to card* if no render node
    if (drmFd_ < 0) {
        rewinddir(dir);
        while ((entry = readdir(dir)) != nullptr) {
            if (strncmp(entry->d_name, "card", 4) == 0) {
                std::string path = std::string("/dev/dri/") + entry->d_name;
                drmFd_ = open(path.c_str(), O_RDWR | O_CLOEXEC);
                if (drmFd_ >= 0) {
                    LOG_INFO << "HeadlessDisplay: Opened card " << path;
                    break;
                }
            }
        }
    }
    
    closedir(dir);
    
    if (drmFd_ < 0) {
        LOG_ERROR << "HeadlessDisplay: No DRM device available";
        return false;
    }
    
    // Create GBM device
    gbmDevice_ = gbm_create_device(drmFd_);
    if (!gbmDevice_) {
        LOG_ERROR << "HeadlessDisplay: Failed to create GBM device";
        close(drmFd_);
        drmFd_ = -1;
        return false;
    }
    
    return true;
}

bool HeadlessDisplay::initEGL() {
    // Get EGL display from GBM device
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    
    if (eglGetPlatformDisplayEXT) {
        eglDisplay_ = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbmDevice_, nullptr);
    } else {
        eglDisplay_ = eglGetDisplay((EGLNativeDisplayType)gbmDevice_);
    }
    
    if (eglDisplay_ == EGL_NO_DISPLAY) {
        LOG_ERROR << "HeadlessDisplay: Failed to get EGL display";
        return false;
    }
    
    // Initialize EGL
    EGLint major, minor;
    if (!eglInitialize(eglDisplay_, &major, &minor)) {
        LOG_ERROR << "HeadlessDisplay: Failed to initialize EGL";
        return false;
    }
    
    LOG_INFO << "HeadlessDisplay: EGL " << major << "." << minor;
    
    // Bind OpenGL API
    if (!eglBindAPI(EGL_OPENGL_API)) {
        LOG_WARNING << "HeadlessDisplay: Failed to bind OpenGL API, trying ES";
        if (!eglBindAPI(EGL_OPENGL_ES_API)) {
            LOG_ERROR << "HeadlessDisplay: Failed to bind any GL API";
            return false;
        }
    }
    
    // Check for surfaceless extension
    const char* extensions = eglQueryString(eglDisplay_, EGL_EXTENSIONS);
    bool hasSurfaceless = extensions && strstr(extensions, "EGL_KHR_surfaceless_context");
    
    // Choose config - we need a surfaceless or pbuffer config
    EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, hasSurfaceless ? 0 : EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };
    
    EGLint numConfigs;
    if (!eglChooseConfig(eglDisplay_, configAttribs, &eglConfig_, 1, &numConfigs) ||
        numConfigs == 0) {
        // Try with GL ES
        configAttribs[9] = EGL_OPENGL_ES2_BIT;
        if (!eglChooseConfig(eglDisplay_, configAttribs, &eglConfig_, 1, &numConfigs) ||
            numConfigs == 0) {
            LOG_ERROR << "HeadlessDisplay: No suitable EGL config";
            return false;
        }
    }
    
    // Create context
    EGLint contextAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    
    eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, EGL_NO_CONTEXT, contextAttribs);
    if (eglContext_ == EGL_NO_CONTEXT) {
        // Try simpler context
        EGLint simpleAttribs[] = { EGL_NONE };
        eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, EGL_NO_CONTEXT, simpleAttribs);
        
        if (eglContext_ == EGL_NO_CONTEXT) {
            LOG_ERROR << "HeadlessDisplay: Failed to create EGL context";
            return false;
        }
    }
    
    // Create pbuffer surface if not using surfaceless
    if (!hasSurfaceless) {
        EGLint pbufferAttribs[] = {
            EGL_WIDTH, width_,
            EGL_HEIGHT, height_,
            EGL_NONE
        };
        
        eglSurface_ = eglCreatePbufferSurface(eglDisplay_, eglConfig_, pbufferAttribs);
        if (eglSurface_ == EGL_NO_SURFACE) {
            LOG_WARNING << "HeadlessDisplay: Failed to create pbuffer, using surfaceless";
            eglSurface_ = EGL_NO_SURFACE;
        }
    } else {
        eglSurface_ = EGL_NO_SURFACE;
    }
    
    return true;
}

bool HeadlessDisplay::createOffscreenSurface(int width, int height) {
    // Create texture
    glGenTextures(1, &offscreenTexture_);
    glBindTexture(GL_TEXTURE_2D, offscreenTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    
    // Create depth renderbuffer
    glGenRenderbuffers(1, &depthRBO_);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRBO_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    
    // Create FBO
    glGenFramebuffers(1, &offscreenFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, offscreenFBO_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, offscreenTexture_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, depthRBO_);
    
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR << "HeadlessDisplay: FBO incomplete: " << status;
        destroyOffscreenSurface();
        return false;
    }
    
    LOG_INFO << "HeadlessDisplay: Created offscreen surface " << width << "x" << height;
    return true;
}

void HeadlessDisplay::destroyOffscreenSurface() {
    if (offscreenFBO_) {
        glDeleteFramebuffers(1, &offscreenFBO_);
        offscreenFBO_ = 0;
    }
    if (offscreenTexture_) {
        glDeleteTextures(1, &offscreenTexture_);
        offscreenTexture_ = 0;
    }
    if (depthRBO_) {
        glDeleteRenderbuffers(1, &depthRBO_);
        depthRBO_ = 0;
    }
}

void HeadlessDisplay::initEGLExtensions() {
#ifdef HAVE_EGL
    eglCreateImageKHR_ = (PFNEGLCREATEIMAGEKHRPROC)
        eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR_ = (PFNEGLDESTROYIMAGEKHRPROC)
        eglGetProcAddress("eglDestroyImageKHR");
    glEGLImageTargetTexture2DOES_ = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    
    if (eglCreateImageKHR_ && eglDestroyImageKHR_ && glEGLImageTargetTexture2DOES_) {
        LOG_INFO << "HeadlessDisplay: EGL image extensions available";
    }
#endif
}

void HeadlessDisplay::initVAAPI() {
#ifdef HAVE_VAAPI_INTEROP
    if (drmFd_ < 0) {
        return;
    }
    
    vaDisplay_ = vaGetDisplayDRM(drmFd_);
    if (!vaDisplay_) {
        LOG_WARNING << "HeadlessDisplay: Failed to get VAAPI display";
        return;
    }
    
    int major, minor;
    VAStatus status = vaInitialize(vaDisplay_, &major, &minor);
    if (status != VA_STATUS_SUCCESS) {
        LOG_WARNING << "HeadlessDisplay: Failed to initialize VAAPI: " << vaErrorStr(status);
        vaDisplay_ = nullptr;
        return;
    }
    
    LOG_INFO << "HeadlessDisplay: VAAPI initialized (version " << major << "." << minor << ")";
#endif
}

} // namespace videocomposer

#endif // HAVE_DRM_BACKEND

