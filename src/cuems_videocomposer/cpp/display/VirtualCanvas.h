/**
 * VirtualCanvas.h - Unified rendering target for all outputs
 * 
 * Part of the Virtual Canvas architecture for cuems-videocomposer.
 * 
 * The VirtualCanvas is a single FBO sized to contain all output regions.
 * All layers are composited here first, then regions are extracted and
 * blitted to physical outputs with optional edge blending and warping.
 * 
 * Benefits:
 * - Layers can span across multiple outputs
 * - Edge blending is applied uniformly after composition
 * - Single coordinate system for all content
 * - Easy capture for virtual outputs (NDI, recording)
 */

#ifndef VIDEOCOMPOSER_VIRTUALCANVAS_H
#define VIDEOCOMPOSER_VIRTUALCANVAS_H

#include <GL/glew.h>  // For GLuint
#include <vector>

#ifdef HAVE_EGL
#include <EGL/egl.h>
#endif

namespace videocomposer {

/**
 * VirtualCanvas - Single FBO for compositing all layers
 * 
 * Usage:
 *   canvas.init(eglDisplay, eglContext);
 *   canvas.configure(3840, 1080);  // Two 1920x1080 outputs side by side
 *   
 *   // In render loop:
 *   canvas.beginFrame();
 *   renderer.compositeLayers(layers);
 *   canvas.endFrame();
 *   
 *   // Then blit regions to outputs
 *   blitShader.blit(canvas.getTexture(), outputRegion);
 */
class VirtualCanvas {
public:
    VirtualCanvas();
    ~VirtualCanvas();
    
    // Disable copy
    VirtualCanvas(const VirtualCanvas&) = delete;
    VirtualCanvas& operator=(const VirtualCanvas&) = delete;
    
    // ===== Initialization =====
    
    /**
     * Initialize the virtual canvas
     * @param eglDisplay EGL display (for context validation)
     * @param eglContext EGL context (must be current when calling GL functions)
     * @return true on success
     */
#ifdef HAVE_EGL
    bool init(EGLDisplay eglDisplay, EGLContext eglContext);
#else
    bool init(void* display = nullptr, void* context = nullptr);
#endif
    
    /**
     * Cleanup all resources
     */
    void cleanup();
    
    /**
     * Check if initialized
     */
    bool isInitialized() const { return initialized_; }
    
    // ===== Configuration =====
    
    /**
     * Configure canvas size
     * Creates or resizes the FBO to the specified dimensions.
     * Should be called when output configuration changes.
     * 
     * @param width Total canvas width in pixels
     * @param height Total canvas height in pixels
     * @return true if FBO was created/resized successfully
     */
    bool configure(int width, int height);
    
    /**
     * Get canvas dimensions
     */
    int getWidth() const { return width_; }
    int getHeight() const { return height_; }
    
    // ===== Rendering =====
    
    /**
     * Begin rendering to the canvas
     * Binds the FBO, sets viewport, and clears to black.
     * Call before rendering layers.
     */
    void beginFrame();
    
    /**
     * End rendering to the canvas
     * Unbinds the FBO (restores default framebuffer).
     * Call after rendering layers.
     */
    void endFrame();
    
    /**
     * Check if currently rendering (between beginFrame/endFrame)
     */
    bool isRendering() const { return rendering_; }
    
    // ===== Texture Access =====
    
    /**
     * Get the canvas texture for blitting to outputs
     * @return OpenGL texture ID (RGBA8 format)
     */
    GLuint getTexture() const { return texture_; }
    
    /**
     * Get the FBO ID (for direct binding if needed)
     */
    GLuint getFBO() const { return fbo_; }
    
    // ===== Frame Capture (for sinks) =====
    
    /**
     * Capture the full canvas to CPU memory
     * Uses PBO for async readback to avoid stalls.
     * 
     * @param buffer Output buffer (must be width*height*4 bytes)
     * @param bufferSize Size of output buffer
     * @return true if capture succeeded
     */
    bool captureFrame(void* buffer, size_t bufferSize);
    
    /**
     * Capture a region of the canvas
     * @param x Region X offset
     * @param y Region Y offset
     * @param width Region width
     * @param height Region height
     * @param buffer Output buffer
     * @param bufferSize Size of output buffer
     * @return true if capture succeeded
     */
    bool captureRegion(int x, int y, int width, int height,
                       void* buffer, size_t bufferSize);
    
    /**
     * Start async capture (non-blocking)
     * Call getAsyncCaptureResult() to retrieve data.
     */
    void startAsyncCapture();
    
    /**
     * Check if async capture is ready
     */
    bool isAsyncCaptureReady() const;
    
    /**
     * Get async capture result
     * @param buffer Output buffer
     * @param bufferSize Size of output buffer
     * @return true if data was copied, false if not ready or failed
     */
    bool getAsyncCaptureResult(void* buffer, size_t bufferSize);
    
private:
    bool initialized_ = false;
    bool rendering_ = false;
    
    int width_ = 0;
    int height_ = 0;
    
    // FBO resources
    GLuint fbo_ = 0;
    GLuint texture_ = 0;
    GLuint depthRbo_ = 0;
    
    // PBO for async capture (double-buffered)
    GLuint pbo_[2] = {0, 0};
    int currentPbo_ = 0;
    bool pboInitialized_ = false;
    bool asyncCaptureInProgress_ = false;
    int asyncCaptureWidth_ = 0;
    int asyncCaptureHeight_ = 0;
    
#ifdef HAVE_EGL
    EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
    EGLContext eglContext_ = EGL_NO_CONTEXT;
#endif
    
    // ===== Private Methods =====
    
    /**
     * Create the FBO with specified dimensions
     */
    bool createFBO(int width, int height);
    
    /**
     * Destroy the FBO and release resources
     */
    void destroyFBO();
    
    /**
     * Initialize PBOs for async capture
     */
    bool initPBOs(int width, int height);
    
    /**
     * Destroy PBOs
     */
    void destroyPBOs();
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_VIRTUALCANVAS_H

