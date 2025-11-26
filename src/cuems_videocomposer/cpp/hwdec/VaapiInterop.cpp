#ifdef HAVE_VAAPI_INTEROP

#include "VaapiInterop.h"
#include "../display/OpenGLDisplay.h"
#include "../utils/Logger.h"

// Include GLEW before GL for proper initialization
#ifdef HAVE_GLEW
#include <GL/glew.h>
#endif
#include <GL/gl.h>

#include <unistd.h>  // for close()
#include <cstring>   // for memset()

// EGL extension constants for DMA-BUF import
#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT             0x3270
#endif
#ifndef EGL_LINUX_DRM_FOURCC_EXT
#define EGL_LINUX_DRM_FOURCC_EXT          0x3271
#endif
#ifndef EGL_DMA_BUF_PLANE0_FD_EXT
#define EGL_DMA_BUF_PLANE0_FD_EXT         0x3272
#endif
#ifndef EGL_DMA_BUF_PLANE0_OFFSET_EXT
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT     0x3273
#endif
#ifndef EGL_DMA_BUF_PLANE0_PITCH_EXT
#define EGL_DMA_BUF_PLANE0_PITCH_EXT      0x3274
#endif
#ifndef EGL_WIDTH
#define EGL_WIDTH                         0x3057
#endif
#ifndef EGL_HEIGHT
#define EGL_HEIGHT                        0x3056
#endif

namespace videocomposer {

VaapiInterop::VaapiInterop()
    : eglDisplay_(EGL_NO_DISPLAY)
    , eglCreateImageKHR_(nullptr)
    , eglDestroyImageKHR_(nullptr)
    , glEGLImageTargetTexture2DOES_(nullptr)
    , initialized_(false)
    , eglImageY_(EGL_NO_IMAGE_KHR)
    , eglImageUV_(EGL_NO_IMAGE_KHR)
    , textureY_(0)
    , textureUV_(0)
    , frameWidth_(0)
    , frameHeight_(0)
{
}

VaapiInterop::~VaapiInterop() {
    releaseFrame();
    
    // Delete textures
    if (textureY_ != 0) {
        glDeleteTextures(1, &textureY_);
        textureY_ = 0;
    }
    if (textureUV_ != 0) {
        glDeleteTextures(1, &textureUV_);
        textureUV_ = 0;
    }
}

bool VaapiInterop::init(OpenGLDisplay* display) {
    if (!display) {
        LOG_ERROR << "VaapiInterop: OpenGLDisplay is null";
        return false;
    }
    
    if (!display->hasVaapiSupport()) {
        LOG_WARNING << "VaapiInterop: OpenGLDisplay does not have VAAPI support";
        return false;
    }
    
    // Get EGL display and extension functions from OpenGLDisplay
    eglDisplay_ = display->getEGLDisplay();
    if (eglDisplay_ == EGL_NO_DISPLAY) {
        LOG_ERROR << "VaapiInterop: Invalid EGL display";
        return false;
    }
    
    eglCreateImageKHR_ = display->getEglCreateImageKHR();
    eglDestroyImageKHR_ = display->getEglDestroyImageKHR();
    glEGLImageTargetTexture2DOES_ = display->getGlEGLImageTargetTexture2DOES();
    
    if (!eglCreateImageKHR_ || !eglDestroyImageKHR_ || !glEGLImageTargetTexture2DOES_) {
        LOG_ERROR << "VaapiInterop: Missing EGL extension functions";
        return false;
    }
    
    // Generate textures for Y and UV planes
    glGenTextures(1, &textureY_);
    glGenTextures(1, &textureUV_);
    
    if (textureY_ == 0 || textureUV_ == 0) {
        LOG_ERROR << "VaapiInterop: Failed to generate textures";
        return false;
    }
    
    // Setup texture parameters
    glBindTexture(GL_TEXTURE_2D, textureY_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    glBindTexture(GL_TEXTURE_2D, textureUV_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    glBindTexture(GL_TEXTURE_2D, 0);
    
    initialized_ = true;
    LOG_INFO << "VaapiInterop initialized successfully";
    
    return true;
}

bool VaapiInterop::importFrame(AVFrame* vaapiFrame,
                               GLuint& texY, GLuint& texUV,
                               int& width, int& height) {
    if (!initialized_) {
        LOG_ERROR << "VaapiInterop: Not initialized";
        return false;
    }
    
    if (!vaapiFrame) {
        LOG_ERROR << "VaapiInterop: Null frame";
        return false;
    }
    
    if (vaapiFrame->format != AV_PIX_FMT_VAAPI) {
        LOG_ERROR << "VaapiInterop: Frame is not VAAPI format (format=" << vaapiFrame->format << ")";
        return false;
    }
    
    if (!vaapiFrame->hw_frames_ctx) {
        LOG_ERROR << "VaapiInterop: Frame has no hardware context";
        return false;
    }
    
    // Release previous frame resources
    releaseFrame();
    
    // Get VAAPI surface from AVFrame
    // For VAAPI frames, data[3] contains the VASurfaceID
    VASurfaceID surface = (VASurfaceID)(uintptr_t)vaapiFrame->data[3];
    
    // Get VAAPI device context from frame
    AVHWFramesContext* hwFramesCtx = (AVHWFramesContext*)vaapiFrame->hw_frames_ctx->data;
    if (!hwFramesCtx || !hwFramesCtx->device_ctx) {
        LOG_ERROR << "VaapiInterop: Invalid hardware frames context";
        return false;
    }
    
    AVVAAPIDeviceContext* vaapiDevCtx = (AVVAAPIDeviceContext*)hwFramesCtx->device_ctx->hwctx;
    if (!vaapiDevCtx) {
        LOG_ERROR << "VaapiInterop: Invalid VAAPI device context";
        return false;
    }
    
    VADisplay vaDisplay = vaapiDevCtx->display;
    
    // Export surface to DMA-BUF using vaExportSurfaceHandle
    VADRMPRIMESurfaceDescriptor desc;
    memset(&desc, 0, sizeof(desc));
    
    VAStatus vaStatus = vaExportSurfaceHandle(
        vaDisplay, surface,
        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &desc);
    
    if (vaStatus != VA_STATUS_SUCCESS) {
        LOG_ERROR << "VaapiInterop: vaExportSurfaceHandle failed: " << vaStatus;
        return false;
    }
    
    // Sync the surface to ensure decoding is complete
    vaStatus = vaSyncSurface(vaDisplay, surface);
    if (vaStatus != VA_STATUS_SUCCESS) {
        LOG_WARNING << "VaapiInterop: vaSyncSurface failed: " << vaStatus;
        // Continue anyway - the frame might still be usable
    }
    
    width = desc.width;
    height = desc.height;
    frameWidth_ = width;
    frameHeight_ = height;
    
    LOG_VERBOSE << "VaapiInterop: Exported surface " << surface 
               << " (" << width << "x" << height << ")"
               << " fourcc=" << std::hex << desc.fourcc << std::dec
               << " layers=" << desc.num_layers
               << " objects=" << desc.num_objects;
    
    // Verify we have NV12 format (expected from VAAPI H.264 decoding)
    if (desc.fourcc != VA_FOURCC_NV12 && desc.fourcc != VA_FOURCC('N','V','1','2')) {
        LOG_WARNING << "VaapiInterop: Unexpected fourcc: " << std::hex << desc.fourcc 
                   << " (expected NV12)";
        // Continue anyway - DRM fourcc might differ
    }
    
    // For NV12, we expect 2 layers: Y and UV
    if (desc.num_layers < 2) {
        LOG_ERROR << "VaapiInterop: Not enough layers for NV12 (got " << desc.num_layers << ")";
        // Close DMA-BUF fds
        for (uint32_t i = 0; i < desc.num_objects; i++) {
            close(desc.objects[i].fd);
        }
        return false;
    }
    
    // Create EGL image for Y plane (R8 format, full resolution)
    int yFd = desc.objects[desc.layers[0].object_index[0]].fd;
    uint32_t yOffset = desc.layers[0].offset[0];
    uint32_t yPitch = desc.layers[0].pitch[0];
    
    eglImageY_ = createEGLImageFromDmaBuf(
        yFd, width, height,
        DRM_FORMAT_R8, yOffset, yPitch);
    
    if (eglImageY_ == EGL_NO_IMAGE_KHR) {
        LOG_ERROR << "VaapiInterop: Failed to create EGL image for Y plane";
        // Close DMA-BUF fds
        for (uint32_t i = 0; i < desc.num_objects; i++) {
            close(desc.objects[i].fd);
        }
        return false;
    }
    
    // Create EGL image for UV plane (GR88/RG88 format, half resolution)
    int uvFd = desc.objects[desc.layers[1].object_index[0]].fd;
    uint32_t uvOffset = desc.layers[1].offset[0];
    uint32_t uvPitch = desc.layers[1].pitch[0];
    
    eglImageUV_ = createEGLImageFromDmaBuf(
        uvFd, width / 2, height / 2,
        DRM_FORMAT_GR88, uvOffset, uvPitch);
    
    if (eglImageUV_ == EGL_NO_IMAGE_KHR) {
        LOG_ERROR << "VaapiInterop: Failed to create EGL image for UV plane";
        eglDestroyImageKHR_(eglDisplay_, eglImageY_);
        eglImageY_ = EGL_NO_IMAGE_KHR;
        // Close DMA-BUF fds
        for (uint32_t i = 0; i < desc.num_objects; i++) {
            close(desc.objects[i].fd);
        }
        return false;
    }
    
    // Bind EGL images to OpenGL textures
    glBindTexture(GL_TEXTURE_2D, textureY_);
    glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, eglImageY_);
    
    glBindTexture(GL_TEXTURE_2D, textureUV_);
    glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, eglImageUV_);
    
    glBindTexture(GL_TEXTURE_2D, 0);
    
    // Close DMA-BUF fds (EGL holds references now)
    for (uint32_t i = 0; i < desc.num_objects; i++) {
        close(desc.objects[i].fd);
    }
    
    // Return texture IDs
    texY = textureY_;
    texUV = textureUV_;
    
    LOG_VERBOSE << "VaapiInterop: Imported frame " << width << "x" << height 
               << " (Y=" << textureY_ << ", UV=" << textureUV_ << ")";
    
    return true;
}

void VaapiInterop::releaseFrame() {
    if (eglImageY_ != EGL_NO_IMAGE_KHR) {
        eglDestroyImageKHR_(eglDisplay_, eglImageY_);
        eglImageY_ = EGL_NO_IMAGE_KHR;
    }
    if (eglImageUV_ != EGL_NO_IMAGE_KHR) {
        eglDestroyImageKHR_(eglDisplay_, eglImageUV_);
        eglImageUV_ = EGL_NO_IMAGE_KHR;
    }
}

EGLImageKHR VaapiInterop::createEGLImageFromDmaBuf(
    int fd, int width, int height,
    uint32_t fourcc, uint32_t offset, uint32_t pitch) {
    
    EGLint attribs[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)fourcc,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)pitch,
        EGL_NONE
    };
    
    EGLImageKHR image = eglCreateImageKHR_(
        eglDisplay_, EGL_NO_CONTEXT,
        EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    
    if (image == EGL_NO_IMAGE_KHR) {
        EGLint error = eglGetError();
        LOG_ERROR << "VaapiInterop: eglCreateImageKHR failed (error=" << std::hex << error << ")";
    }
    
    return image;
}

} // namespace videocomposer

#endif // HAVE_VAAPI_INTEROP

