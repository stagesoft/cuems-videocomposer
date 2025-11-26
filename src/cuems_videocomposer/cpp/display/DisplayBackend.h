#ifndef VIDEOCOMPOSER_DISPLAYBACKEND_H
#define VIDEOCOMPOSER_DISPLAYBACKEND_H

#include "../video/FrameBuffer.h"
#include <cstdint>

// Forward declarations for EGL/VAAPI types (avoid including headers)
#ifdef HAVE_EGL
typedef void* EGLDisplay;
typedef void* EGLContext;
typedef void* EGLImageKHR;
typedef EGLImageKHR (*PFNEGLCREATEIMAGEKHRPROC)(EGLDisplay, EGLContext, unsigned int, void*, const int*);
typedef unsigned int (*PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay, EGLImageKHR);
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(unsigned int, void*);
#endif

#ifdef HAVE_VAAPI_INTEROP
typedef void* VADisplay;
#endif

namespace videocomposer {

// Forward declarations
class LayerManager;
class OSDManager;

/**
 * DisplayBackend - Abstract base class for display backends
 * 
 * Interface for all display backends (X11, Wayland, SDL, etc.)
 * Keeps architecture open for future backends. Currently implemented:
 * - X11Display (X11 + EGL + OpenGL)
 * - WaylandDisplay (Wayland + EGL + OpenGL) - in progress
 */
class DisplayBackend {
public:
    virtual ~DisplayBackend() = default;

    /**
     * Open/create display window
     * @return true on success, false on failure
     */
    virtual bool openWindow() = 0;

    /**
     * Close display window
     */
    virtual void closeWindow() = 0;

    /**
     * Check if window is open
     * @return true if open, false otherwise
     */
    virtual bool isWindowOpen() const = 0;

    /**
     * Render layers to display
     * @param layerManager Layer manager containing all layers to render
     * @param osdManager Optional OSD manager for on-screen display (can be nullptr)
     */
    virtual void render(LayerManager* layerManager, OSDManager* osdManager = nullptr) = 0;

    /**
     * Handle window/display events
     * Should be called regularly from main loop
     */
    virtual void handleEvents() = 0;

    /**
     * Resize window
     * @param width New width
     * @param height New height
     */
    virtual void resize(unsigned int width, unsigned int height) = 0;

    /**
     * Get window size
     * @param width Output width
     * @param height Output height
     */
    virtual void getWindowSize(unsigned int* width, unsigned int* height) const = 0;

    /**
     * Set window position
     * @param x X position
     * @param y Y position
     */
    virtual void setPosition(int x, int y) = 0;

    /**
     * Get window position
     * @param x Output X position
     * @param y Output Y position
     */
    virtual void getWindowPos(int* x, int* y) const = 0;

    /**
     * Set fullscreen mode
     * @param action 0=windowed, 1=fullscreen, 2=toggle
     */
    virtual void setFullscreen(int action) = 0;

    /**
     * Get fullscreen state
     * @return true if fullscreen, false otherwise
     */
    virtual bool getFullscreen() const = 0;

    /**
     * Set always-on-top
     * @param action 0=normal, 1=ontop, 2=toggle
     */
    virtual void setOnTop(int action) = 0;

    /**
     * Get always-on-top state
     * @return true if on top, false otherwise
     */
    virtual bool getOnTop() const = 0;

    /**
     * Check if backend supports multi-display
     * @return true if supported, false otherwise
     */
    virtual bool supportsMultiDisplay() const = 0;

    /**
     * Get OpenGL context (for OpenGL backends)
     * @return Pointer to context, or nullptr if not applicable
     */
    virtual void* getContext() { return nullptr; }

    /**
     * Make this display's OpenGL context current
     * Required for backends that support OpenGL rendering
     */
    virtual void makeCurrent() {}

    /**
     * Clear the current OpenGL context
     */
    virtual void clearCurrent() {}

#ifdef HAVE_EGL
    /**
     * Get EGL display (for VAAPI zero-copy interop)
     * @return EGL display handle, or nullptr if not available
     */
    virtual EGLDisplay getEGLDisplay() const { return nullptr; }

    /**
     * Check if backend supports VAAPI hardware decode
     * @return true if VAAPI is supported
     */
    virtual bool hasVaapiSupport() const { return false; }

    /**
     * Get EGL extension function pointers (for VaapiInterop)
     */
    virtual PFNEGLCREATEIMAGEKHRPROC getEglCreateImageKHR() const { return nullptr; }
    virtual PFNEGLDESTROYIMAGEKHRPROC getEglDestroyImageKHR() const { return nullptr; }
    virtual PFNGLEGLIMAGETARGETTEXTURE2DOESPROC getGlEGLImageTargetTexture2DOES() const { return nullptr; }
#endif

#ifdef HAVE_VAAPI_INTEROP
    /**
     * Get VAAPI display (shared between decoder and EGL interop)
     * @return VAAPI display handle, or nullptr if not available
     */
    virtual VADisplay getVADisplay() const { return nullptr; }
#endif
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_DISPLAYBACKEND_H

