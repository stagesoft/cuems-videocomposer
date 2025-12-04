/**
 * DRMSurface.cpp - Per-output DRM rendering surface implementation
 */

#include "DRMSurface.h"
#include "DRMOutputManager.h"
#include "../../utils/Logger.h"

#include <cstring>
#include <string>
#include <unistd.h>
#include <poll.h>
#include <iomanip>

// DRM fourcc and modifiers
#include <drm_fourcc.h>

// OpenGL headers
#include <GL/gl.h>

namespace videocomposer {

DRMSurface::DRMSurface(DRMOutputManager* outputManager, const std::string& outputName)
    : outputManager_(outputManager)
    , outputName_(outputName)
    , width_(0)
    , height_(0)
{
}

DRMSurface::~DRMSurface() {
    cleanup();
}

bool DRMSurface::init(EGLContext sharedContext, EGLDisplay sharedDisplay, gbm_device* sharedGbmDevice) {
    if (!outputManager_ || !outputManager_->isInitialized()) {
        LOG_ERROR << "DRMSurface: Invalid output manager";
        return false;
    }
    
    // Get connector info by name
    const DRMConnector* connector = outputManager_->getConnectorByName(outputName_);
    if (!connector || !connector->info.connected) {
        LOG_ERROR << "DRMSurface: Output " << outputName_ << " not connected";
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
    
    // Use shared GBM device if provided, otherwise create new one
    if (sharedGbmDevice) {
        gbmDevice_ = sharedGbmDevice;
        ownGbmDevice_ = false;
        LOG_INFO << "DRMSurface: Using shared GBM device";
    } else {
    gbmDevice_ = gbm_create_device(outputManager_->getFd());
    if (!gbmDevice_) {
        LOG_ERROR << "DRMSurface: Failed to create GBM device";
        LOG_ERROR << "DRMSurface: Ensure the GPU driver supports GBM (Mesa or NVIDIA 495+)";
        return false;
        }
        ownGbmDevice_ = true;
    }
    
    // Log GBM backend info
    const char* gbmBackend = gbm_device_get_backend_name(gbmDevice_);
    LOG_INFO << "DRMSurface: GBM backend: " << (gbmBackend ? gbmBackend : "unknown");
    
    // Check if this is NVIDIA - it requires explicit modifiers
    bool isNvidia = gbmBackend && (std::string(gbmBackend).find("nvidia") != std::string::npos);
    if (isNvidia) {
        LOG_INFO << "DRMSurface: NVIDIA GBM detected - will use explicit modifiers";
    }
    
    // TODO: Implement full mpv-style IN_FORMATS probing for all drivers.
    // Currently we only use explicit modifiers for NVIDIA. A more robust approach
    // (like mpv does) would be to:
    //   1. Query the DRM plane's "IN_FORMATS" property blob
    //   2. Parse all supported format+modifier combinations from drm_format_modifier_blob
    //   3. Use gbm_surface_create_with_modifiers() with the probed modifiers
    // This would provide optimal tiled formats on Intel/AMD and proper support for all
    // NVIDIA modifier variants. See mpv's probe_gbm_modifiers() in context_drm_egl.c
    
    // Formats to try - prefer ARGB for NVIDIA (better compatibility)
    static const struct {
        uint32_t format;
        const char* name;
    } formats[] = {
        { GBM_FORMAT_ARGB8888, "ARGB8888" },
        { GBM_FORMAT_XRGB8888, "XRGB8888" },
        { GBM_FORMAT_ABGR8888, "ABGR8888" },
        { GBM_FORMAT_XBGR8888, "XBGR8888" },
    };
    
    // Track the format we successfully created
    uint32_t gbmFormat = 0;
    
    // NVIDIA requires explicit modifiers - try gbm_surface_create_with_modifiers first
    // DRM_FORMAT_MOD_LINEAR (0x0) is universally supported
    if (isNvidia) {
        // Modifiers to try - LINEAR is safest, then NVIDIA tiled formats
        uint64_t modifiers[] = {
            DRM_FORMAT_MOD_LINEAR,  // 0x0 - always works
            DRM_FORMAT_MOD_INVALID  // Sentinel
        };
        
        for (const auto& fmt : formats) {
            // Try with LINEAR modifier first (most compatible)
            gbmSurface_ = gbm_surface_create_with_modifiers(
                gbmDevice_, width_, height_, fmt.format,
                modifiers, 1);  // Just LINEAR
            
            if (gbmSurface_) {
                gbmFormat = fmt.format;
                LOG_INFO << "DRMSurface: Created GBM surface with format " << fmt.name 
                         << " using DRM_FORMAT_MOD_LINEAR (NVIDIA path)";
                goto gbm_surface_created;
            }
        }
        
        LOG_WARNING << "DRMSurface: gbm_surface_create_with_modifiers failed, trying legacy path";
    }
    
    // Legacy path - try without explicit modifiers (works on Mesa/Intel/AMD)
    static const struct {
        uint32_t flags;
        const char* name;
    } usageFlags[] = {
        { GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING, "SCANOUT|RENDERING" },
        { GBM_BO_USE_RENDERING, "RENDERING only" },
        { GBM_BO_USE_SCANOUT, "SCANOUT only" },
        { 0, "no flags" },  // Last resort
    };
    
    for (const auto& fmt : formats) {
        for (const auto& usage : usageFlags) {
            gbmSurface_ = gbm_surface_create(gbmDevice_, width_, height_, fmt.format, usage.flags);
            if (gbmSurface_) {
                gbmFormat = fmt.format;
                LOG_INFO << "DRMSurface: Created GBM surface with format " << fmt.name 
                         << ", usage " << usage.name;
                goto gbm_surface_created;
            }
        }
    }
    
    // All attempts failed
    LOG_ERROR << "DRMSurface: Failed to create GBM surface with any format/usage combination";
    LOG_ERROR << "DRMSurface: This may indicate:";
    LOG_ERROR << "DRMSurface:   - GPU driver doesn't support GBM scanout";
    LOG_ERROR << "DRMSurface:   - Another process (X11/Wayland) holds the display";
    LOG_ERROR << "DRMSurface:   - NVIDIA: ensure nvidia-drm.modeset=1 is set";
    if (isNvidia) {
        LOG_ERROR << "DRMSurface:   - NVIDIA: try running from a TTY without X11/Wayland";
    }
    cleanup();
    return false;
    
gbm_surface_created:
    
    // Use shared EGL display if provided, otherwise create new one
    bool usingPlatformDisplay = false;
    
    if (sharedDisplay != EGL_NO_DISPLAY) {
        eglDisplay_ = sharedDisplay;
        ownEglDisplay_ = false;
        LOG_INFO << "DRMSurface: Using shared EGL display";
        // Assume platform display if shared
        usingPlatformDisplay = true;
    } else {
        // Initialize EGL - prefer platform-aware functions for GBM
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    
    if (eglGetPlatformDisplayEXT) {
        eglDisplay_ = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, gbmDevice_, nullptr);
            if (eglDisplay_ != EGL_NO_DISPLAY) {
                usingPlatformDisplay = true;
            }
        }
        
        if (eglDisplay_ == EGL_NO_DISPLAY) {
            // Fallback to legacy path
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
        ownEglDisplay_ = true;
    }
    
    // Also get the platform window surface function - required for GBM on Mesa/Intel
    PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC eglCreatePlatformWindowSurfaceEXT =
        (PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
    
    // Bind OpenGL ES or OpenGL API
    if (!eglBindAPI(EGL_OPENGL_API)) {
        LOG_WARNING << "DRMSurface: Failed to bind OpenGL API, trying OpenGL ES";
        if (!eglBindAPI(EGL_OPENGL_ES_API)) {
            LOG_ERROR << "DRMSurface: Failed to bind any OpenGL API";
            cleanup();
            return false;
        }
    }
    
    // Log the GBM format we're using
    LOG_INFO << "DRMSurface: GBM surface format: 0x" << std::hex << gbmFormat << std::dec;
    
    // Choose EGL config that matches GBM format
    // For GBM, we need to find a config with matching EGL_NATIVE_VISUAL_ID
    EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, (gbmFormat == GBM_FORMAT_ARGB8888 || gbmFormat == GBM_FORMAT_ABGR8888) ? 8 : 0,
        EGL_DEPTH_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };
    
    // Get all matching configs, then find one with matching native visual
    EGLConfig configs[64];
    EGLint numConfigs;
    if (!eglChooseConfig(eglDisplay_, configAttribs, configs, 64, &numConfigs) || numConfigs == 0) {
        // Try OpenGL ES
        configAttribs[13] = EGL_OPENGL_ES2_BIT;
        if (!eglChooseConfig(eglDisplay_, configAttribs, configs, 64, &numConfigs) || numConfigs == 0) {
            LOG_ERROR << "DRMSurface: Failed to choose EGL config";
            cleanup();
            return false;
        }
    }
    
    LOG_INFO << "DRMSurface: Found " << numConfigs << " matching EGL configs";
    
    // Find config with matching native visual ID (GBM format)
    eglConfig_ = nullptr;
    for (int i = 0; i < numConfigs; i++) {
        EGLint visualId;
        if (eglGetConfigAttrib(eglDisplay_, configs[i], EGL_NATIVE_VISUAL_ID, &visualId)) {
            if (static_cast<uint32_t>(visualId) == gbmFormat) {
                eglConfig_ = configs[i];
                LOG_INFO << "DRMSurface: Found matching EGL config (visual 0x" 
                         << std::hex << visualId << std::dec << ")";
                break;
            }
        }
    }
    
    // Fallback: use first config if no exact match
    if (!eglConfig_) {
        eglConfig_ = configs[0];
        EGLint visualId;
        eglGetConfigAttrib(eglDisplay_, eglConfig_, EGL_NATIVE_VISUAL_ID, &visualId);
        LOG_WARNING << "DRMSurface: No exact format match, using config with visual 0x" 
                    << std::hex << visualId << std::dec;
    }
    
    // Create or reuse EGL context
    // IMPORTANT: All surfaces must share the SAME context (not just share objects)
    // because VAOs are context-specific and not shared between contexts
    if (sharedContext != EGL_NO_CONTEXT) {
        // Use the shared context directly - all surfaces share one context
        eglContext_ = sharedContext;
        ownEglContext_ = false;
        LOG_INFO << "DRMSurface: Using shared EGL context (VAOs will work across surfaces)";
    } else {
        // Create new context for first surface
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
                LOG_ERROR << "DRMSurface: Failed to create EGL context";
                cleanup();
                return false;
            }
        }
        ownEglContext_ = true;
    }
    
    // Create EGL surface from GBM surface
    // Use platform-aware function if available (required for Intel/Mesa with GBM)
    if (usingPlatformDisplay && eglCreatePlatformWindowSurfaceEXT) {
        LOG_INFO << "DRMSurface: Using eglCreatePlatformWindowSurfaceEXT for GBM surface";
        eglSurface_ = eglCreatePlatformWindowSurfaceEXT(eglDisplay_, eglConfig_, gbmSurface_, nullptr);
    } else {
        LOG_INFO << "DRMSurface: Using legacy eglCreateWindowSurface";
    eglSurface_ = eglCreateWindowSurface(eglDisplay_, eglConfig_,
                                         (EGLNativeWindowType)gbmSurface_, nullptr);
    }
    
    if (eglSurface_ == EGL_NO_SURFACE) {
        EGLint eglError = eglGetError();
        LOG_ERROR << "DRMSurface: Failed to create EGL surface (EGL error: " << eglError << ")";
        
        // Provide more specific error info
        switch (eglError) {
            case EGL_BAD_NATIVE_WINDOW:  // 0x3009 = 12297
                LOG_ERROR << "DRMSurface: EGL_BAD_NATIVE_WINDOW - GBM surface not accepted as window";
                LOG_ERROR << "DRMSurface: This can happen when:";
                LOG_ERROR << "DRMSurface:   - GPU is already in use by X11/Wayland";
                LOG_ERROR << "DRMSurface:   - EGL config incompatible with GBM surface format";
                LOG_ERROR << "DRMSurface:   - Driver doesn't support EGL_PLATFORM_GBM";
                break;
            case EGL_BAD_MATCH:
                LOG_ERROR << "DRMSurface: EGL_BAD_MATCH - Config/surface format mismatch";
                break;
            case EGL_BAD_ALLOC:
                LOG_ERROR << "DRMSurface: EGL_BAD_ALLOC - Cannot allocate surface resources";
                break;
            default:
                LOG_ERROR << "DRMSurface: Check EGL error code: " << eglError;
                break;
        }
        
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
    LOG_INFO << "DRMSurface: Initialized successfully for " << outputName_;
    
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
        
        // Only destroy context if we created it
        if (eglContext_ != EGL_NO_CONTEXT && ownEglContext_) {
            eglDestroyContext(eglDisplay_, eglContext_);
        }
        eglContext_ = EGL_NO_CONTEXT;
        ownEglContext_ = false;
        
        // Only terminate if we created it
        if (ownEglDisplay_) {
        eglTerminate(eglDisplay_);
        }
        eglDisplay_ = EGL_NO_DISPLAY;
        ownEglDisplay_ = false;
    }
    
    // Destroy GBM resources
    if (gbmSurface_) {
        gbm_surface_destroy(gbmSurface_);
        gbmSurface_ = nullptr;
    }
    
    // Only destroy GBM device if we created it
    if (gbmDevice_ && ownGbmDevice_) {
        gbm_device_destroy(gbmDevice_);
    }
    gbmDevice_ = nullptr;
    ownGbmDevice_ = false;
    
    initialized_ = false;
}

bool DRMSurface::resize(int width, int height) {
    LOG_INFO << "DRMSurface::resize called: " << width << "x" << height;
    LOG_INFO << "  outputManager_=" << (void*)outputManager_ << " gbmDevice_=" << (void*)gbmDevice_;
    
    if (!outputManager_ || !gbmDevice_) {
        LOG_ERROR << "DRMSurface::resize: Not properly initialized";
        return false;
    }
    
    if (width <= 0 || height <= 0) {
        LOG_ERROR << "DRMSurface::resize: Invalid dimensions " << width << "x" << height;
        return false;
    }
    
    if (width == static_cast<int>(width_) && height == static_cast<int>(height_)) {
        LOG_INFO << "DRMSurface::resize: Already at " << width << "x" << height;
        return true;
    }
    
    LOG_INFO << "DRMSurface::resize: Resizing from " << width_ << "x" << height_ 
             << " to " << width << "x" << height;
    
    // Wait for pending flip
    if (flipPending_) {
        waitForFlip();
    }
    
    // Destroy old framebuffers
    destroyFramebuffer(currentFb_);
    destroyFramebuffer(nextFb_);
    
    // Release current BO
    if (currentBo_ && gbmSurface_) {
        gbm_surface_release_buffer(gbmSurface_, currentBo_);
        currentBo_ = nullptr;
    }
    
    // Destroy old EGL surface
    if (eglDisplay_ != EGL_NO_DISPLAY && eglSurface_ != EGL_NO_SURFACE) {
        eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(eglDisplay_, eglSurface_);
        eglSurface_ = EGL_NO_SURFACE;
    }
    
    // Destroy old GBM surface
    if (gbmSurface_) {
        gbm_surface_destroy(gbmSurface_);
        gbmSurface_ = nullptr;
    }
    
    // Update dimensions
    width_ = static_cast<uint32_t>(width);
    height_ = static_cast<uint32_t>(height);
    
    // Reset modeSet_ flag so next schedulePageFlip does a full drmModeSetCrtc
    // instead of just drmModePageFlip (which would fail since CRTC was cleared)
    modeSet_ = false;
    
    // Check if NVIDIA (same approach as init)
    const char* gbmBackend = gbm_device_get_backend_name(gbmDevice_);
    bool isNvidia = gbmBackend && (std::string(gbmBackend).find("nvidia") != std::string::npos);
    
    // Formats to try - prefer ARGB for NVIDIA
    static const struct {
        uint32_t format;
        const char* name;
    } formats[] = {
        { GBM_FORMAT_ARGB8888, "ARGB8888" },
        { GBM_FORMAT_XRGB8888, "XRGB8888" },
        { GBM_FORMAT_ABGR8888, "ABGR8888" },
        { GBM_FORMAT_XBGR8888, "XBGR8888" },
    };
    
    // NVIDIA requires explicit modifiers (see TODO in init() for full IN_FORMATS probing)
    if (isNvidia) {
        uint64_t modifiers[] = { DRM_FORMAT_MOD_LINEAR };
        
        for (const auto& fmt : formats) {
            gbmSurface_ = gbm_surface_create_with_modifiers(
                gbmDevice_, width_, height_, fmt.format,
                modifiers, 1);
            
            if (gbmSurface_) {
                LOG_INFO << "DRMSurface::resize: Created GBM surface with format " << fmt.name 
                         << " using DRM_FORMAT_MOD_LINEAR (NVIDIA path)";
                goto gbm_resize_surface_created;
            }
        }
        LOG_WARNING << "DRMSurface::resize: NVIDIA modifiers failed, trying legacy path";
    }
    
    // Legacy path
    static const struct {
        uint32_t flags;
        const char* name;
    } usageFlags[] = {
        { GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING, "SCANOUT|RENDERING" },
        { GBM_BO_USE_RENDERING, "RENDERING only" },
        { GBM_BO_USE_SCANOUT, "SCANOUT only" },
        { 0, "no flags" },
    };
    
    for (const auto& fmt : formats) {
        for (const auto& usage : usageFlags) {
            gbmSurface_ = gbm_surface_create(gbmDevice_, width_, height_, fmt.format, usage.flags);
            if (gbmSurface_) {
                LOG_INFO << "DRMSurface::resize: Created GBM surface with format " << fmt.name 
                         << ", usage " << usage.name;
                goto gbm_resize_surface_created;
            }
        }
    }
    
    LOG_ERROR << "DRMSurface::resize: Failed to create new GBM surface with any format";
    return false;
    
gbm_resize_surface_created:
    
    // Try platform-aware EGL surface creation first, then fallback
    PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC eglCreatePlatformWindowSurfaceEXT =
        (PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC)eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT");
    
    if (eglCreatePlatformWindowSurfaceEXT) {
        eglSurface_ = eglCreatePlatformWindowSurfaceEXT(eglDisplay_, eglConfig_, gbmSurface_, nullptr);
    }
    
    if (eglSurface_ == EGL_NO_SURFACE) {
        // Fallback to legacy
        eglSurface_ = eglCreateWindowSurface(eglDisplay_, eglConfig_,
                                              (EGLNativeWindowType)gbmSurface_, nullptr);
    }
    
    if (eglSurface_ == EGL_NO_SURFACE) {
        LOG_ERROR << "DRMSurface::resize: Failed to create new EGL surface";
        gbm_surface_destroy(gbmSurface_);
        gbmSurface_ = nullptr;
        return false;
    }
    
    LOG_INFO << "DRMSurface::resize: Successfully resized to " << width << "x" << height;
    return true;
}

const OutputInfo& DRMSurface::getOutputInfo() const {
    static OutputInfo defaultInfo;
    const DRMConnector* conn = outputManager_->getConnectorByName(outputName_);
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
    uint32_t format = gbm_bo_get_format(bo);
    uint64_t modifier = gbm_bo_get_modifier(bo);
    
    // Use modern multi-plane aware API (required for NVIDIA and many modern drivers)
    uint32_t handles[4] = {0};
    uint32_t strides[4] = {0};
    uint32_t offsets[4] = {0};
    uint64_t modifiers[4] = {0};
    uint32_t flags = 0;
    
    int num_planes = gbm_bo_get_plane_count(bo);
    if (num_planes <= 0) {
        num_planes = 1;  // Fallback for older GBM
    }
    
    for (int i = 0; i < num_planes && i < 4; ++i) {
        handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
        strides[i] = gbm_bo_get_stride_for_plane(bo, i);
        offsets[i] = gbm_bo_get_offset(bo, i);
        modifiers[i] = modifier;
    }
    
    // If plane-aware functions failed, fall back to legacy single-plane
    if (handles[0] == 0) {
        handles[0] = gbm_bo_get_handle(bo).u32;
        strides[0] = gbm_bo_get_stride(bo);
        offsets[0] = 0;
    }
    
    int ret = -1;
    
    // Try with modifiers first (NVIDIA and modern drivers prefer this)
    if (modifier != 0 && modifier != DRM_FORMAT_MOD_INVALID) {
        flags = DRM_MODE_FB_MODIFIERS;
        LOG_INFO << "DRMSurface: Creating framebuffer with modifier 0x" << std::hex << modifier << std::dec;
        
        ret = drmModeAddFB2WithModifiers(outputManager_->getFd(), width, height,
                                          format, handles, strides, offsets,
                                          modifiers, &fb.fbId, flags);
        if (ret != 0) {
            LOG_WARNING << "DRMSurface: drmModeAddFB2WithModifiers failed: " << strerror(-ret) 
                       << ", trying without modifiers";
        }
    }
    
    // Try drmModeAddFB2 without modifiers
    if (ret != 0) {
        ret = drmModeAddFB2(outputManager_->getFd(), width, height,
                            format, handles, strides, offsets, &fb.fbId, 0);
        if (ret != 0) {
            LOG_WARNING << "DRMSurface: drmModeAddFB2 failed: " << strerror(-ret) 
                       << ", trying legacy API";
        }
    }
    
    // Final fallback to legacy drmModeAddFB (should rarely be needed)
    if (ret != 0) {
        uint32_t depth = 24;
        uint32_t bpp = 32;
        
        if (format == GBM_FORMAT_ARGB8888 || format == GBM_FORMAT_XRGB8888 ||
            format == GBM_FORMAT_ABGR8888 || format == GBM_FORMAT_XBGR8888) {
            depth = 24;
            bpp = 32;
        }
        
        ret = drmModeAddFB(outputManager_->getFd(), width, height,
                           depth, bpp, strides[0], handles[0], &fb.fbId);
    }
    
    if (ret != 0) {
        LOG_ERROR << "DRMSurface: Failed to create framebuffer with all methods: " << strerror(-ret);
        LOG_ERROR << "DRMSurface: Format=0x" << std::hex << format << std::dec 
                 << " Size=" << width << "x" << height;
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
    
    int ret;
    
    // First frame: use drmModeSetCrtc to set the mode and initial framebuffer
    // Subsequent frames: use drmModePageFlip for vsync'd updates
    if (!modeSet_) {
        const DRMConnector* conn = outputManager_->getConnectorByName(outputName_);
        if (!conn) {
            LOG_ERROR << "DRMSurface: No connector info for initial modeset";
            gbm_surface_release_buffer(gbmSurface_, bo);
            return false;
        }
        
        // Use currentMode if available (set after mode change), otherwise use savedCrtc
        drmModeModeInfo* mode = nullptr;
        if (conn->hasCurrentMode) {
            mode = const_cast<drmModeModeInfo*>(&conn->currentMode);
        } else if (conn->savedCrtc) {
            mode = &conn->savedCrtc->mode;
        }
        
        if (!mode) {
            LOG_ERROR << "DRMSurface: No mode available for modeset";
            gbm_surface_release_buffer(gbmSurface_, bo);
            return false;
        }
        
        LOG_INFO << "DRMSurface: Setting mode for " << conn->info.name 
                 << " (" << mode->hdisplay << "x" << mode->vdisplay << ")";
        ret = drmModeSetCrtc(outputManager_->getFd(), crtcId_,
                            nextFb_.fbId, 0, 0, &connectorId_, 1, mode);
        
        if (ret != 0) {
            LOG_ERROR << "DRMSurface: Modeset failed: " << strerror(-ret);
            gbm_surface_release_buffer(gbmSurface_, bo);
            return false;
        }
        
        modeSet_ = true;
        LOG_INFO << "DRMSurface: Modeset successful, framebuffer displayed";
        
        // Release previous buffer if any
        if (currentBo_) {
            gbm_surface_release_buffer(gbmSurface_, currentBo_);
        }
        currentBo_ = bo;
        
        // Swap framebuffers
        std::swap(currentFb_, nextFb_);
        
        // No flip pending for modeset
        return true;
    }
    
    // Subsequent frames: use page flip for vsync
    ret = drmModePageFlip(outputManager_->getFd(), crtcId_,
                              nextFb_.fbId, DRM_MODE_PAGE_FLIP_EVENT, this);
    
    if (ret != 0) {
        LOG_ERROR << "DRMSurface: Page flip failed: " << strerror(-ret) << " (errno=" << -ret << ")";
        
        // Fallback: try drmModeSetCrtc instead
        const DRMConnector* conn = outputManager_->getConnectorByName(outputName_);
        if (conn) {
            drmModeModeInfo* mode = nullptr;
            if (conn->hasCurrentMode) {
                mode = const_cast<drmModeModeInfo*>(&conn->currentMode);
            } else if (conn->savedCrtc) {
                mode = &conn->savedCrtc->mode;
            }
            
            if (mode) {
                ret = drmModeSetCrtc(outputManager_->getFd(), crtcId_,
                                    nextFb_.fbId, 0, 0, &connectorId_, 1, mode);
                if (ret != 0) {
                    LOG_ERROR << "DRMSurface: SetCrtc fallback failed: " << strerror(-ret);
                    gbm_surface_release_buffer(gbmSurface_, bo);
                    return false;
                }
                // No flip pending in fallback mode
                return true;
            }
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

