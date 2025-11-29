#ifndef VIDEOCOMPOSER_X11DISPLAY_H
#define VIDEOCOMPOSER_X11DISPLAY_H

#include "DisplayBackend.h"
#include "OpenGLRenderer.h"
#include <memory>

// Forward declarations for platform-specific types
#if defined(PLATFORM_LINUX) || (!defined(PLATFORM_WINDOWS) && !defined(PLATFORM_OSX))
struct _XDisplay;
typedef struct _XDisplay Display;
typedef unsigned long Window;
typedef struct __GLXcontextRec *GLXContext;

// EGL forward declarations for platform-specific types (not in DisplayBackend)
#ifdef HAVE_EGL
typedef void* EGLSurface;
typedef void* EGLConfig;
#endif

#elif defined(PLATFORM_WINDOWS)
struct HDC__;
typedef struct HDC__* HDC;
struct HWND__;
typedef struct HWND__* HWND;
typedef void* HGLRC;
#elif defined(PLATFORM_OSX)
// OSX types would go here
typedef void* NSContext;
#endif

namespace videocomposer {

// Forward declaration
class LayerManager;

/**
 * X11Display - X11-based display backend with EGL/OpenGL (Linux only)
 * 
 * Implements DisplayBackend using X11 + EGL + OpenGL on Linux.
 * Supports multi-layer rendering and multi-display output (Xinerama).
 * 
 * Note: This backend uses X11 for windowing and EGL for OpenGL context.
 *       For Wayland systems, use WaylandDisplay instead.
 */
class X11Display : public DisplayBackend {
public:
    X11Display();
    virtual ~X11Display();

    // DisplayBackend interface
    bool openWindow() override;
    void closeWindow() override;
    bool isWindowOpen() const override;
    void render(LayerManager* layerManager, OSDManager* osdManager = nullptr) override;
    void handleEvents() override;
    void resize(unsigned int width, unsigned int height) override;
    void getWindowSize(unsigned int* width, unsigned int* height) const override;
    void setPosition(int x, int y) override;
    void getWindowPos(int* x, int* y) const override;
    void setFullscreen(int action) override;
    bool getFullscreen() const override;
    void setOnTop(int action) override;
    bool getOnTop() const override;
    bool supportsMultiDisplay() const override { return true; }
    void* getContext() override;

    // OpenGL context management (override DisplayBackend interface)
    void makeCurrent() override;
    void clearCurrent() override;

    // Get renderer access
    OpenGLRenderer* getRenderer() override { return renderer_.get(); }

#ifdef HAVE_EGL
    // EGL context access (for VAAPI zero-copy interop) - override DisplayBackend interface
    EGLDisplay getEGLDisplay() const override { return eglDisplay_; }
    bool hasVaapiSupport() const override { return vaapiSupported_; }
    
    // EGL extension function pointers (for VaapiInterop) - override DisplayBackend interface
    PFNEGLCREATEIMAGEKHRPROC getEglCreateImageKHR() const override { return eglCreateImageKHR_; }
    PFNEGLDESTROYIMAGEKHRPROC getEglDestroyImageKHR() const override { return eglDestroyImageKHR_; }
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC getGlEGLImageTargetTexture2DOES() const override { return glEGLImageTargetTexture2DOES_; }
#endif

#ifdef HAVE_VAAPI_INTEROP
    // VAAPI display access (shared between decoder and EGL interop) - override DisplayBackend interface
    VADisplay getVADisplay() const override { return vaDisplay_; }
#endif

private:
    // Platform-specific initialization
#if defined(USE_GLX) || (!defined(PLATFORM_WINDOWS) && !defined(PLATFORM_OSX))
    bool initGLX();
    void cleanupGLX();
    void handleEventsGLX();
    
#ifdef HAVE_EGL
    // EGL initialization for VAAPI zero-copy interop
    bool initEGL();
    void cleanupEGL();
    bool queryEGLExtensions();
#endif

#elif defined(PLATFORM_WINDOWS)
    bool initWGL();
    void cleanupWGL();
    void handleEventsWGL();
#elif defined(PLATFORM_OSX)
    bool initCGL();
    void cleanupCGL();
    void handleEventsCGL();
#endif

    // OpenGL context management (implementation details)
    void swapBuffers();

    // Window management
    void setupWindowHints();
    void updateWindowProperties();

    // OpenGL renderer
    std::unique_ptr<OpenGLRenderer> renderer_;
    
    // OSD renderer
    std::unique_ptr<class OSDRenderer> osdRenderer_;

    // Platform-specific window/context handles
#if defined(PLATFORM_LINUX) || (!defined(PLATFORM_WINDOWS) && !defined(PLATFORM_OSX))
    Display* display_;
    Window window_;
    GLXContext context_;
    int screen_;
    
#ifdef HAVE_EGL
    // EGL context for VAAPI zero-copy interop
    EGLDisplay eglDisplay_;
    EGLContext eglContext_;
    EGLSurface eglSurface_;
    EGLConfig eglConfig_;
    bool vaapiSupported_;
    
    // EGL extension function pointers
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_;
#endif

#ifdef HAVE_VAAPI_INTEROP
    // VAAPI display (shared between decoder and EGL interop for zero-copy)
    VADisplay vaDisplay_;
#endif

#elif defined(PLATFORM_WINDOWS)
    HDC hdc_;
    HWND hwnd_;
    HGLRC hglrc_;
#elif defined(PLATFORM_OSX)
    // OSX handles
    void* nsContext_;
#endif

    // Window state
    unsigned int windowWidth_;
    unsigned int windowHeight_;
    int windowX_;
    int windowY_;
    bool fullscreen_;
    bool ontop_;
    bool windowOpen_;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_X11DISPLAY_H

