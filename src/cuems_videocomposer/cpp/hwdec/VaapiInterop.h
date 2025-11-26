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
// GL_OES_EGL_image extension function type
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum, void*);

#include <drm_fourcc.h>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/frame.h>
}

namespace videocomposer {

// Forward declaration
class OpenGLDisplay;

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
     * Initialize with OpenGLDisplay (gets EGL display and extension functions)
     * @param display OpenGLDisplay instance with EGL initialized
     * @return true if initialization succeeded
     */
    bool init(OpenGLDisplay* display);
    
    /**
     * Import VAAPI frame to OpenGL textures (zero-copy)
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
    
private:
    // EGL handles (from OpenGLDisplay)
    EGLDisplay eglDisplay_;
    
    // EGL extension function pointers (from OpenGLDisplay)
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_;
    
    // State
    bool initialized_;
    
    // Current frame state
    EGLImageKHR eglImageY_;
    EGLImageKHR eglImageUV_;
    GLuint textureY_;
    GLuint textureUV_;
    
    // Cached frame info
    int frameWidth_;
    int frameHeight_;
    
    // Create EGL image from DMA-BUF descriptor
    EGLImageKHR createEGLImageFromDmaBuf(
        int fd, int width, int height,
        uint32_t fourcc, uint32_t offset, uint32_t pitch);
};

} // namespace videocomposer

#endif // HAVE_VAAPI_INTEROP
#endif // VIDEOCOMPOSER_VAAPIINTEROP_H

