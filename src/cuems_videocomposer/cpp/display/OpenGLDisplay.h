#ifndef VIDEOCOMPOSER_OPENGLDISPLAY_H
#define VIDEOCOMPOSER_OPENGLDISPLAY_H

#include "DisplayBackend.h"
#include "OpenGLRenderer.h"
#include <memory>

// Forward declarations for platform-specific types
#if defined(PLATFORM_LINUX) || (!defined(PLATFORM_WINDOWS) && !defined(PLATFORM_OSX))
struct _XDisplay;
typedef struct _XDisplay Display;
typedef unsigned long Window;
typedef struct __GLXcontextRec *GLXContext;

// EGL forward declarations (for VAAPI zero-copy)
#ifdef HAVE_EGL
typedef void* EGLDisplay;
typedef void* EGLContext;
typedef void* EGLSurface;
typedef void* EGLConfig;
typedef void* EGLImageKHR;
// Function pointer types for EGL extensions
typedef EGLImageKHR (*PFNEGLCREATEIMAGEKHRPROC)(EGLDisplay, EGLContext, unsigned int, void*, const int*);
typedef unsigned int (*PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay, EGLImageKHR);
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(unsigned int, void*);
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
 * OpenGLDisplay - OpenGL-based display backend (Linux GLX only)
 * 
 * Implements DisplayBackend using OpenGL with GLX on Linux.
 * Supports multi-layer rendering and multi-display output (Xinerama/Wayland).
 * 
 * Note: Only Linux GLX is implemented. Windows (WGL) and macOS (CGL) are not supported.
 */
class OpenGLDisplay : public DisplayBackend {
public:
    OpenGLDisplay();
    virtual ~OpenGLDisplay();

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

    // OpenGL context management (public for frame loading that needs GPU texture allocation)
    void makeCurrent();
    void clearCurrent();

#ifdef HAVE_EGL
    // EGL context access (for VAAPI zero-copy interop)
    EGLDisplay getEGLDisplay() const { return eglDisplay_; }
    bool hasVaapiSupport() const { return vaapiSupported_; }
    
    // EGL extension function pointers (for VaapiInterop)
    PFNEGLCREATEIMAGEKHRPROC getEglCreateImageKHR() const { return eglCreateImageKHR_; }
    PFNEGLDESTROYIMAGEKHRPROC getEglDestroyImageKHR() const { return eglDestroyImageKHR_; }
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC getGlEGLImageTargetTexture2DOES() const { return glEGLImageTargetTexture2DOES_; }
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

#endif // VIDEOCOMPOSER_OPENGLDISPLAY_H

