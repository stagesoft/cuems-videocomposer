#ifndef VIDEOCOMPOSER_WAYLANDDISPLAY_H
#define VIDEOCOMPOSER_WAYLANDDISPLAY_H

#include "DisplayBackend.h"
#include "OpenGLRenderer.h"
#include <memory>

#ifdef HAVE_WAYLAND

// Forward declarations for Wayland types
struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_surface;
struct wl_egl_window;
struct wl_array;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct zwp_linux_dmabuf_v1;
struct wp_presentation;

// EGL forward declarations for platform-specific types (not in DisplayBackend)
#ifdef HAVE_EGL
typedef void* EGLSurface;
typedef void* EGLConfig;
#endif

namespace videocomposer {

// Forward declaration
class LayerManager;

/**
 * WaylandDisplay - Wayland-based display backend with EGL/OpenGL (Linux only)
 * 
 * Implements DisplayBackend using Wayland + EGL + OpenGL on Linux.
 * Supports VAAPI zero-copy via DMA-BUF import using zwp_linux_dmabuf_v1 protocol.
 * 
 * Note: This backend uses Wayland for windowing and EGL for OpenGL context.
 *       For X11 systems, use X11Display instead.
 */
class WaylandDisplay : public DisplayBackend {
public:
    WaylandDisplay();
    virtual ~WaylandDisplay();

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
    bool supportsMultiDisplay() const override { return false; }  // Wayland doesn't have Xinerama-like API
    void* getContext() override;

    // OpenGL context management (override DisplayBackend interface)
    void makeCurrent() override;
    void clearCurrent() override;

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

    // Wayland registry callbacks (static for C API) - must be public to be called from C structs
    static void registryHandleGlobal(void* data, struct wl_registry* registry,
                                     uint32_t name, const char* interface, uint32_t version);
    static void registryHandleGlobalRemove(void* data, struct wl_registry* registry, uint32_t name);
    
    // XDG shell callbacks (static for C API) - must be public
    static void xdgWmBasePing(void* data, struct xdg_wm_base* xdg_wm_base, uint32_t serial);
    static void xdgSurfaceConfigure(void* data, struct xdg_surface* xdg_surface, uint32_t serial);
    static void xdgToplevelConfigure(void* data, struct xdg_toplevel* xdg_toplevel,
                                    int32_t width, int32_t height, ::wl_array* states);
    static void xdgToplevelClose(void* data, struct xdg_toplevel* xdg_toplevel);

private:
    // Wayland initialization
    bool initWayland();
    void cleanupWayland();
    
    // EGL initialization for OpenGL
    bool initEGL();
    void cleanupEGL();
    bool queryEGLExtensions();
    
    // VAAPI initialization
    bool initVAAPI();
    void cleanupVAAPI();
    
    // Wayland event handling
    void handleWaylandEvents();
    
    // OpenGL context management (implementation details)
    void swapBuffers();

    // OpenGL renderer
    std::unique_ptr<OpenGLRenderer> renderer_;
    
    // OSD renderer
    std::unique_ptr<class OSDRenderer> osdRenderer_;

    // Wayland core objects
    struct wl_display* wlDisplay_;
    struct wl_registry* wlRegistry_;
    struct wl_compositor* wlCompositor_;
    struct wl_surface* wlSurface_;
    struct wl_egl_window* wlEglWindow_;
    
    // XDG shell (window management)
    struct xdg_wm_base* xdgWmBase_;
    struct xdg_surface* xdgSurface_;
    struct xdg_toplevel* xdgToplevel_;
    
    // DMA-BUF protocol (for VAAPI zero-copy)
    struct zwp_linux_dmabuf_v1* dmabuf_;
    
    // Presentation timing (optional)
    struct wp_presentation* presentation_;
    
#ifdef HAVE_EGL
    // EGL context for OpenGL
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
    int vaDrmFd_;  // DRM render node file descriptor for VAAPI
#endif

    // Window state
    unsigned int windowWidth_;
    unsigned int windowHeight_;
    int windowX_;
    int windowY_;
    bool fullscreen_;
    bool ontop_;
    bool windowOpen_;
    bool configured_;  // Has xdg_surface been configured?
};

} // namespace videocomposer

#endif // HAVE_WAYLAND

#endif // VIDEOCOMPOSER_WAYLANDDISPLAY_H

