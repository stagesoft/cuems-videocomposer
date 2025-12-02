/**
 * DRMSurface.cpp - Per-output DRM rendering surface implementation
 */

#include "DRMSurface.h"
#include "DRMOutputManager.h"
#include "../../utils/Logger.h"

#include <cstring>
#include <unistd.h>
#include <poll.h>

// OpenGL headers
#include <GL/gl.h>

namespace videocomposer {

DRMSurface::DRMSurface(DRMOutputManager* outputManager, int outputIndex)
    : outputManager_(outputManager)
    , outputIndex_(outputIndex)
    , width_(0)
    , height_(0)
{
}

DRMSurface::~DRMSurface() {
    cleanup();
}

bool DRMSurface::init(EGLContext sharedContext) {
    if (!outputManager_ || !outputManager_->isInitialized()) {
        LOG_ERROR << "DRMSurface: Invalid output manager";
        return false;
    }
    
    // Get connector info
    const DRMConnector* connector = outputManager_->getConnector(outputIndex_);
    if (!connector || !connector->info.connected) {
        LOG_ERROR << "DRMSurface: Output " << outputIndex_ << " not connected";
        return false;
    }
    
    connectorId_ = connector->connectorId;
    crtcId_ = connector->crtcId;
    width_ = connector->info.width;
    height_ = connector->info.height;
    
    if (width_ == 0 || height_ == 0) {
        LOG_ERROR << "DRMSurface: Invalid output dimensions";
        return false;
    }
    
    LOG_INFO << "DRMSurface: Initializing for " << connector->info.name
             << " (" << width_ << "x" << height_ << ")";
    
    // Create GBM device
    gbmDevice_ = gbm_create_device(outputManager_->getFd());
    if (!gbmDevice_) {
        LOG_ERROR << "DRMSurface: Failed to create GBM device";
        return false;
    }
    
    // Create GBM surface
    gbmSurface_ = gbm_surface_create(gbmDevice_, width_, height_,
                                      GBM_FORMAT_XRGB8888,
                                      GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!gbmSurface_) {
        LOG_ERROR << "DRMSurface: Failed to create GBM surface";
        cleanup();
        return false;
    }
    
    // Initialize EGL
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    
    if (eglGetPlatformDisplayEXT) {
        eglDisplay_ = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbmDevice_, nullptr);
    } else {
        eglDisplay_ = eglGetDisplay((EGLNativeDisplayType)gbmDevice_);
    }
    
    if (eglDisplay_ == EGL_NO_DISPLAY) {
        LOG_ERROR << "DRMSurface: Failed to get EGL display";
        cleanup();
        return false;
    }
    
    EGLint major, minor;
    if (!eglInitialize(eglDisplay_, &major, &minor)) {
        LOG_ERROR << "DRMSurface: Failed to initialize EGL";
        cleanup();
        return false;
    }
    
    LOG_INFO << "DRMSurface: EGL " << major << "." << minor;
    
    // Bind OpenGL ES or OpenGL API
    if (!eglBindAPI(EGL_OPENGL_API)) {
        LOG_WARNING << "DRMSurface: Failed to bind OpenGL API, trying OpenGL ES";
        if (!eglBindAPI(EGL_OPENGL_ES_API)) {
            LOG_ERROR << "DRMSurface: Failed to bind any OpenGL API";
            cleanup();
            return false;
        }
    }
    
    // Choose EGL config
    EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_DEPTH_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };
    
    EGLint numConfigs;
    if (!eglChooseConfig(eglDisplay_, configAttribs, &eglConfig_, 1, &numConfigs) ||
        numConfigs == 0) {
        // Try OpenGL ES
        configAttribs[13] = EGL_OPENGL_ES2_BIT;
        if (!eglChooseConfig(eglDisplay_, configAttribs, &eglConfig_, 1, &numConfigs) ||
            numConfigs == 0) {
            LOG_ERROR << "DRMSurface: Failed to choose EGL config";
            cleanup();
            return false;
        }
    }
    
    // Create EGL context
    EGLint contextAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    
    eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, sharedContext, contextAttribs);
    if (eglContext_ == EGL_NO_CONTEXT) {
        // Try simpler context
        EGLint simpleAttribs[] = { EGL_NONE };
        eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, sharedContext, simpleAttribs);
        
        if (eglContext_ == EGL_NO_CONTEXT) {
            LOG_ERROR << "DRMSurface: Failed to create EGL context";
            cleanup();
            return false;
        }
    }
    
    // Create EGL surface from GBM surface
    eglSurface_ = eglCreateWindowSurface(eglDisplay_, eglConfig_,
                                         (EGLNativeWindowType)gbmSurface_, nullptr);
    if (eglSurface_ == EGL_NO_SURFACE) {
        EGLint eglError = eglGetError();
        LOG_ERROR << "DRMSurface: Failed to create EGL surface (EGL error: " << eglError << ")";
        LOG_ERROR << "DRMSurface: This typically happens when the GPU is already in use by X11/Wayland";
        LOG_ERROR << "DRMSurface: For DRM direct rendering, run from a TTY (Ctrl+Alt+F2) without X/Wayland";
        cleanup();
        return false;
    }
    
    // Make context current to test
    if (!eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_)) {
        LOG_ERROR << "DRMSurface: Failed to make EGL context current";
        cleanup();
        return false;
    }
    
    // Log GL info
    LOG_INFO << "DRMSurface: GL Vendor: " << (const char*)glGetString(GL_VENDOR);
    LOG_INFO << "DRMSurface: GL Renderer: " << (const char*)glGetString(GL_RENDERER);
    LOG_INFO << "DRMSurface: GL Version: " << (const char*)glGetString(GL_VERSION);
    
    eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    
    initialized_ = true;
    LOG_INFO << "DRMSurface: Initialized successfully for output " << outputIndex_;
    
    return true;
}

void DRMSurface::cleanup() {
    // Wait for any pending flip
    if (flipPending_) {
        waitForFlip();
    }
    
    // Destroy framebuffers
    destroyFramebuffer(currentFb_);
    destroyFramebuffer(nextFb_);
    
    // Release current BO
    if (currentBo_ && gbmSurface_) {
        gbm_surface_release_buffer(gbmSurface_, currentBo_);
        currentBo_ = nullptr;
    }
    
    // Destroy EGL resources
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
    
    // Destroy GBM resources
    if (gbmSurface_) {
        gbm_surface_destroy(gbmSurface_);
        gbmSurface_ = nullptr;
    }
    
    if (gbmDevice_) {
        gbm_device_destroy(gbmDevice_);
        gbmDevice_ = nullptr;
    }
    
    initialized_ = false;
}

const OutputInfo& DRMSurface::getOutputInfo() const {
    static OutputInfo defaultInfo;
    const DRMConnector* conn = outputManager_->getConnector(outputIndex_);
    return conn ? conn->info : defaultInfo;
}

void DRMSurface::makeCurrent() {
    if (eglDisplay_ != EGL_NO_DISPLAY && eglContext_ != EGL_NO_CONTEXT) {
        eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_);
    }
}

void DRMSurface::releaseCurrent() {
    if (eglDisplay_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
}

void DRMSurface::swapBuffers() {
    if (eglDisplay_ != EGL_NO_DISPLAY && eglSurface_ != EGL_NO_SURFACE) {
        eglSwapBuffers(eglDisplay_, eglSurface_);
    }
}

bool DRMSurface::beginFrame() {
    if (!initialized_) {
        return false;
    }
    
    makeCurrent();
    
    // Set viewport
    glViewport(0, 0, width_, height_);
    
    return true;
}

void DRMSurface::endFrame() {
    if (!initialized_) {
        return;
    }
    
    // Swap buffers (renders to GBM surface)
    eglSwapBuffers(eglDisplay_, eglSurface_);
}

bool DRMSurface::createFramebuffer(gbm_bo* bo, Framebuffer& fb) {
    if (!bo || !outputManager_) {
        return false;
    }
    
    fb.bo = bo;
    
    uint32_t width = gbm_bo_get_width(bo);
    uint32_t height = gbm_bo_get_height(bo);
    uint32_t stride = gbm_bo_get_stride(bo);
    uint32_t handle = gbm_bo_get_handle(bo).u32;
    uint32_t format = gbm_bo_get_format(bo);
    
    // Determine depth and bpp from format
    uint32_t depth = 24;
    uint32_t bpp = 32;
    
    if (format == GBM_FORMAT_ARGB8888 || format == GBM_FORMAT_XRGB8888) {
        depth = 24;
        bpp = 32;
    }
    
    // Create DRM framebuffer
    int ret = drmModeAddFB(outputManager_->getFd(), width, height,
                           depth, bpp, stride, handle, &fb.fbId);
    
    if (ret != 0) {
        LOG_ERROR << "DRMSurface: Failed to create framebuffer: " << strerror(-ret);
        fb.bo = nullptr;
        fb.fbId = 0;
        return false;
    }
    
    return true;
}

void DRMSurface::destroyFramebuffer(Framebuffer& fb) {
    if (fb.fbId && outputManager_) {
        drmModeRmFB(outputManager_->getFd(), fb.fbId);
    }
    fb.fbId = 0;
    fb.bo = nullptr;
}

bool DRMSurface::schedulePageFlip() {
    if (!initialized_ || !outputManager_ || flipPending_) {
        return false;
    }
    
    // Lock front buffer from GBM surface
    gbm_bo* bo = gbm_surface_lock_front_buffer(gbmSurface_);
    if (!bo) {
        LOG_ERROR << "DRMSurface: Failed to lock front buffer";
        return false;
    }
    
    // Check if we need to create a new framebuffer
    if (nextFb_.bo != bo) {
        destroyFramebuffer(nextFb_);
        if (!createFramebuffer(bo, nextFb_)) {
            gbm_surface_release_buffer(gbmSurface_, bo);
            return false;
        }
    }
    
    // Schedule page flip
    int ret = drmModePageFlip(outputManager_->getFd(), crtcId_,
                              nextFb_.fbId, DRM_MODE_PAGE_FLIP_EVENT, this);
    
    if (ret != 0) {
        LOG_ERROR << "DRMSurface: Page flip failed: " << strerror(-ret);
        
        // Fallback: try drmModeSetCrtc instead
        const DRMConnector* conn = outputManager_->getConnector(outputIndex_);
        if (conn && conn->savedCrtc) {
            ret = drmModeSetCrtc(outputManager_->getFd(), crtcId_,
                                nextFb_.fbId, 0, 0, &connectorId_, 1,
                                &conn->savedCrtc->mode);
            if (ret != 0) {
                LOG_ERROR << "DRMSurface: SetCrtc fallback failed: " << strerror(-ret);
                gbm_surface_release_buffer(gbmSurface_, bo);
                return false;
            }
            // No flip pending in fallback mode
            return true;
        }
        
        gbm_surface_release_buffer(gbmSurface_, bo);
        return false;
    }
    
    flipPending_ = true;
    
    // Release previous buffer
    if (currentBo_) {
        gbm_surface_release_buffer(gbmSurface_, currentBo_);
    }
    currentBo_ = bo;
    
    // Swap framebuffers
    std::swap(currentFb_, nextFb_);
    
    return true;
}

void DRMSurface::waitForFlip() {
    if (!flipPending_ || !outputManager_) {
        return;
    }
    
    int fd = outputManager_->getFd();
    
    // Wait for page flip event
    struct pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    
    drmEventContext evctx = {};
    evctx.version = 2;
    evctx.page_flip_handler = pageFlipHandler;
    
    while (flipPending_) {
        int ret = poll(&pfd, 1, 1000);  // 1 second timeout
        
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_ERROR << "DRMSurface: Poll error: " << strerror(errno);
            break;
        }
        
        if (ret == 0) {
            LOG_WARNING << "DRMSurface: Page flip timeout";
            break;
        }
        
        if (pfd.revents & POLLIN) {
            drmHandleEvent(fd, &evctx);
        }
    }
    
    flipPending_ = false;
}

void DRMSurface::pageFlipHandler(int fd, unsigned int frame,
                                  unsigned int sec, unsigned int usec,
                                  void* data) {
    DRMSurface* surface = static_cast<DRMSurface*>(data);
    if (surface) {
        surface->flipPending_ = false;
    }
}

} // namespace videocomposer

