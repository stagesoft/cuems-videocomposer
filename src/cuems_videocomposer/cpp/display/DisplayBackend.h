#ifndef VIDEOCOMPOSER_DISPLAYBACKEND_H
#define VIDEOCOMPOSER_DISPLAYBACKEND_H

#include "../video/FrameBuffer.h"
#include "OutputInfo.h"
#include <cstdint>
#include <vector>
#include <string>

// Forward declarations for EGL/VAAPI types (avoid including headers)
#ifdef HAVE_EGL
typedef void* EGLDisplay;
typedef void* EGLContext;
typedef void* EGLImageKHR;
typedef EGLImageKHR (*PFNEGLCREATEIMAGEKHRPROC)(EGLDisplay, EGLContext, unsigned int, void*, const int*);
typedef unsigned int (*PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay, EGLImageKHR);
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(unsigned int, void*);
typedef void (*PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC)(unsigned int, void*, const int*);
#endif

#ifdef HAVE_VAAPI_INTEROP
typedef void* VADisplay;
#endif

namespace videocomposer {

// Forward declarations
class LayerManager;
class OSDManager;
class OpenGLRenderer;

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

    // ===== Multi-Output Support =====
    
    /**
     * Get number of physical outputs
     * @return Number of outputs (1 for single-display backends)
     */
    virtual size_t getOutputCount() const { return 1; }
    
    /**
     * Get list of all connected outputs with their info
     * @return Vector of OutputInfo structures
     */
    virtual std::vector<OutputInfo> getOutputs() const { return {}; }
    
    /**
     * Configure output region on virtual canvas
     * @param outputName Output name (e.g., "HDMI-A-1")
     * @param canvasX X position on canvas
     * @param canvasY Y position on canvas  
     * @param canvasWidth Width on canvas (0 = use output native width)
     * @param canvasHeight Height on canvas (0 = use output native height)
     */
    virtual bool configureOutputRegion(const std::string& outputName, int canvasX, int canvasY, 
                                        int canvasWidth = 0, int canvasHeight = 0) { 
        (void)outputName; (void)canvasX; (void)canvasY; 
        (void)canvasWidth; (void)canvasHeight;
        return false; 
    }
    
    /**
     * Configure edge blending for an output
     * @param outputName Output name
     * @param left/right/top/bottom Edge blend width in pixels
     * @param gamma Blend gamma (typically 2.2)
     */
    virtual bool configureOutputBlend(const std::string& outputName, float left, float right,
                                       float top, float bottom, float gamma = 2.2f) {
        (void)outputName; (void)left; (void)right; (void)top; (void)bottom; (void)gamma;
        return false;
    }
    
    /**
     * Set output resolution/mode
     * @param outputName Output name
     * @param width New width
     * @param height New height
     * @param refresh Refresh rate (0 = use default)
     */
    virtual bool setOutputMode(const std::string& outputName, int width, int height, double refresh = 0.0) {
        (void)outputName; (void)width; (void)height; (void)refresh;
        return false;
    }
    
    // ===== Virtual Output / Capture Support =====
    
    /**
     * Enable/disable frame capture for virtual outputs
     * @param enabled Enable or disable capture
     * @param width Capture width (0 = use canvas width)
     * @param height Capture height (0 = use canvas height)
     */
    virtual void setCaptureEnabled(bool enabled, int width = 0, int height = 0) {
        (void)enabled; (void)width; (void)height;
    }
    
    /**
     * Check if capture is enabled
     */
    virtual bool isCaptureEnabled() const { return false; }
    
    /**
     * Set output sink manager for virtual outputs
     * @param sinkManager Output sink manager (not owned)
     */
    virtual void setOutputSinkManager(class OutputSinkManager* sinkManager) {
        (void)sinkManager;
    }
    
    /**
     * Set resolution mode for all outputs
     * @param mode Mode string: "native", "maximum", "1080p", "720p", "4k"
     * @return true if mode was set (may need re-init to apply)
     */
    virtual bool setResolutionMode(const std::string& mode) {
        (void)mode;
        return false;
    }
    
    /**
     * Save display configuration to file
     * @param path File path (empty = default)
     * @return true on success
     */
    virtual bool saveConfiguration(const std::string& path = "") {
        (void)path;
        return false;
    }
    
    /**
     * Load display configuration from file
     * @param path File path (empty = default)
     * @return true on success
     */
    virtual bool loadConfiguration(const std::string& path = "") {
        (void)path;
        return false;
    }

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
    
    /**
     * Get the OpenGL renderer
     * @return Reference to the renderer, or nullptr if not initialized
     */
    virtual OpenGLRenderer* getRenderer() { return nullptr; }

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
    virtual PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC getGlEGLImageTargetTexStorageEXT() const { return nullptr; }
    
    /**
     * Check if we're running on Desktop GL (vs OpenGL ES)
     * Desktop GL should use glEGLImageTargetTexStorageEXT
     * OpenGL ES should use glEGLImageTargetTexture2DOES
     */
    virtual bool isDesktopGL() const { return false; }
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

