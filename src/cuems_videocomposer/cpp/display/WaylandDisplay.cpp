#include "WaylandDisplay.h"

#ifdef HAVE_WAYLAND

#include "../layer/LayerManager.h"
#include "../osd/OSDManager.h"
#include "../osd/OSDRenderer.h"
#include "../utils/Logger.h"

#include <wayland-client.h>
#include <wayland-egl.h>
#include "xdg-shell-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

#ifdef HAVE_VAAPI_INTEROP
#include <va/va.h>
#include <va/va_drm.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <cstring>

namespace videocomposer {

// Wayland registry listener
static const struct wl_registry_listener registry_listener = {
    WaylandDisplay::registryHandleGlobal,
    WaylandDisplay::registryHandleGlobalRemove
};

// XDG WM Base listener
static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    WaylandDisplay::xdgWmBasePing
};

// XDG Surface listener
static const struct xdg_surface_listener xdg_surface_listener = {
    WaylandDisplay::xdgSurfaceConfigure
};

// XDG Toplevel listener
static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    WaylandDisplay::xdgToplevelConfigure,
    WaylandDisplay::xdgToplevelClose
};

WaylandDisplay::WaylandDisplay()
    : renderer_(std::make_unique<OpenGLRenderer>())
    , osdRenderer_(std::make_unique<OSDRenderer>())
    , wlDisplay_(nullptr)
    , wlRegistry_(nullptr)
    , wlCompositor_(nullptr)
    , wlSurface_(nullptr)
    , wlEglWindow_(nullptr)
    , xdgWmBase_(nullptr)
    , xdgSurface_(nullptr)
    , xdgToplevel_(nullptr)
    , dmabuf_(nullptr)
    , presentation_(nullptr)
#ifdef HAVE_EGL
    , eglDisplay_(EGL_NO_DISPLAY)
    , eglContext_(EGL_NO_CONTEXT)
    , eglSurface_(EGL_NO_SURFACE)
    , eglConfig_(nullptr)
    , vaapiSupported_(false)
    , eglCreateImageKHR_(nullptr)
    , eglDestroyImageKHR_(nullptr)
    , glEGLImageTargetTexture2DOES_(nullptr)
#endif
#ifdef HAVE_VAAPI_INTEROP
    , vaDisplay_(nullptr)
    , vaDrmFd_(-1)
#endif
    , windowWidth_(1280)
    , windowHeight_(720)
    , windowX_(0)
    , windowY_(0)
    , fullscreen_(false)
    , ontop_(false)
    , windowOpen_(false)
    , configured_(false)
{
}

WaylandDisplay::~WaylandDisplay() {
    closeWindow();
}

bool WaylandDisplay::openWindow() {
    if (windowOpen_) {
        return true;
    }

    if (!initWayland()) {
        LOG_ERROR << "Failed to initialize Wayland";
        return false;
    }

    if (!initEGL()) {
        LOG_ERROR << "Failed to initialize EGL";
        cleanupWayland();
        return false;
    }

    if (!initVAAPI()) {
        LOG_WARNING << "Failed to initialize VAAPI - hardware decode may not work";
    }

    makeCurrent();

    // Initialize renderer
    if (!renderer_->init()) {
        LOG_ERROR << "Failed to initialize OpenGL renderer";
        clearCurrent();
        closeWindow();
        return false;
    }

    // Initialize OSD renderer
    if (!osdRenderer_->init()) {
        LOG_WARNING << "Failed to initialize OSD renderer (OSD will be disabled)";
    }

    clearCurrent();
    windowOpen_ = true;
    
    LOG_INFO << "Wayland display opened: " << windowWidth_ << "x" << windowHeight_;
    
    // Render initial black frame to make window visible
    render(nullptr, nullptr);
    
    return true;
}

void WaylandDisplay::closeWindow() {
    if (!windowOpen_) {
        return;
    }

    makeCurrent();
    renderer_->cleanup();
    if (osdRenderer_) {
        osdRenderer_->cleanup();
    }
    clearCurrent();

    cleanupVAAPI();
    cleanupEGL();
    cleanupWayland();

    windowOpen_ = false;
}

bool WaylandDisplay::isWindowOpen() const {
    return windowOpen_;
}

void WaylandDisplay::render(LayerManager* layerManager, OSDManager* osdManager) {
    if (!windowOpen_) {
        return;
    }

    makeCurrent();

    // If no layer manager, just clear to black and swap
    if (!layerManager) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFlush();
        swapBuffers();
        clearCurrent();
        return;
    }

    // Get all layers sorted by z-order
    auto layers = layerManager->getLayersSortedByZOrder();
    
    // Convert to const vector for rendering
    std::vector<const VideoLayer*> constLayers;
    constLayers.reserve(layers.size());
    for (auto* layer : layers) {
        constLayers.push_back(layer);
    }

    // Set viewport
    renderer_->setViewport(0, 0, windowWidth_, windowHeight_);

    // Composite all layers
    renderer_->compositeLayers(constLayers);

    // Render OSD if available
    if (osdManager && osdRenderer_) {
        auto osdItems = osdRenderer_->prepareOSDRender(osdManager, windowWidth_, windowHeight_);
        if (!osdItems.empty()) {
            renderer_->renderOSDItems(osdItems);
            
            // Cleanup OSD textures after rendering
            for (const auto& item : osdItems) {
                if (item.textureId != 0) {
                    glDeleteTextures(1, &item.textureId);
                }
            }
        }
    }

    glFlush();
    swapBuffers();
    
    renderer_->cleanupDeferredTextures();
    
    clearCurrent();
}

void WaylandDisplay::handleEvents() {
    if (!windowOpen_) {
        return;
    }

    handleWaylandEvents();
}

void WaylandDisplay::resize(unsigned int width, unsigned int height) {
    windowWidth_ = width;
    windowHeight_ = height;

    if (windowOpen_ && wlEglWindow_) {
        wl_egl_window_resize(wlEglWindow_, width, height, 0, 0);
        
        makeCurrent();
        renderer_->setViewport(0, 0, width, height);
        clearCurrent();
    }
}

void WaylandDisplay::getWindowSize(unsigned int* width, unsigned int* height) const {
    if (width) *width = windowWidth_;
    if (height) *height = windowHeight_;
}

void WaylandDisplay::setPosition(int x, int y) {
    windowX_ = x;
    windowY_ = y;
    // Note: Wayland doesn't allow clients to set absolute window position
}

void WaylandDisplay::getWindowPos(int* x, int* y) const {
    if (x) *x = windowX_;
    if (y) *y = windowY_;
}

void WaylandDisplay::setFullscreen(int action) {
    bool newState = fullscreen_;
    
    if (action == 0) {
        newState = false;
    } else if (action == 1) {
        newState = true;
    } else if (action == 2) {
        newState = !fullscreen_;
    }

    if (newState != fullscreen_ && xdgToplevel_) {
        fullscreen_ = newState;
        if (fullscreen_) {
            xdg_toplevel_set_fullscreen(xdgToplevel_, nullptr);
        } else {
            xdg_toplevel_unset_fullscreen(xdgToplevel_);
        }
        wl_surface_commit(wlSurface_);
    }
}

bool WaylandDisplay::getFullscreen() const {
    return fullscreen_;
}

void WaylandDisplay::setOnTop(int action) {
    // Wayland doesn't have a direct "always on top" concept
    // This would need to be requested via the compositor
    bool newState = ontop_;
    
    if (action == 0) {
        newState = false;
    } else if (action == 1) {
        newState = true;
    } else if (action == 2) {
        newState = !ontop_;
    }

    ontop_ = newState;
}

bool WaylandDisplay::getOnTop() const {
    return ontop_;
}

void* WaylandDisplay::getContext() {
#ifdef HAVE_EGL
    return eglContext_;
#else
    return nullptr;
#endif
}

void WaylandDisplay::makeCurrent() {
#ifdef HAVE_EGL
    if (eglDisplay_ != EGL_NO_DISPLAY && eglContext_ != EGL_NO_CONTEXT && eglSurface_ != EGL_NO_SURFACE) {
        if (!eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_)) {
            LOG_ERROR << "eglMakeCurrent failed: 0x" << std::hex << eglGetError() << std::dec;
        }
    }
#endif
}

void WaylandDisplay::clearCurrent() {
#ifdef HAVE_EGL
    if (eglDisplay_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
#endif
}

void WaylandDisplay::swapBuffers() {
#ifdef HAVE_EGL
    if (eglDisplay_ != EGL_NO_DISPLAY && eglSurface_ != EGL_NO_SURFACE) {
        eglSwapBuffers(eglDisplay_, eglSurface_);
    }
#endif
}

// Wayland initialization
bool WaylandDisplay::initWayland() {
    // Step 1: Connect to Wayland display
    const char* waylandDisplay = getenv("WAYLAND_DISPLAY");
    wlDisplay_ = wl_display_connect(nullptr);
    if (!wlDisplay_) {
        LOG_ERROR << "Failed to connect to Wayland display";
        if (waylandDisplay) {
            LOG_ERROR << "  WAYLAND_DISPLAY=" << waylandDisplay;
            LOG_ERROR << "  Check if socket exists: /run/user/$UID/" << waylandDisplay;
        } else {
            LOG_ERROR << "  WAYLAND_DISPLAY not set (using default 'wayland-0')";
        }
        LOG_ERROR << "  Are you running under a Wayland compositor? (Sway, GNOME Wayland, KDE Wayland, etc.)";
        LOG_ERROR << "  If running under X11, the application will try to fall back to X11 backend";
        return false;
    }
    
    LOG_INFO << "Connected to Wayland display" << (waylandDisplay ? std::string(" (") + waylandDisplay + ")" : "");
    
    // Step 2: Get registry and bind to protocols
    wlRegistry_ = wl_display_get_registry(wlDisplay_);
    if (!wlRegistry_) {
        LOG_ERROR << "Failed to get Wayland registry";
        return false;
    }
    
    wl_registry_add_listener(wlRegistry_, &registry_listener, this);
    
    // Roundtrip to receive all registry events
    wl_display_roundtrip(wlDisplay_);
    
    // Step 3: Check that we have required protocols
    if (!wlCompositor_) {
        LOG_ERROR << "Wayland compositor not available";
        return false;
    }
    
    if (!xdgWmBase_) {
        LOG_ERROR << "XDG shell not available";
        return false;
    }
    
    // DMA-BUF is optional but important for VAAPI
    if (!dmabuf_) {
        LOG_WARNING << "zwp_linux_dmabuf_v1 not available - VAAPI zero-copy may not work";
    } else {
        LOG_INFO << "zwp_linux_dmabuf_v1 available - VAAPI zero-copy enabled";
    }
    
    // Step 4: Create Wayland surface
    wlSurface_ = wl_compositor_create_surface(wlCompositor_);
    if (!wlSurface_) {
        LOG_ERROR << "Failed to create Wayland surface";
        return false;
    }
    
    // Step 5: Create XDG surface and toplevel
    xdgSurface_ = xdg_wm_base_get_xdg_surface(xdgWmBase_, wlSurface_);
    if (!xdgSurface_) {
        LOG_ERROR << "Failed to create XDG surface";
        return false;
    }
    
    xdg_surface_add_listener(xdgSurface_, &xdg_surface_listener, this);
    
    xdgToplevel_ = xdg_surface_get_toplevel(xdgSurface_);
    if (!xdgToplevel_) {
        LOG_ERROR << "Failed to create XDG toplevel";
        return false;
    }
    
    xdg_toplevel_add_listener(xdgToplevel_, &xdg_toplevel_listener, this);
    xdg_toplevel_set_title(xdgToplevel_, "cuems-videocomposer");
    xdg_toplevel_set_app_id(xdgToplevel_, "cuems-videocomposer");
    
    // Set initial window size hints
    xdg_toplevel_set_min_size(xdgToplevel_, 320, 240);
    
    // Commit the surface to trigger the first configure event
    wl_surface_commit(wlSurface_);
    
    // Wait for configure event (with timeout to avoid hanging)
    int maxTries = 100;
    while (!configured_ && wlDisplay_ && maxTries-- > 0) {
        if (wl_display_dispatch(wlDisplay_) == -1) {
            LOG_ERROR << "Wayland display dispatch failed while waiting for configure";
            return false;
        }
    }
    
    if (!configured_) {
        LOG_WARNING << "Wayland surface not configured after waiting - continuing anyway";
    } else {
        LOG_INFO << "Wayland surface configured";
    }
    
    return true;
}

void WaylandDisplay::cleanupWayland() {
    if (wlEglWindow_) {
        wl_egl_window_destroy(wlEglWindow_);
        wlEglWindow_ = nullptr;
    }
    
    if (xdgToplevel_) {
        xdg_toplevel_destroy(xdgToplevel_);
        xdgToplevel_ = nullptr;
    }
    
    if (xdgSurface_) {
        xdg_surface_destroy(xdgSurface_);
        xdgSurface_ = nullptr;
    }
    
    if (wlSurface_) {
        wl_surface_destroy(wlSurface_);
        wlSurface_ = nullptr;
    }
    
    if (xdgWmBase_) {
        xdg_wm_base_destroy(xdgWmBase_);
        xdgWmBase_ = nullptr;
    }
    
    if (dmabuf_) {
        zwp_linux_dmabuf_v1_destroy(dmabuf_);
        dmabuf_ = nullptr;
    }
    
    if (wlCompositor_) {
        wl_compositor_destroy(wlCompositor_);
        wlCompositor_ = nullptr;
    }
    
    if (wlRegistry_) {
        wl_registry_destroy(wlRegistry_);
        wlRegistry_ = nullptr;
    }
    
    if (wlDisplay_) {
        wl_display_disconnect(wlDisplay_);
        wlDisplay_ = nullptr;
    }
    
    configured_ = false;
}

// EGL initialization
bool WaylandDisplay::initEGL() {
#ifndef HAVE_EGL
    LOG_ERROR << "EGL support not compiled in";
    return false;
#else
    // Step 1: Get EGL display from Wayland display
    eglDisplay_ = eglGetDisplay((EGLNativeDisplayType)wlDisplay_);
    if (eglDisplay_ == EGL_NO_DISPLAY) {
        LOG_ERROR << "eglGetDisplay failed";
        return false;
    }
    
    // Step 2: Initialize EGL
    EGLint major, minor;
    if (!eglInitialize(eglDisplay_, &major, &minor)) {
        LOG_ERROR << "eglInitialize failed";
        return false;
    }
    LOG_INFO << "EGL initialized: version " << major << "." << minor;
    
    // Step 3: Bind OpenGL API
    if (!eglBindAPI(EGL_OPENGL_API)) {
        LOG_ERROR << "eglBindAPI(EGL_OPENGL_API) failed";
        return false;
    }
    
    // Step 4: Choose EGL config
    // Note: EGL_ALPHA_SIZE set to 0 for opaque window (no compositor-level transparency)
    // Internal alpha blending still works for HAP Alpha, OSD, and layer opacity
    EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,  // Opaque window - no compositor-level transparency
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_NONE
    };
    
    EGLint numConfigs;
    if (!eglChooseConfig(eglDisplay_, configAttribs, &eglConfig_, 1, &numConfigs) || numConfigs == 0) {
        LOG_ERROR << "eglChooseConfig failed";
        return false;
    }
    
    // Step 5: Create EGL context
    EGLint contextAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
        EGL_NONE
    };
    
    eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, EGL_NO_CONTEXT, contextAttribs);
    if (eglContext_ == EGL_NO_CONTEXT) {
        // Try without profile
        LOG_WARNING << "EGL core profile context failed, trying compatibility";
        EGLint fallbackAttribs[] = { EGL_NONE };
        eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, EGL_NO_CONTEXT, fallbackAttribs);
    }
    
    if (eglContext_ == EGL_NO_CONTEXT) {
        LOG_ERROR << "eglCreateContext failed: 0x" << std::hex << eglGetError() << std::dec;
        return false;
    }
    
    // Step 6: Create EGL window
    wlEglWindow_ = wl_egl_window_create(wlSurface_, windowWidth_, windowHeight_);
    if (!wlEglWindow_) {
        LOG_ERROR << "wl_egl_window_create failed";
        return false;
    }
    
    // Step 7: Create EGL surface
    eglSurface_ = eglCreateWindowSurface(eglDisplay_, eglConfig_, wlEglWindow_, nullptr);
    if (eglSurface_ == EGL_NO_SURFACE) {
        LOG_ERROR << "eglCreateWindowSurface failed: 0x" << std::hex << eglGetError() << std::dec;
        return false;
    }
    
    // Step 8: Query EGL extensions for VAAPI support
    if (!queryEGLExtensions()) {
        LOG_WARNING << "Required EGL extensions not available - VAAPI zero-copy may not work";
    }
    
    LOG_INFO << "EGL initialized on Wayland";
    
    return true;
#endif // HAVE_EGL
}

void WaylandDisplay::cleanupEGL() {
#ifdef HAVE_EGL
    if (eglDisplay_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        
        if (eglContext_ != EGL_NO_CONTEXT) {
            eglDestroyContext(eglDisplay_, eglContext_);
            eglContext_ = EGL_NO_CONTEXT;
        }
        
        if (eglSurface_ != EGL_NO_SURFACE) {
            eglDestroySurface(eglDisplay_, eglSurface_);
            eglSurface_ = EGL_NO_SURFACE;
        }
        
        eglTerminate(eglDisplay_);
        eglDisplay_ = EGL_NO_DISPLAY;
    }
    
    eglConfig_ = nullptr;
    vaapiSupported_ = false;
    eglCreateImageKHR_ = nullptr;
    eglDestroyImageKHR_ = nullptr;
    glEGLImageTargetTexture2DOES_ = nullptr;
#endif
}

bool WaylandDisplay::queryEGLExtensions() {
#ifndef HAVE_EGL
    return false;
#else
    const char* extensions = eglQueryString(eglDisplay_, EGL_EXTENSIONS);
    if (!extensions) {
        LOG_WARNING << "eglQueryString(EGL_EXTENSIONS) failed";
        return false;
    }
    
    LOG_VERBOSE << "EGL extensions: " << extensions;
    
    // Check for required extensions
    bool hasDmaBufImport = (strstr(extensions, "EGL_EXT_image_dma_buf_import") != nullptr);
    bool hasImageBase = (strstr(extensions, "EGL_KHR_image_base") != nullptr);
    
    if (!hasDmaBufImport) {
        LOG_WARNING << "EGL_EXT_image_dma_buf_import not supported";
        return false;
    }
    
    if (!hasImageBase) {
        LOG_WARNING << "EGL_KHR_image_base not supported";
        return false;
    }
    
    LOG_INFO << "EGL extensions available: EGL_EXT_image_dma_buf_import, EGL_KHR_image_base";
    
    // Get extension function pointers
    eglCreateImageKHR_ = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR_ = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    glEGLImageTargetTexture2DOES_ = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    
    if (!eglCreateImageKHR_ || !eglDestroyImageKHR_ || !glEGLImageTargetTexture2DOES_) {
        LOG_WARNING << "Failed to load EGL extension functions";
        return false;
    }
    
    LOG_INFO << "EGL extension functions loaded successfully";
    vaapiSupported_ = true;
    
    return true;
#endif
}

// VAAPI initialization
bool WaylandDisplay::initVAAPI() {
#ifndef HAVE_VAAPI_INTEROP
    return false;
#else
    // Open DRM render node
    vaDrmFd_ = open("/dev/dri/renderD128", O_RDWR);
    if (vaDrmFd_ < 0) {
        LOG_WARNING << "Failed to open /dev/dri/renderD128";
        return false;
    }
    
    // Create VAAPI display from DRM fd
    vaDisplay_ = vaGetDisplayDRM(vaDrmFd_);
    if (!vaDisplay_) {
        LOG_WARNING << "vaGetDisplayDRM failed";
        close(vaDrmFd_);
        vaDrmFd_ = -1;
        return false;
    }
    
    // Initialize VAAPI
    int majorVer, minorVer;
    VAStatus status = vaInitialize(vaDisplay_, &majorVer, &minorVer);
    if (status != VA_STATUS_SUCCESS) {
        LOG_WARNING << "vaInitialize failed: " << vaErrorStr(status);
        vaDisplay_ = nullptr;
        close(vaDrmFd_);
        vaDrmFd_ = -1;
        return false;
    }
    
    LOG_INFO << "VAAPI display initialized: version " << majorVer << "." << minorVer;
    vaapiSupported_ = true;
    
    return true;
#endif
}

void WaylandDisplay::cleanupVAAPI() {
#ifdef HAVE_VAAPI_INTEROP
    if (vaDisplay_) {
        vaTerminate(vaDisplay_);
        vaDisplay_ = nullptr;
    }
    
    if (vaDrmFd_ >= 0) {
        close(vaDrmFd_);
        vaDrmFd_ = -1;
    }
#endif
}

// Wayland event handling
void WaylandDisplay::handleWaylandEvents() {
    if (!wlDisplay_) return;
    
    // Dispatch pending events without blocking
    wl_display_dispatch_pending(wlDisplay_);
    wl_display_flush(wlDisplay_);
}

// Wayland registry callbacks
void WaylandDisplay::registryHandleGlobal(void* data, struct wl_registry* registry,
                                         uint32_t name, const char* interface, uint32_t version) {
    WaylandDisplay* display = static_cast<WaylandDisplay*>(data);
    
    if (strcmp(interface, "wl_compositor") == 0) {
        display->wlCompositor_ = static_cast<struct wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 1)
        );
        LOG_INFO << "Bound to wl_compositor";
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        display->xdgWmBase_ = static_cast<struct xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, 1)
        );
        xdg_wm_base_add_listener(display->xdgWmBase_, &xdg_wm_base_listener, display);
        LOG_INFO << "Bound to xdg_wm_base";
    } else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
        display->dmabuf_ = static_cast<struct zwp_linux_dmabuf_v1*>(
            wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 3)
        );
        LOG_INFO << "Bound to zwp_linux_dmabuf_v1";
    }
}

void WaylandDisplay::registryHandleGlobalRemove(void* data, struct wl_registry* registry, uint32_t name) {
    // Handle global removal if needed
}

// XDG shell callbacks
void WaylandDisplay::xdgWmBasePing(void* data, struct xdg_wm_base* xdg_wm_base, uint32_t serial) {
    xdg_wm_base_pong(xdg_wm_base, serial);
}

void WaylandDisplay::xdgSurfaceConfigure(void* data, struct xdg_surface* xdg_surface, uint32_t serial) {
    WaylandDisplay* display = static_cast<WaylandDisplay*>(data);
    xdg_surface_ack_configure(xdg_surface, serial);
    display->configured_ = true;
}

void WaylandDisplay::xdgToplevelConfigure(void* data, struct xdg_toplevel* xdg_toplevel,
                                         int32_t width, int32_t height, ::wl_array* states) {
    WaylandDisplay* display = static_cast<WaylandDisplay*>(data);
    
    if (width > 0 && height > 0) {
        display->resize(width, height);
    }
}

void WaylandDisplay::xdgToplevelClose(void* data, struct xdg_toplevel* xdg_toplevel) {
    // Handle window close request
    // The application should handle this by checking a flag in the main loop
}

} // namespace videocomposer

#endif // HAVE_WAYLAND

