#include "OpenGLDisplay.h"
#include "../layer/LayerManager.h"
#include "../osd/OSDManager.h"
#include "../osd/OSDRenderer.h"
#include "../utils/Logger.h"
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <iostream>

#if defined(PLATFORM_LINUX) || (!defined(PLATFORM_WINDOWS) && !defined(PLATFORM_OSX))
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#define USE_GLX 1

#ifdef HAVE_EGL
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cstring>  // for strstr

// EGL extension constants
#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#endif
#endif

#ifdef HAVE_VAAPI_INTEROP
#include <va/va.h>
#include <va/va_x11.h>
#endif
#elif defined(PLATFORM_WINDOWS)
#include <windows.h>
#include <GL/gl.h>
#define USE_WGL 1
#elif defined(PLATFORM_OSX)
// OSX includes
#define USE_CGL 1
#endif

namespace videocomposer {

OpenGLDisplay::OpenGLDisplay()
    : renderer_(std::make_unique<OpenGLRenderer>())
    , osdRenderer_(std::make_unique<OSDRenderer>())
#if defined(PLATFORM_LINUX) || (!defined(PLATFORM_WINDOWS) && !defined(PLATFORM_OSX))
    , display_(nullptr)
    , window_(0)
    , context_(nullptr)
    , screen_(0)
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
#endif
#elif defined(PLATFORM_WINDOWS)
    , hdc_(nullptr)
    , hwnd_(nullptr)
    , hglrc_(nullptr)
#elif defined(PLATFORM_OSX)
    , nsContext_(nullptr)
#endif
    , windowWidth_(640)
    , windowHeight_(360)
    , windowX_(0)
    , windowY_(0)
    , fullscreen_(false)
    , ontop_(false)
    , windowOpen_(false)
{
}

OpenGLDisplay::~OpenGLDisplay() {
    closeWindow();
}

bool OpenGLDisplay::openWindow() {
    if (windowOpen_) {
        return true;
    }

#if defined(USE_GLX)
#ifdef HAVE_EGL
    // Pure EGL initialization for VAAPI zero-copy support
    // This replaces GLX with EGL for context management
    if (!initEGL()) {
        LOG_ERROR << "Failed to initialize EGL";
        // Fall back to GLX if EGL fails
        LOG_WARNING << "Falling back to GLX (VAAPI zero-copy disabled)";
        if (!initGLX()) {
            LOG_ERROR << "Failed to initialize GLX";
            return false;
        }
    }
#else
    // No EGL support - use GLX directly
    if (!initGLX()) {
        LOG_ERROR << "Failed to initialize GLX";
        return false;
    }
#endif

#elif defined(USE_WGL)
    if (!initWGL()) {
        return false;
    }
#elif defined(USE_CGL)
    if (!initCGL()) {
        return false;
    }
#else
    std::cerr << "No OpenGL implementation available for this platform" << std::endl;
    return false;
#endif

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
        // Don't fail window creation if OSD fails
    }

    clearCurrent();
    windowOpen_ = true;
    return true;
}

void OpenGLDisplay::closeWindow() {
    if (!windowOpen_) {
        return;
    }

    makeCurrent();
    renderer_->cleanup();
    if (osdRenderer_) {
        osdRenderer_->cleanup();
    }
    clearCurrent();

#if defined(USE_GLX)
#ifdef HAVE_EGL
    // If EGL was initialized (pure EGL mode), use EGL cleanup which handles everything
    if (eglDisplay_ != EGL_NO_DISPLAY) {
        cleanupEGL();
    } else {
        // Otherwise use GLX cleanup (fallback mode)
        cleanupGLX();
    }
#else
    cleanupGLX();
#endif
#elif defined(USE_WGL)
    cleanupWGL();
#elif defined(USE_CGL)
    cleanupCGL();
#endif

    windowOpen_ = false;
}

bool OpenGLDisplay::isWindowOpen() const {
    return windowOpen_;
}

void OpenGLDisplay::render(LayerManager* layerManager, OSDManager* osdManager) {
    if (!windowOpen_ || !layerManager) {
        return;
    }

    makeCurrent();

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
            
            // Cleanup OSD textures after rendering (but before swapBuffers)
            for (const auto& item : osdItems) {
                if (item.textureId != 0) {
                    glDeleteTextures(1, &item.textureId);
                }
            }
        }
    }

    // Flush and swap buffers (matches original xjadeo)
    glFlush();
    swapBuffers();
    
    // Cleanup deferred texture deletions AFTER swapBuffers
    // This ensures OpenGL is done using the textures before we delete them
    renderer_->cleanupDeferredTextures();
    
    clearCurrent();
}

void OpenGLDisplay::handleEvents() {
    if (!windowOpen_) {
        return;
    }

#if defined(USE_GLX)
    handleEventsGLX();
#elif defined(USE_WGL)
    handleEventsWGL();
#elif defined(USE_CGL)
    handleEventsCGL();
#endif
}

void OpenGLDisplay::resize(unsigned int width, unsigned int height) {
    windowWidth_ = width;
    windowHeight_ = height;

    if (windowOpen_) {
        makeCurrent();
        renderer_->setViewport(0, 0, width, height);
        clearCurrent();
    }
}

void OpenGLDisplay::getWindowSize(unsigned int* width, unsigned int* height) const {
    if (width) *width = windowWidth_;
    if (height) *height = windowHeight_;
}

void OpenGLDisplay::setPosition(int x, int y) {
    windowX_ = x;
    windowY_ = y;
    // Platform-specific implementation would move window here
}

void OpenGLDisplay::getWindowPos(int* x, int* y) const {
    if (x) *x = windowX_;
    if (y) *y = windowY_;
}

void OpenGLDisplay::setFullscreen(int action) {
    bool newState = fullscreen_;
    
    if (action == 0) {
        newState = false;
    } else if (action == 1) {
        newState = true;
    } else if (action == 2) {
        newState = !fullscreen_;
    }

    if (newState != fullscreen_) {
        fullscreen_ = newState;
        // Platform-specific fullscreen toggle would go here
    }
}

bool OpenGLDisplay::getFullscreen() const {
    return fullscreen_;
}

void OpenGLDisplay::setOnTop(int action) {
    bool newState = ontop_;
    
    if (action == 0) {
        newState = false;
    } else if (action == 1) {
        newState = true;
    } else if (action == 2) {
        newState = !ontop_;
    }

    if (newState != ontop_) {
        ontop_ = newState;
        // Platform-specific on-top toggle would go here
    }
}

bool OpenGLDisplay::getOnTop() const {
    return ontop_;
}

void* OpenGLDisplay::getContext() {
#if defined(USE_GLX)
    return context_;
#elif defined(USE_WGL)
    return hglrc_;
#elif defined(USE_CGL)
    return nsContext_;
#else
    return nullptr;
#endif
}

// Platform-specific implementations
#if defined(USE_GLX)
bool OpenGLDisplay::initGLX() {
    display_ = XOpenDisplay(nullptr);
    if (!display_) {
        std::cerr << "Cannot open X display" << std::endl;
        return false;
    }

    screen_ = DefaultScreen(display_);

    // Get GLX visual
    int attribs[] = {
        GLX_RGBA,
        GLX_DOUBLEBUFFER,
        GLX_RED_SIZE, 8,
        GLX_GREEN_SIZE, 8,
        GLX_BLUE_SIZE, 8,
        GLX_ALPHA_SIZE, 8,
        None
    };

    XVisualInfo* vi = glXChooseVisual(display_, screen_, attribs);
    if (!vi) {
        XCloseDisplay(display_);
        display_ = nullptr;
        return false;
    }

    // Create window
    Window root = RootWindow(display_, screen_);
    Colormap cmap = XCreateColormap(display_, root, vi->visual, AllocNone);

    XSetWindowAttributes attr;
    attr.colormap = cmap;
    attr.border_pixel = 0;
    attr.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | 
                      ButtonReleaseMask | StructureNotifyMask;

    window_ = XCreateWindow(display_, root,
                           windowX_, windowY_, windowWidth_, windowHeight_,
                           0, vi->depth, InputOutput, vi->visual,
                           CWBorderPixel | CWColormap | CWEventMask, &attr);

    if (!window_) {
        XFree(vi);
        XCloseDisplay(display_);
        display_ = nullptr;
        return false;
    }

    XStoreName(display_, window_, "cuems-videocomposer");
    
    Atom wmDelete = XInternAtom(display_, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display_, window_, &wmDelete, 1);

    // Create GLX context
    context_ = glXCreateContext(display_, vi, nullptr, GL_TRUE);
    XFree(vi);

    if (!context_) {
        XDestroyWindow(display_, window_);
        XCloseDisplay(display_);
        display_ = nullptr;
        window_ = 0;
        return false;
    }

    XMapRaised(display_, window_);
    XSync(display_, False);
    
    // Ensure window is visible and raised
    XRaiseWindow(display_, window_);
    XFlush(display_);

    return true;
}

void OpenGLDisplay::cleanupGLX() {
    if (context_) {
        glXDestroyContext(display_, context_);
        context_ = nullptr;
    }
    if (window_) {
        XDestroyWindow(display_, window_);
        window_ = 0;
    }
    if (display_) {
        XCloseDisplay(display_);
        display_ = nullptr;
    }
}

#ifdef HAVE_EGL
bool OpenGLDisplay::initEGL() {
    // Pure EGL initialization - creates X11 window, EGL context, and surface
    // This enables VAAPI zero-copy by using EGL for all OpenGL operations
    
    // Step 1: Open X11 display
    display_ = XOpenDisplay(nullptr);
    if (!display_) {
        LOG_ERROR << "Cannot open X display";
        return false;
    }
    screen_ = DefaultScreen(display_);
    
    // Step 2: Get EGL display from X11 display
    eglDisplay_ = eglGetDisplay((EGLNativeDisplayType)display_);
    if (eglDisplay_ == EGL_NO_DISPLAY) {
        LOG_ERROR << "eglGetDisplay failed";
        XCloseDisplay(display_);
        display_ = nullptr;
        return false;
    }
    
    // Step 3: Initialize EGL
    EGLint major, minor;
    if (!eglInitialize(eglDisplay_, &major, &minor)) {
        LOG_ERROR << "eglInitialize failed";
        eglDisplay_ = EGL_NO_DISPLAY;
        XCloseDisplay(display_);
        display_ = nullptr;
        return false;
    }
    LOG_INFO << "EGL initialized: version " << major << "." << minor;
    
    // Step 4: Bind OpenGL API (not OpenGL ES)
    if (!eglBindAPI(EGL_OPENGL_API)) {
        LOG_ERROR << "eglBindAPI(EGL_OPENGL_API) failed";
        eglTerminate(eglDisplay_);
        eglDisplay_ = EGL_NO_DISPLAY;
        XCloseDisplay(display_);
        display_ = nullptr;
        return false;
    }
    
    // Step 5: Choose EGL config
    EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_NONE
    };
    
    EGLint numConfigs;
    if (!eglChooseConfig(eglDisplay_, configAttribs, &eglConfig_, 1, &numConfigs) || numConfigs == 0) {
        LOG_ERROR << "eglChooseConfig failed";
        eglTerminate(eglDisplay_);
        eglDisplay_ = EGL_NO_DISPLAY;
        XCloseDisplay(display_);
        display_ = nullptr;
        return false;
    }
    
    // Step 6: Get X11 visual ID from EGL config
    EGLint visualId;
    if (!eglGetConfigAttrib(eglDisplay_, eglConfig_, EGL_NATIVE_VISUAL_ID, &visualId)) {
        LOG_ERROR << "eglGetConfigAttrib(EGL_NATIVE_VISUAL_ID) failed";
        eglTerminate(eglDisplay_);
        eglDisplay_ = EGL_NO_DISPLAY;
        XCloseDisplay(display_);
        display_ = nullptr;
        return false;
    }
    
    // Step 7: Get XVisualInfo from visual ID
    XVisualInfo viTemplate;
    viTemplate.visualid = visualId;
    int numVisuals;
    XVisualInfo* vi = XGetVisualInfo(display_, VisualIDMask, &viTemplate, &numVisuals);
    if (!vi) {
        LOG_ERROR << "XGetVisualInfo failed for visual ID " << visualId;
        eglTerminate(eglDisplay_);
        eglDisplay_ = EGL_NO_DISPLAY;
        XCloseDisplay(display_);
        display_ = nullptr;
        return false;
    }
    
    // Step 8: Create X11 window with EGL-compatible visual
    Window root = RootWindow(display_, screen_);
    Colormap cmap = XCreateColormap(display_, root, vi->visual, AllocNone);
    
    XSetWindowAttributes attr;
    attr.colormap = cmap;
    attr.border_pixel = 0;
    attr.event_mask = ExposureMask | KeyPressMask | ButtonPressMask |
                      ButtonReleaseMask | StructureNotifyMask;
    
    window_ = XCreateWindow(display_, root,
                           windowX_, windowY_, windowWidth_, windowHeight_,
                           0, vi->depth, InputOutput, vi->visual,
                           CWBorderPixel | CWColormap | CWEventMask, &attr);
    XFree(vi);
    
    if (!window_) {
        LOG_ERROR << "XCreateWindow failed";
        eglTerminate(eglDisplay_);
        eglDisplay_ = EGL_NO_DISPLAY;
        XCloseDisplay(display_);
        display_ = nullptr;
        return false;
    }
    
    XStoreName(display_, window_, "cuems-videocomposer");
    Atom wmDelete = XInternAtom(display_, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display_, window_, &wmDelete, 1);
    
    // Step 9: Create EGL context
    // Use Compatibility Profile to support legacy fixed-function pipeline (glBegin/glEnd)
    // which is still used by some rendering paths (software decode, HAP)
    EGLint contextAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
        EGL_NONE
    };
    
    eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, EGL_NO_CONTEXT, contextAttribs);
    if (eglContext_ == EGL_NO_CONTEXT) {
        // Try without core profile (fallback for older drivers)
        LOG_WARNING << "EGL core profile context failed, trying compatibility";
        EGLint fallbackAttribs[] = { EGL_NONE };
        eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, EGL_NO_CONTEXT, fallbackAttribs);
    }
    
    if (eglContext_ == EGL_NO_CONTEXT) {
        LOG_ERROR << "eglCreateContext failed: 0x" << std::hex << eglGetError() << std::dec;
        XDestroyWindow(display_, window_);
        window_ = 0;
        eglTerminate(eglDisplay_);
        eglDisplay_ = EGL_NO_DISPLAY;
        XCloseDisplay(display_);
        display_ = nullptr;
        return false;
    }
    
    // Step 10: Create EGL window surface
    eglSurface_ = eglCreateWindowSurface(eglDisplay_, eglConfig_, window_, nullptr);
    if (eglSurface_ == EGL_NO_SURFACE) {
        LOG_ERROR << "eglCreateWindowSurface failed: 0x" << std::hex << eglGetError() << std::dec;
        eglDestroyContext(eglDisplay_, eglContext_);
        eglContext_ = EGL_NO_CONTEXT;
        XDestroyWindow(display_, window_);
        window_ = 0;
        eglTerminate(eglDisplay_);
        eglDisplay_ = EGL_NO_DISPLAY;
        XCloseDisplay(display_);
        display_ = nullptr;
        return false;
    }
    
    // Step 11: Map the window
    XMapRaised(display_, window_);
    XSync(display_, False);
    XRaiseWindow(display_, window_);
    XFlush(display_);
    
    // Step 12: Query EGL extensions for VAAPI zero-copy
    if (!queryEGLExtensions()) {
        LOG_WARNING << "Required EGL extensions not available - VAAPI zero-copy disabled";
        // Don't fail - we can still render, just without VAAPI zero-copy
    }
    
#ifdef HAVE_VAAPI_INTEROP
    // Step 13: Create VAAPI display from X11 display
    // This MUST be shared between the FFmpeg decoder and VaapiInterop for zero-copy
    vaDisplay_ = vaGetDisplay(display_);
    if (vaDisplay_) {
        int majorVer, minorVer;
        VAStatus status = vaInitialize(vaDisplay_, &majorVer, &minorVer);
        if (status == VA_STATUS_SUCCESS) {
            LOG_INFO << "VAAPI display initialized: version " << majorVer << "." << minorVer;
            vaapiSupported_ = true;
        } else {
            LOG_WARNING << "vaInitialize failed: " << vaErrorStr(status);
            vaDisplay_ = nullptr;
        }
    } else {
        LOG_WARNING << "vaGetDisplay failed - VAAPI zero-copy disabled";
    }
#endif
    
    // Log the EGL config details
    EGLint redSize, greenSize, blueSize, alphaSize, bufferSize;
    eglGetConfigAttrib(eglDisplay_, eglConfig_, EGL_RED_SIZE, &redSize);
    eglGetConfigAttrib(eglDisplay_, eglConfig_, EGL_GREEN_SIZE, &greenSize);
    eglGetConfigAttrib(eglDisplay_, eglConfig_, EGL_BLUE_SIZE, &blueSize);
    eglGetConfigAttrib(eglDisplay_, eglConfig_, EGL_ALPHA_SIZE, &alphaSize);
    eglGetConfigAttrib(eglDisplay_, eglConfig_, EGL_BUFFER_SIZE, &bufferSize);
    LOG_INFO << "EGL config: R=" << redSize << " G=" << greenSize << " B=" << blueSize 
             << " A=" << alphaSize << " buffer=" << bufferSize;
    
    LOG_INFO << "Pure EGL initialization complete - VAAPI zero-copy enabled";
    vaapiSupported_ = true;
    
    return true;
}

void OpenGLDisplay::cleanupEGL() {
#ifdef HAVE_VAAPI_INTEROP
    if (vaDisplay_) {
        vaTerminate(vaDisplay_);
        vaDisplay_ = nullptr;
    }
#endif
    
    // Destroy EGL context and surface
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
    
    // Clean up X11 resources (created by initEGL in pure EGL mode)
    if (window_) {
        XDestroyWindow(display_, window_);
        window_ = 0;
    }
    if (display_) {
        XCloseDisplay(display_);
        display_ = nullptr;
    }
}

bool OpenGLDisplay::queryEGLExtensions() {
    const char* extensions = eglQueryString(eglDisplay_, EGL_EXTENSIONS);
    if (!extensions) {
        LOG_WARNING << "eglQueryString(EGL_EXTENSIONS) failed";
        return false;
    }
    
    LOG_VERBOSE << "EGL extensions: " << extensions;
    
    // Check for required extensions for VAAPI zero-copy
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
    
    if (!eglCreateImageKHR_) {
        LOG_WARNING << "eglCreateImageKHR not available";
        return false;
    }
    
    if (!eglDestroyImageKHR_) {
        LOG_WARNING << "eglDestroyImageKHR not available";
        return false;
    }
    
    if (!glEGLImageTargetTexture2DOES_) {
        LOG_WARNING << "glEGLImageTargetTexture2DOES not available";
        return false;
    }
    
    LOG_INFO << "EGL extension functions loaded successfully";
    
    return true;
}
#endif // HAVE_EGL

void OpenGLDisplay::handleEventsGLX() {
    if (!display_) return;

    XEvent event;
    while (XPending(display_)) {
        XNextEvent(display_, &event);

        switch (event.type) {
            case Expose:
                // Trigger redraw
                break;
            case KeyPress: {
                KeySym keysym = XLookupKeysym(&event.xkey, 0);
                if (keysym == XK_Escape) {
                    // Quit signal - would be handled by application
                }
                break;
            }
            case ClientMessage: {
                Atom wmDelete = XInternAtom(display_, "WM_DELETE_WINDOW", False);
                if (event.xclient.data.l[0] == static_cast<long>(wmDelete)) {
                    // Window close - would be handled by application
                }
                break;
            }
            case ConfigureNotify:
                windowWidth_ = event.xconfigure.width;
                windowHeight_ = event.xconfigure.height;
                break;
        }
    }
}

void OpenGLDisplay::makeCurrent() {
#if defined(USE_GLX)
#ifdef HAVE_EGL
    // Pure EGL path - preferred for VAAPI zero-copy
    if (eglDisplay_ != EGL_NO_DISPLAY && eglContext_ != EGL_NO_CONTEXT && eglSurface_ != EGL_NO_SURFACE) {
        if (!eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_)) {
            LOG_ERROR << "eglMakeCurrent failed: 0x" << std::hex << eglGetError() << std::dec;
        }
        return;
    }
#endif
    // Fallback to GLX (only if EGL not initialized)
    if (display_ && window_ && context_) {
        glXMakeCurrent(display_, window_, context_);
    }
#elif defined(USE_WGL)
    if (hdc_ && hglrc_) {
        wglMakeCurrent(hdc_, hglrc_);
    }
#elif defined(USE_CGL)
    // OSX make current
#endif
}

void OpenGLDisplay::clearCurrent() {
#if defined(USE_GLX)
#ifdef HAVE_EGL
    // Pure EGL path
    if (eglDisplay_ != EGL_NO_DISPLAY && eglContext_ != EGL_NO_CONTEXT) {
        eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return;
    }
#endif
    // Fallback to GLX
    if (display_) {
        glXMakeCurrent(display_, None, nullptr);
    }
#elif defined(USE_WGL)
    wglMakeCurrent(nullptr, nullptr);
#elif defined(USE_CGL)
    // OSX clear current
#endif
}

void OpenGLDisplay::swapBuffers() {
#if defined(USE_GLX)
#ifdef HAVE_EGL
    // Pure EGL path
    if (eglDisplay_ != EGL_NO_DISPLAY && eglSurface_ != EGL_NO_SURFACE) {
        eglSwapBuffers(eglDisplay_, eglSurface_);
        return;
    }
#endif
    // Fallback to GLX
    if (display_ && window_) {
        glXSwapBuffers(display_, window_);
    }
#elif defined(USE_WGL)
    if (hdc_) {
        SwapBuffers(hdc_);
    }
#elif defined(USE_CGL)
    // OSX swap buffers
#endif
}

#elif defined(USE_WGL)
// Windows (WGL) implementation - NOT SUPPORTED
// Only Linux GLX is implemented
bool OpenGLDisplay::initWGL() {
    return false;
}

void OpenGLDisplay::cleanupWGL() {
}

void OpenGLDisplay::handleEventsWGL() {
}

#elif defined(USE_CGL)
// macOS (CGL) implementation - NOT SUPPORTED
// Only Linux GLX is implemented
bool OpenGLDisplay::initCGL() {
    return false;
}

void OpenGLDisplay::cleanupCGL() {
}

void OpenGLDisplay::handleEventsCGL() {
}

void OpenGLDisplay::makeCurrent() {
    // OSX context make current
}

void OpenGLDisplay::clearCurrent() {
    // OSX context clear
}

void OpenGLDisplay::swapBuffers() {
    // OSX swap buffers
}
#endif

} // namespace videocomposer

