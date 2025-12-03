#ifndef VIDEOCOMPOSER_VAAPIINTEROP_H
#define VIDEOCOMPOSER_VAAPIINTEROP_H

#ifdef HAVE_VAAPI_INTEROP

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

// EGL types - use minimal definitions to avoid GLEW conflicts
#include <EGL/egl.h>
#include <EGL/eglext.h>

// GL types - minimal definitions to avoid GLEW conflicts
typedef unsigned int GLuint;
typedef unsigned int GLenum;

// Define EGL extension function pointer types if not already defined
#ifndef PFNEGLCREATEIMAGEKHRPROC
typedef EGLImageKHR (*PFNEGLCREATEIMAGEKHRPROC)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*);
#endif
#ifndef PFNEGLDESTROYIMAGEKHRPROC
typedef EGLBoolean (*PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay, EGLImageKHR);
#endif
// EGL sync extension function types
#ifndef EGL_SYNC_FENCE_KHR
#define EGL_SYNC_FENCE_KHR 0x30F9
#endif
#ifndef EGL_SYNC_NATIVE_FENCE_ANDROID
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#endif
#ifndef EGL_FOREVER_KHR
#define EGL_FOREVER_KHR 0xFFFFFFFFFFFFFFFFull
#endif
#ifndef EGL_CONDITION_SATISFIED_KHR
#define EGL_CONDITION_SATISFIED_KHR 0x30F6
#endif
#ifndef EGL_TIMEOUT_EXPIRED_KHR
#define EGL_TIMEOUT_EXPIRED_KHR 0x30F5
#endif
typedef void* EGLSyncKHR;
typedef EGLSyncKHR (*PFNEGLCREATESYNCKHRPROC)(EGLDisplay, EGLenum, const EGLint*);
typedef EGLBoolean (*PFNEGLDESTROYSYNCKHRPROC)(EGLDisplay, EGLSyncKHR);
typedef EGLint (*PFNEGLCLIENTWAITSYNCKHRPROC)(EGLDisplay, EGLSyncKHR, EGLint, uint64_t);
// GL_OES_EGL_image extension function type (for OpenGL ES)
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum, void*);
// GL_EXT_EGL_image_storage extension function type (for Desktop OpenGL)
typedef void (*PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC)(GLenum, void*, const int*);

#include <drm_fourcc.h>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/frame.h>
}

namespace videocomposer {

// Forward declaration
class DisplayBackend;

/**
 * VaapiInterop - Zero-copy VAAPI to OpenGL texture interop
 * 
 * This class enables hardware-decoded VAAPI frames to be used directly
 * as OpenGL textures without any CPU memory transfers.
 * 
 * Pipeline:
 *   VAAPI Surface → DRM PRIME FD → EGL Image → OpenGL Texture
 * 
 * The result is NV12 format textures (Y plane + UV plane) which are
 * converted to RGB by the NV12 shader during rendering.
 */
class VaapiInterop {
public:
    VaapiInterop();
    ~VaapiInterop();
    
    /**
     * Initialize with DisplayBackend (gets EGL display and extension functions)
     * @param display DisplayBackend instance with EGL initialized (X11Display or WaylandDisplay)
     * @return true if initialization succeeded
     */
    bool init(DisplayBackend* display);
    
    /**
     * Phase 1: Create EGL images from VAAPI frame (can be called from any thread)
     * This exports the VAAPI surface to DMA-BUF and creates EGL images.
     * Call bindTexturesToImages() from the GL thread to complete the import.
     * 
     * @param vaapiFrame AVFrame with format AV_PIX_FMT_VAAPI
     * @param width Output: Frame width
     * @param height Output: Frame height
     * @return true if EGL images were created successfully
     */
    bool createEGLImages(AVFrame* vaapiFrame, int& width, int& height);
    
    /**
     * Phase 2: Bind GL textures to EGL images (MUST be called from GL thread!)
     * Call this after createEGLImages() to complete the zero-copy import.
     * 
     * @param texY Output: OpenGL texture ID for Y plane
     * @param texUV Output: OpenGL texture ID for UV plane
     * @return true if texture binding succeeded
     */
    bool bindTexturesToImages(GLuint& texY, GLuint& texUV);
    
    /**
     * Combined import - does both phases in one call
     * Only works if called from the GL thread (requires current GL context)
     * 
     * @param vaapiFrame AVFrame with format AV_PIX_FMT_VAAPI
     * @param texY Output: OpenGL texture ID for Y plane
     * @param texUV Output: OpenGL texture ID for UV plane
     * @param width Output: Frame width
     * @param height Output: Frame height
     * @return true if import succeeded
     */
    bool importFrame(AVFrame* vaapiFrame, 
                    GLuint& texY, GLuint& texUV,
                    int& width, int& height);
    
    /**
     * Release resources for the current imported frame
     * Must be called before importing a new frame
     */
    void releaseFrame();
    
    /**
     * Release the current frame immediately after rendering (MPV pattern)
     * This returns the VAAPI surface to the pool while keeping textures alive
     * Should be called right after the frame is rendered
     */
    void releaseCurrentFrame();
    
    /**
     * Check if EGL images are ready for binding (created but not yet bound)
     */
    bool hasEGLImages() const { return eglImageY_ != EGL_NO_IMAGE_KHR && eglImageUV_ != EGL_NO_IMAGE_KHR; }
    
    /**
     * Get cached frame dimensions (valid after createEGLImages)
     */
    int getFrameWidth() const { return frameWidth_; }
    int getFrameHeight() const { return frameHeight_; }
    
    /**
     * Check if VAAPI interop is available and initialized
     */
    bool isAvailable() const { return initialized_; }
    
    /**
     * Get the Y plane texture ID (valid after successful importFrame)
     */
    GLuint getTextureY() const { return textureY_; }
    
    /**
     * Get the UV plane texture ID (valid after successful importFrame)
     */
    GLuint getTextureUV() const { return textureUV_; }
    
    /**
     * Get the shared VADisplay (for use by FFmpeg decoder)
     * This MUST be the same display used for EGL interop to enable zero-copy
     */
    VADisplay getVADisplay() const { return vaDisplay_; }
    
private:
    // VAAPI display (shared with FFmpeg decoder for zero-copy)
    VADisplay vaDisplay_;
    
    // EGL handles (from DisplayBackend)
    EGLDisplay eglDisplay_;
    
    // EGL extension function pointers (from DisplayBackend)
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_;
    // GL_OES_EGL_image - works on both ES and Desktop GL, best DRM/KMS compatibility
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_;
    
    // EGL sync extension functions
    PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR_;
    PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR_;
    PFNEGLCLIENTWAITSYNCKHRPROC eglClientWaitSyncKHR_;
    bool hasSyncSupport_;
    
    // State
    bool initialized_;
    
    // Current frame state
    EGLImageKHR eglImageY_;
    EGLImageKHR eglImageUV_;
    // Previous EGL images - kept alive until new textures are bound
    EGLImageKHR prevEglImageY_;
    EGLImageKHR prevEglImageUV_;
    GLuint textureY_;
    GLuint textureUV_;
    
    // Cached frame info
    int frameWidth_;
    int frameHeight_;
    
    // Frame reference management - keeps VAAPI surfaces alive during GPU rendering
    // This is CRITICAL: we must keep AVFrames alive while GPU is still rendering from their surfaces
    // We need TWO frame references because:
    //   - currentFrame_: being rendered by GPU right now
    //   - previousFrame_: just imported, will be rendered next frame
    AVFrame* previousFrame_;   // Frame N-1 (being rendered)
    AVFrame* currentFrame_;    // Frame N (just imported, queued for rendering)
    
    // ========== DEBUG INSTRUMENTATION ==========
    // Frame counter for debugging frozen frames issue
    uint64_t debugFrameCounter_;
    // Last surface ID for debugging
    VASurfaceID debugLastSurfaceId_;
    // Last DMA-BUF FD numbers for debugging
    int debugLastYFd_;
    int debugLastUVFd_;
    // Last EGL image handles for debugging
    EGLImageKHR debugLastEglImageY_;
    EGLImageKHR debugLastEglImageUV_;
    // Enable detailed texture readback debugging (expensive!)
    bool debugReadbackEnabled_;
    
    // ========== EXPERIMENTAL FIXES ==========
    // Keep DMA-BUF FDs open until frame is released (instead of closing immediately)
    // Some drivers may lose reference when FD is closed
    bool keepFDsOpen_;
    int openYFd_;
    int openUVFd_;
    
    // Use EGL sync fence to ensure DMA-BUF is ready before texture sampling
    bool useEglSync_;
    
    // Enable VAAPI surface readback debugging (very expensive!)
    bool debugSurfaceReadbackEnabled_;
    
    // Create EGL image from DMA-BUF descriptor
    EGLImageKHR createEGLImageFromDmaBuf(
        int fd, int width, int height,
        uint32_t fourcc, uint32_t offset, uint32_t pitch, uint64_t modifier = 0);
    
    // Debug: Read back Y texture data and log first few pixels
    void debugReadbackYTexture();
    
    // Debug: Read back VAAPI surface data to verify decoded content
    void debugReadbackVaapiSurface(VASurfaceID surface, VADisplay vaDisplay, int width, int height);
};

} // namespace videocomposer

#endif // HAVE_VAAPI_INTEROP
#endif // VIDEOCOMPOSER_VAAPIINTEROP_H

