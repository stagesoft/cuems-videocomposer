#ifdef HAVE_VAAPI_INTEROP

#include "VaapiInterop.h"
#include "../display/DisplayBackend.h"
#include "../utils/Logger.h"

// Include GLEW before GL for proper initialization
#ifdef HAVE_GLEW
#include <GL/glew.h>
#endif
#include <GL/gl.h>

#include <unistd.h>  // for close()
#include <cstring>   // for memset()
#include <chrono>    // for timing diagnostics
#include <vector>    // for texture readback
#include <sstream>   // for debug output
#include <iomanip>   // for setprecision

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
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#endif
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#endif
#ifndef EGL_WIDTH
#define EGL_WIDTH                         0x3057
#endif
#ifndef EGL_HEIGHT
#define EGL_HEIGHT                        0x3056
#endif

namespace videocomposer {

VaapiInterop::VaapiInterop()
    : vaDisplay_(nullptr)
    , eglDisplay_(EGL_NO_DISPLAY)
    , eglCreateImageKHR_(nullptr)
    , eglDestroyImageKHR_(nullptr)
    , glEGLImageTargetTexture2DOES_(nullptr)
    , glEGLImageTargetTexStorageEXT_(nullptr)
    , isDesktopGL_(false)
    , eglCreateSyncKHR_(nullptr)
    , eglDestroySyncKHR_(nullptr)
    , eglClientWaitSyncKHR_(nullptr)
    , hasSyncSupport_(false)
    , initialized_(false)
    , eglImageY_(EGL_NO_IMAGE_KHR)
    , eglImageUV_(EGL_NO_IMAGE_KHR)
    , prevEglImageY_(EGL_NO_IMAGE_KHR)
    , prevEglImageUV_(EGL_NO_IMAGE_KHR)
    , textureY_(0)
    , textureUV_(0)
    , frameWidth_(0)
    , frameHeight_(0)
    , previousFrame_(nullptr)
    , currentFrame_(nullptr)
    // Debug instrumentation
    , debugFrameCounter_(0)
    , debugLastSurfaceId_(VA_INVALID_ID)
    , debugLastYFd_(-1)
    , debugLastUVFd_(-1)
    , debugLastEglImageY_(EGL_NO_IMAGE_KHR)
    , debugLastEglImageUV_(EGL_NO_IMAGE_KHR)
    , debugReadbackEnabled_(false)  // Set to true for detailed debugging
    // Experimental fixes
    , keepFDsOpen_(true)  // ENABLED: Keep DMA-BUF FDs open until frame release
    , openYFd_(-1)
    , openUVFd_(-1)
    , useEglSync_(true)  // ENABLED: Use EGL sync fence
    , debugSurfaceReadbackEnabled_(false)  // Set to true for VAAPI surface verification
{
}

VaapiInterop::~VaapiInterop() {
    releaseFrame();
    
    // Unref frames if they exist
    if (currentFrame_) {
        av_frame_unref(currentFrame_);
        av_frame_free(&currentFrame_);
        currentFrame_ = nullptr;
    }
    if (previousFrame_) {
        av_frame_unref(previousFrame_);
        av_frame_free(&previousFrame_);
        previousFrame_ = nullptr;
    }
    
    // Delete textures
    if (textureY_ != 0) {
        glDeleteTextures(1, &textureY_);
        textureY_ = 0;
    }
    if (textureUV_ != 0) {
        glDeleteTextures(1, &textureUV_);
        textureUV_ = 0;
    }
    
    // Close any remaining open FDs
    if (openYFd_ >= 0) {
        close(openYFd_);
        openYFd_ = -1;
    }
    if (openUVFd_ >= 0) {
        close(openUVFd_);
        openUVFd_ = -1;
    }
}

bool VaapiInterop::init(DisplayBackend* display) {
    if (!display) {
        LOG_ERROR << "VaapiInterop: DisplayBackend is null";
        return false;
    }
    
    if (!display->hasVaapiSupport()) {
        LOG_WARNING << "VaapiInterop: DisplayBackend does not have VAAPI support";
        return false;
    }
    
    // Get EGL display and extension functions from DisplayBackend
    eglDisplay_ = display->getEGLDisplay();
    if (eglDisplay_ == EGL_NO_DISPLAY) {
        LOG_ERROR << "VaapiInterop: Invalid EGL display";
        return false;
    }
    
    eglCreateImageKHR_ = display->getEglCreateImageKHR();
    eglDestroyImageKHR_ = display->getEglDestroyImageKHR();
    glEGLImageTargetTexture2DOES_ = display->getGlEGLImageTargetTexture2DOES();
    glEGLImageTargetTexStorageEXT_ = display->getGlEGLImageTargetTexStorageEXT();
    isDesktopGL_ = display->isDesktopGL();
    
    // MPV approach: Use TexStorageEXT on Desktop GL, Texture2DOES on ES
    // DRM/KMS always uses Desktop GL, so we prefer TexStorageEXT there
    if (isDesktopGL_) {
        if (!glEGLImageTargetTexStorageEXT_) {
            LOG_WARNING << "VaapiInterop: glEGLImageTargetTexStorageEXT not available, trying Texture2DOES";
            if (!glEGLImageTargetTexture2DOES_) {
                LOG_ERROR << "VaapiInterop: No EGL image target function available";
                return false;
            }
            LOG_INFO << "VaapiInterop: Using glEGLImageTargetTexture2DOES (fallback)";
    } else {
            LOG_INFO << "VaapiInterop: Using glEGLImageTargetTexStorageEXT (Desktop GL/DRM)";
        }
    } else {
        if (!glEGLImageTargetTexture2DOES_) {
            LOG_ERROR << "VaapiInterop: glEGLImageTargetTexture2DOES not available";
        return false;
        }
        LOG_INFO << "VaapiInterop: Using glEGLImageTargetTexture2DOES (OpenGL ES)";
    }
    
    if (!eglCreateImageKHR_ || !eglDestroyImageKHR_) {
        LOG_ERROR << "VaapiInterop: Missing EGL extension functions";
        return false;
    }
    
    // Load EGL sync extension functions (optional - for synchronization)
    eglCreateSyncKHR_ = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    eglDestroySyncKHR_ = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    eglClientWaitSyncKHR_ = (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");
    
    if (eglCreateSyncKHR_ && eglDestroySyncKHR_ && eglClientWaitSyncKHR_) {
        hasSyncSupport_ = true;
        LOG_INFO << "VaapiInterop: EGL sync extension available (eglCreateSyncKHR)";
    } else {
        hasSyncSupport_ = false;
        LOG_INFO << "VaapiInterop: EGL sync extension not available";
    }
    
    // Get VAAPI display (shared with FFmpeg decoder for zero-copy)
    vaDisplay_ = display->getVADisplay();
    if (!vaDisplay_) {
        LOG_WARNING << "VaapiInterop: No VADisplay available - zero-copy may not work";
        // Continue anyway - we might still be able to use the CPU copy path
    } else {
        LOG_INFO << "VaapiInterop: Using shared VADisplay for zero-copy";
    }
    
    // MPV approach: Textures are NOT created in init()
    // - For glEGLImageTargetTexture2DOES (ES): textures created once in init and reused
    // Textures created on first bind, reused for subsequent frames
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
    
    // Debug: Check if the VADisplay from the frame matches our shared display
    if (vaDisplay_ && vaDisplay != vaDisplay_) {
        LOG_WARNING << "VaapiInterop: Frame VADisplay (" << vaDisplay 
                   << ") differs from shared VADisplay (" << vaDisplay_ << ")";
    }
    
    // CRITICAL: Sync BEFORE export to ensure decoding is complete (like mpv)
    VAStatus vaStatus = vaSyncSurface(vaDisplay, surface);
    if (vaStatus != VA_STATUS_SUCCESS) {
        LOG_WARNING << "VaapiInterop: vaSyncSurface (pre-export) failed: " << vaStatus;
        // Continue anyway - export might still work
    }
    
    // Export surface to DMA-BUF using vaExportSurfaceHandle
    VADRMPRIMESurfaceDescriptor desc;
    memset(&desc, 0, sizeof(desc));
    
    // Use SEPARATE_LAYERS like mpv does for EGL interop
    // Each plane becomes a separate layer with its own DRM format
    vaStatus = vaExportSurfaceHandle(
        vaDisplay, surface,
        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &desc);
    
    if (vaStatus != VA_STATUS_SUCCESS) {
        LOG_ERROR << "VaapiInterop: vaExportSurfaceHandle failed: " << vaStatus;
        return false;
    }
    
    // NOTE: Post-export sync removed - pre-export sync is sufficient
    // EGL image import provides implicit synchronization
    
    width = desc.width;
    height = desc.height;
    frameWidth_ = width;
    frameHeight_ = height;
    
    // Verify we have NV12 format (expected from VAAPI H.264 decoding)
    if (desc.fourcc != VA_FOURCC_NV12 && desc.fourcc != VA_FOURCC('N','V','1','2')) {
        LOG_WARNING << "VaapiInterop: Unexpected fourcc: " << std::hex << desc.fourcc 
                   << " (expected NV12)";
        // Continue anyway - DRM fourcc might differ
    }
    
    // With COMPOSED_LAYERS, we get 1 layer with 2 planes for NV12
    // With SEPARATE_LAYERS, we get 2 layers with 1 plane each
    int yObjectIdx, uvObjectIdx;
    uint32_t yOffset, uvOffset, yPitch, uvPitch;
    uint64_t yModifier, uvModifier;
    uint32_t yFormat, uvFormat;
    
    if (desc.num_layers == 1 && desc.layers[0].num_planes >= 2) {
        // COMPOSED_LAYERS mode: single layer with multiple planes
        yObjectIdx = desc.layers[0].object_index[0];
        yOffset = desc.layers[0].offset[0];
        yPitch = desc.layers[0].pitch[0];
        yModifier = desc.objects[yObjectIdx].drm_format_modifier;
        yFormat = DRM_FORMAT_R8;  // Y plane as R8
        
        uvObjectIdx = desc.layers[0].object_index[1];
        uvOffset = desc.layers[0].offset[1];
        uvPitch = desc.layers[0].pitch[1];
        uvModifier = desc.objects[uvObjectIdx].drm_format_modifier;
        uvFormat = DRM_FORMAT_GR88;  // UV plane as GR88
    } else if (desc.num_layers >= 2) {
        // SEPARATE_LAYERS mode: each plane is a separate layer
        yObjectIdx = desc.layers[0].object_index[0];
        yOffset = desc.layers[0].offset[0];
        yPitch = desc.layers[0].pitch[0];
        yModifier = desc.objects[yObjectIdx].drm_format_modifier;
        yFormat = desc.layers[0].drm_format;  // Use layer's actual format
        
        uvObjectIdx = desc.layers[1].object_index[0];
        uvOffset = desc.layers[1].offset[0];
        uvPitch = desc.layers[1].pitch[0];
        uvModifier = desc.objects[uvObjectIdx].drm_format_modifier;
        uvFormat = desc.layers[1].drm_format;  // Use layer's actual format
    } else {
        LOG_ERROR << "VaapiInterop: Invalid layer configuration for NV12";
        // Close DMA-BUF fds
        for (uint32_t i = 0; i < desc.num_objects; i++) {
            close(desc.objects[i].fd);
        }
        return false;
    }
    
    int yFd = desc.objects[yObjectIdx].fd;
    int uvFd = desc.objects[uvObjectIdx].fd;
    
    // Create EGL image for Y plane (full resolution)
    eglImageY_ = createEGLImageFromDmaBuf(
        yFd, width, height,
        yFormat, yOffset, yPitch, yModifier);
    
    if (eglImageY_ == EGL_NO_IMAGE_KHR) {
        LOG_ERROR << "VaapiInterop: Failed to create EGL image for Y plane";
        // Close DMA-BUF fds
        for (uint32_t i = 0; i < desc.num_objects; i++) {
            close(desc.objects[i].fd);
        }
        return false;
    }
    
    // Create EGL image for UV plane (half resolution)
    eglImageUV_ = createEGLImageFromDmaBuf(
        uvFd, width / 2, height / 2,
        uvFormat, uvOffset, uvPitch, uvModifier);
    
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
    
    
    // Close DMA-BUF fds now - EGL holds references to the underlying buffers
    for (uint32_t i = 0; i < desc.num_objects; i++) {
        close(desc.objects[i].fd);
    }
    
    // Phase 1 complete - EGL images are ready
    // Now try to bind textures (Phase 2) - this requires a GL context
    if (!bindTexturesToImages(texY, texUV)) {
        LOG_WARNING << "VaapiInterop: Phase 2 (texture binding) failed - EGL images will be kept for later binding";
        // Don't clean up EGL images - they can be bound later from the GL thread
        // Return false to indicate incomplete import
        return false;
    }
    
    
    return true;
}

bool VaapiInterop::createEGLImages(AVFrame* vaapiFrame, int& width, int& height) {
    // Create EGL images only - do NOT bind textures here
    // The caller should call bindTexturesToImages() from the GL thread
    
    if (!initialized_) {
        LOG_ERROR << "VaapiInterop: Not initialized";
        return false;
    }
    
    if (!vaapiFrame) {
        LOG_ERROR << "VaapiInterop: Null frame";
        return false;
    }
    
    if (vaapiFrame->format != AV_PIX_FMT_VAAPI) {
        LOG_ERROR << "VaapiInterop: Frame is not VAAPI format";
        return false;
    }
    
    if (!vaapiFrame->hw_frames_ctx) {
        LOG_ERROR << "VaapiInterop: Frame has no hardware context";
        return false;
    }
    
    // Get VAAPI surface from AVFrame
    VASurfaceID surface = (VASurfaceID)(uintptr_t)vaapiFrame->data[3];
    
    // CRITICAL: Save old EGL images before creating new ones
    // They must stay alive until new textures are bound (GPU might still be sampling from them)
    // They will be destroyed in bindTexturesToImages() AFTER new textures are bound
    prevEglImageY_ = eglImageY_;
    prevEglImageUV_ = eglImageUV_;
    eglImageY_ = EGL_NO_IMAGE_KHR;
    eglImageUV_ = EGL_NO_IMAGE_KHR;
    
    // CRITICAL: MPV-style immediate release pattern  
    // MPV does: map frame → render → unmap (release immediately)
    // To match this, we keep ONLY the frame being rendered (currentFrame_)
    // Old frame was already released in previous call to releaseCurrentFrame()
    
    // Allocate currentFrame_ on first use
    if (!currentFrame_) {
        currentFrame_ = av_frame_alloc();
        if (!currentFrame_) {
            LOG_ERROR << "VaapiInterop: Failed to allocate currentFrame_";
            return false;
        }
    }
    
    // CRITICAL: Release any existing frame BEFORE importing new one
    // This is necessary when multiple layers share the same VaapiInterop instance
    // Each layer may try to import a frame before the previous layer's frame is released
    // Releasing here ensures we don't have multiple frames in flight
    if (currentFrame_->buf[0]) {
        // Release the previous frame's resources (but keep EGL images/textures for now)
        // They will be cleaned up in bindTexturesToImages() after new textures are bound
        av_frame_unref(currentFrame_);
    }
    
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
    
    // Export surface to DMA-BUF
    VADRMPRIMESurfaceDescriptor desc;
    memset(&desc, 0, sizeof(desc));
    
    // CRITICAL: Sync BEFORE export to ensure decode is complete
    VAStatus syncStatus = vaSyncSurface(vaDisplay, surface);
    if (syncStatus != VA_STATUS_SUCCESS) {
        LOG_WARNING << "VaapiInterop: vaSyncSurface (pre-export) failed: " << syncStatus;
    }
    
    VAStatus vaStatus = vaExportSurfaceHandle(
        vaDisplay, surface,
        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &desc);
    
    if (vaStatus != VA_STATUS_SUCCESS) {
        LOG_ERROR << "VaapiInterop: vaExportSurfaceHandle failed: " << vaStatus;
        return false;
    }
    
    // NOTE: Post-export sync removed - pre-export sync is sufficient
    // mpv only syncs once before export, not after
    // The EGL image import provides implicit synchronization
    
    width = desc.width;
    height = desc.height;
    
    // DEBUG: Read back VAAPI surface to verify decoded content
    if (debugSurfaceReadbackEnabled_) {
        debugReadbackVaapiSurface(surface, vaDisplay, width, height);
    }
    frameWidth_ = width;
    frameHeight_ = height;
    
    // Extract plane info (same logic as importFrame)
    int yObjectIdx, uvObjectIdx;
    uint32_t yOffset, uvOffset, yPitch, uvPitch;
    uint64_t yModifier, uvModifier;
    uint32_t yFormat, uvFormat;
    int yFd, uvFd;
    
    if (desc.num_layers == 2) {
        // SEPARATE_LAYERS mode - force R8/GR88 like mpv does for NV12
        yObjectIdx = desc.layers[0].object_index[0];
        yOffset = desc.layers[0].offset[0];
        yPitch = desc.layers[0].pitch[0];
        yFormat = DRM_FORMAT_R8;  // Force R8 for Y plane like mpv
        yModifier = desc.objects[yObjectIdx].drm_format_modifier;
        yFd = dup(desc.objects[yObjectIdx].fd);
        
        uvObjectIdx = desc.layers[1].object_index[0];
        uvOffset = desc.layers[1].offset[0];
        uvPitch = desc.layers[1].pitch[0];
        uvFormat = DRM_FORMAT_GR88;  // Force GR88 for UV plane like mpv
        uvModifier = desc.objects[uvObjectIdx].drm_format_modifier;
        uvFd = dup(desc.objects[uvObjectIdx].fd);
        
    } else if (desc.num_layers == 1 && desc.layers[0].num_planes >= 2) {
        // COMPOSED_LAYERS mode
        yObjectIdx = desc.layers[0].object_index[0];
        yOffset = desc.layers[0].offset[0];
        yPitch = desc.layers[0].pitch[0];
        yModifier = desc.objects[yObjectIdx].drm_format_modifier;
        yFormat = DRM_FORMAT_R8;
        yFd = dup(desc.objects[yObjectIdx].fd);
        
        uvObjectIdx = desc.layers[0].object_index[1];
        uvOffset = desc.layers[0].offset[1];
        uvPitch = desc.layers[0].pitch[1];
        uvModifier = desc.objects[uvObjectIdx].drm_format_modifier;
        uvFormat = DRM_FORMAT_GR88;
        uvFd = dup(desc.objects[uvObjectIdx].fd);
    } else {
        LOG_ERROR << "VaapiInterop: Unsupported layer configuration";
        for (uint32_t i = 0; i < desc.num_objects; i++) {
            close(desc.objects[i].fd);
        }
        return false;
    }
    
    // Close original fds (we duped them)
    for (uint32_t i = 0; i < desc.num_objects; i++) {
        close(desc.objects[i].fd);
    }
    
    // Create EGL images
    eglImageY_ = createEGLImageFromDmaBuf(yFd, width, height, yFormat, yOffset, yPitch, yModifier);
    
    if (eglImageY_ == EGL_NO_IMAGE_KHR) {
        LOG_ERROR << "VaapiInterop: Failed to create EGL image for Y plane";
        close(yFd);
        close(uvFd);
        return false;
    }
    
    eglImageUV_ = createEGLImageFromDmaBuf(uvFd, width / 2, height / 2, uvFormat, uvOffset, uvPitch, uvModifier);
    
    // Handle FD lifetime based on experimental flag
    if (keepFDsOpen_) {
        // Keep FDs open until frame is released - driver may need them
        // Close any previously open FDs first
        if (openYFd_ >= 0) {
            close(openYFd_);
        }
        if (openUVFd_ >= 0) {
            close(openUVFd_);
        }
        openYFd_ = yFd;
        openUVFd_ = uvFd;
    } else {
        // Close FDs immediately (original behavior)
        close(yFd);
        close(uvFd);
    }
    
    if (eglImageUV_ == EGL_NO_IMAGE_KHR) {
        LOG_ERROR << "VaapiInterop: Failed to create EGL image for UV plane";
        eglDestroyImageKHR_(eglDisplay_, eglImageY_);
        eglImageY_ = EGL_NO_IMAGE_KHR;
        return false;
    }
    
    // CRITICAL: Clone the new frame to currentFrame_ (increment ref count)
    // This keeps the VAAPI surface alive while we use its EGL images
    // We do this AFTER all validation and EGL image creation succeeds
    int ret = av_frame_ref(currentFrame_, vaapiFrame);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        LOG_ERROR << "VaapiInterop: Failed to ref frame: " << errbuf;
        // Clean up EGL images since we can't keep the surface alive
        eglDestroyImageKHR_(eglDisplay_, eglImageY_);
        eglDestroyImageKHR_(eglDisplay_, eglImageUV_);
        eglImageY_ = EGL_NO_IMAGE_KHR;
        eglImageUV_ = EGL_NO_IMAGE_KHR;
        return false;
    }
    
    return true;
}

bool VaapiInterop::bindTexturesToImages(GLuint& texY, GLuint& texUV) {
    if (!hasEGLImages()) {
        LOG_ERROR << "VaapiInterop: No EGL images to bind";
        return false;
    }
    
    // Check if we have a current EGL context
    EGLContext currentContext = eglGetCurrentContext();
    if (currentContext == EGL_NO_CONTEXT) {
        return false;
    }
    
    // MPV order: delete textures FIRST, then destroy EGL images
    // The texture was bound to the old EGL image, so delete it before destroying the image
    EGLDisplay currentDisplay = eglGetCurrentDisplay();
    if (currentDisplay == EGL_NO_DISPLAY) {
        currentDisplay = eglDisplay_;
    }
    
    // Ensure GPU is done with old textures before we delete them
    if (textureY_ != 0 || textureUV_ != 0) {
        glFinish();  // Wait for all GL commands to complete
    }
    
    // Step 1: Delete old textures (they were bound to old EGL images)
    if (textureY_ != 0) {
        glDeleteTextures(1, &textureY_);
        textureY_ = 0;
    }
    if (textureUV_ != 0) {
        glDeleteTextures(1, &textureUV_);
        textureUV_ = 0;
    }
    
    // Step 2: Now safe to destroy the old EGL images
    if (prevEglImageY_ != EGL_NO_IMAGE_KHR) {
        eglDestroyImageKHR_(currentDisplay, prevEglImageY_);
        prevEglImageY_ = EGL_NO_IMAGE_KHR;
    }
    if (prevEglImageUV_ != EGL_NO_IMAGE_KHR) {
        eglDestroyImageKHR_(currentDisplay, prevEglImageUV_);
        prevEglImageUV_ = EGL_NO_IMAGE_KHR;
    }
    
        glGenTextures(1, &textureY_);
        glGenTextures(1, &textureUV_);
        
        if (textureY_ == 0 || textureUV_ == 0) {
            LOG_ERROR << "VaapiInterop: Failed to create textures";
            return false;
        }
        
    // Set texture parameters before binding EGL image
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
    
    // Clear any pending GL errors before we start
    while (glGetError() != GL_NO_ERROR) {}
    
    // Bind Y plane texture using the appropriate extension (mpv approach)
    glBindTexture(GL_TEXTURE_2D, textureY_);
    bool bindSuccess = false;
    
    if (isDesktopGL_ && glEGLImageTargetTexStorageEXT_) {
        // Desktop GL path (DRM/KMS) - use TexStorageEXT like mpv
        glEGLImageTargetTexStorageEXT_(GL_TEXTURE_2D, eglImageY_, nullptr);
        GLenum err = glGetError();
        if (err == GL_NO_ERROR) {
            bindSuccess = true;
        } else {
            static int errCount = 0;
            if (errCount++ < 5) {
                LOG_WARNING << "VaapiInterop: glEGLImageTargetTexStorageEXT Y failed: 0x" 
                           << std::hex << err << std::dec;
            }
        }
    }
    
    // Fallback to Texture2DOES if TexStorageEXT not available or failed
    if (!bindSuccess && glEGLImageTargetTexture2DOES_) {
        glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, eglImageY_);
        GLenum err = glGetError();
        if (err == GL_NO_ERROR) {
            bindSuccess = true;
    } else {
            static int errCount = 0;
            if (errCount++ < 5) {
                LOG_WARNING << "VaapiInterop: glEGLImageTargetTexture2DOES Y failed: 0x" 
                           << std::hex << err << std::dec;
            }
        }
    }
    
    if (!bindSuccess) {
        LOG_ERROR << "VaapiInterop: Failed to bind Y plane";
        glBindTexture(GL_TEXTURE_2D, 0);
        return false;
    }
    
    // Bind UV plane texture
    glBindTexture(GL_TEXTURE_2D, textureUV_);
    bindSuccess = false;
    
    if (isDesktopGL_ && glEGLImageTargetTexStorageEXT_) {
        // Desktop GL path (DRM/KMS) - use TexStorageEXT like mpv
        glEGLImageTargetTexStorageEXT_(GL_TEXTURE_2D, eglImageUV_, nullptr);
        GLenum err = glGetError();
        if (err == GL_NO_ERROR) {
            bindSuccess = true;
        } else {
            static int errCount = 0;
            if (errCount++ < 5) {
                LOG_WARNING << "VaapiInterop: glEGLImageTargetTexStorageEXT UV failed: 0x" 
                           << std::hex << err << std::dec;
            }
        }
    }
    
    // Fallback to Texture2DOES
    if (!bindSuccess && glEGLImageTargetTexture2DOES_) {
        glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, eglImageUV_);
        GLenum err = glGetError();
        if (err == GL_NO_ERROR) {
            bindSuccess = true;
    } else {
            static int errCount = 0;
            if (errCount++ < 5) {
                LOG_WARNING << "VaapiInterop: glEGLImageTargetTexture2DOES UV failed: 0x" 
                           << std::hex << err << std::dec;
    }
        }
    }
    
    if (!bindSuccess) {
        LOG_ERROR << "VaapiInterop: Failed to bind UV plane";
        glBindTexture(GL_TEXTURE_2D, 0);
        return false;
    }
    
    glBindTexture(GL_TEXTURE_2D, 0);
    
    // CRITICAL: Flush GPU commands to ensure EGL image bindings are submitted
    // glFlush() submits commands to GPU without blocking
    glFlush();
    
    // EXPERIMENTAL: Use EGL sync fence to ensure texture data is ready
    // This may help with GPU caching issues by forcing synchronization
    if (useEglSync_ && hasSyncSupport_) {
        EGLDisplay currentDisplay = eglGetCurrentDisplay();
        if (currentDisplay != EGL_NO_DISPLAY) {
            // Create a sync fence
            EGLSyncKHR sync = eglCreateSyncKHR_(currentDisplay, EGL_SYNC_FENCE_KHR, nullptr);
            if (sync != nullptr) {
                // Wait on the fence (forces GPU to complete all pending operations)
                eglClientWaitSyncKHR_(currentDisplay, sync, 0, EGL_FOREVER_KHR);
                eglDestroySyncKHR_(currentDisplay, sync);
            }
        }
    }
    
    // Close old DMA-BUF FDs if keepFDsOpen_ mode was used
    // (These are from the previous frame)
    if (openYFd_ >= 0) {
        close(openYFd_);
        openYFd_ = -1;
    }
    if (openUVFd_ >= 0) {
        close(openUVFd_);
        openUVFd_ = -1;
    }
    
    // CRITICAL: NOW release the old AVFrame (after new textures bound and old EGL images destroyed)
    // Frame was already released in createEGLImages() - nothing to do here
    // The VAAPI surface was returned to the pool immediately when we imported the new frame

    
    // Return NEW texture IDs
    texY = textureY_;
    texUV = textureUV_;
    
    return true;
}

void VaapiInterop::debugReadbackYTexture() {
    // This is an EXPENSIVE operation - only enable for debugging!
    // Reads back the Y texture to verify it contains the correct frame data
    
    if (textureY_ == 0 || frameWidth_ == 0 || frameHeight_ == 0) {
        LOG_WARNING << "VaapiInterop: Cannot readback - no valid texture";
        return;
    }
    
    // Allocate buffer for Y plane (8-bit luminance)
    size_t bufferSize = frameWidth_ * frameHeight_;
    std::vector<uint8_t> yData(bufferSize);
    
    glBindTexture(GL_TEXTURE_2D, textureY_);
    
    // Use glGetTexImage to read back the texture
    // Note: For NV12 textures imported via EGL, the internal format is R8
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, yData.data());
    
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        LOG_WARNING << "VaapiInterop: glGetTexImage failed with error 0x" << std::hex << error << std::dec;
        glBindTexture(GL_TEXTURE_2D, 0);
        return;
    }
    
    glBindTexture(GL_TEXTURE_2D, 0);
    
    // Calculate average luminance and sample a few pixels for debugging
    uint64_t sum = 0;
    for (size_t i = 0; i < bufferSize; i++) {
        sum += yData[i];
    }
    double avgLuma = (double)sum / bufferSize;
    
    // Sample center pixels
    int centerX = frameWidth_ / 2;
    int centerY = frameHeight_ / 2;
    int centerIdx = centerY * frameWidth_ + centerX;
    
    // Sample corners
    int tl = 0;
    int tr = frameWidth_ - 1;
    int bl = (frameHeight_ - 1) * frameWidth_;
    int br = (frameHeight_ - 1) * frameWidth_ + frameWidth_ - 1;
    
    LOG_INFO << "VaapiInterop: [FRAME " << debugFrameCounter_ << "] TEXTURE READBACK:"
             << " avgLuma=" << std::fixed << std::setprecision(1) << avgLuma
             << " center=" << (int)yData[centerIdx]
             << " corners=[" << (int)yData[tl] << "," << (int)yData[tr] 
             << "," << (int)yData[bl] << "," << (int)yData[br] << "]"
             << " first8=[";
    
    // Log first 8 bytes for pattern detection
    std::ostringstream oss;
    for (int i = 0; i < 8 && i < (int)bufferSize; i++) {
        if (i > 0) oss << ",";
        oss << (int)yData[i];
    }
    LOG_INFO << oss.str() << "]";
}

void VaapiInterop::debugReadbackVaapiSurface(VASurfaceID surface, VADisplay vaDisplay, int width, int height) {
    // This is an EXPENSIVE operation - only enable for debugging!
    // Uses vaDeriveImage + vaMapBuffer to read VAAPI surface to CPU memory
    // This verifies that the VAAPI decoder is producing correct frame data
    
    if (surface == VA_INVALID_ID || vaDisplay == nullptr) {
        LOG_WARNING << "VaapiInterop: Cannot readback - invalid surface or display";
        return;
    }
    
    VAImage image;
    memset(&image, 0, sizeof(image));
    
    // Derive an image from the surface (this is a zero-copy operation on most drivers)
    VAStatus status = vaDeriveImage(vaDisplay, surface, &image);
    if (status != VA_STATUS_SUCCESS) {
        LOG_WARNING << "VaapiInterop: vaDeriveImage failed: " << status;
        return;
    }
    
    // Map the image buffer to CPU memory
    void* buffer = nullptr;
    status = vaMapBuffer(vaDisplay, image.buf, &buffer);
    if (status != VA_STATUS_SUCCESS) {
        LOG_WARNING << "VaapiInterop: vaMapBuffer failed: " << status;
        vaDestroyImage(vaDisplay, image.image_id);
        return;
    }
    
    // Calculate some statistics from the Y plane
    uint8_t* yData = (uint8_t*)buffer + image.offsets[0];
    int yPitch = image.pitches[0];
    
    // Calculate average luminance
    uint64_t sum = 0;
    int pixelCount = 0;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            sum += yData[y * yPitch + x];
            pixelCount++;
        }
    }
    double avgLuma = pixelCount > 0 ? (double)sum / pixelCount : 0.0;
    
    // Sample center and corners
    int centerX = width / 2;
    int centerY = height / 2;
    uint8_t centerLuma = yData[centerY * yPitch + centerX];
    uint8_t tlLuma = yData[0];
    uint8_t trLuma = yData[width - 1];
    uint8_t blLuma = yData[(height - 1) * yPitch];
    uint8_t brLuma = yData[(height - 1) * yPitch + width - 1];
    
    // Sample first 16 bytes for pattern detection
    std::ostringstream oss;
    oss << "VaapiInterop: [FRAME " << debugFrameCounter_ << "] VAAPI SURFACE READBACK:"
        << " format=0x" << std::hex << image.format.fourcc << std::dec
        << " avgLuma=" << std::fixed << std::setprecision(1) << avgLuma
        << " center=" << (int)centerLuma
        << " corners=[" << (int)tlLuma << "," << (int)trLuma << "," << (int)blLuma << "," << (int)brLuma << "]"
        << " first16=[";
    for (int i = 0; i < 16 && i < width; i++) {
        if (i > 0) oss << ",";
        oss << (int)yData[i];
    }
    oss << "]";
    LOG_INFO << oss.str();
    
    // Cleanup
    vaUnmapBuffer(vaDisplay, image.buf);
    vaDestroyImage(vaDisplay, image.image_id);
}

void VaapiInterop::releaseFrame() {
    // CRITICAL: Wait for GPU to finish rendering before releasing resources
    // The GPU might still be sampling from textures backed by these EGL images/DMA-BUFs
    // If we release them while rendering is in progress, we'll get frozen frames
    // when the VAAPI surface pool recycles surfaces
    EGLContext currentContext = eglGetCurrentContext();
    if (currentContext != EGL_NO_CONTEXT) {
        // We have a GL context - ensure all rendering is complete
        glFinish();
    }
    
    // Use eglGetCurrentDisplay() like mpv does
    EGLDisplay currentDisplay = eglGetCurrentDisplay();
    if (currentDisplay == EGL_NO_DISPLAY) {
        currentDisplay = eglDisplay_;  // Fallback to stored display
    }
    
    if (eglImageY_ != EGL_NO_IMAGE_KHR) {
        eglDestroyImageKHR_(currentDisplay, eglImageY_);
        eglImageY_ = EGL_NO_IMAGE_KHR;
    }
    if (eglImageUV_ != EGL_NO_IMAGE_KHR) {
        eglDestroyImageKHR_(currentDisplay, eglImageUV_);
        eglImageUV_ = EGL_NO_IMAGE_KHR;
    }
    
    // Close open DMA-BUF FDs if keepFDsOpen_ mode was used
    if (openYFd_ >= 0) {
        close(openYFd_);
        openYFd_ = -1;
    }
    if (openUVFd_ >= 0) {
        close(openUVFd_);
        openUVFd_ = -1;
    }
}

EGLImageKHR VaapiInterop::createEGLImageFromDmaBuf(
    int fd, int width, int height,
    uint32_t fourcc, uint32_t offset, uint32_t pitch, uint64_t modifier) {
    
    // Check if we need to specify modifier (DRM_FORMAT_MOD_INVALID means linear/default)
    // DRM_FORMAT_MOD_INVALID = 0x00ffffffffffffff
    bool hasModifier = (modifier != 0 && modifier != 0x00ffffffffffffffULL);
    
    if (hasModifier) {
        // With modifier - use EGL_DMA_BUF_PLANE0_MODIFIER_LO/HI_EXT
        EGLint attribs[] = {
            EGL_WIDTH, width,
            EGL_HEIGHT, height,
            EGL_LINUX_DRM_FOURCC_EXT, (EGLint)fourcc,
            EGL_DMA_BUF_PLANE0_FD_EXT, fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)pitch,
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(modifier & 0xffffffff),
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(modifier >> 32),
            EGL_NONE
        };
        
        // Use eglGetCurrentDisplay() like mpv does - must match current GL context
        EGLDisplay currentDisplay = eglGetCurrentDisplay();
        if (currentDisplay == EGL_NO_DISPLAY) {
            LOG_ERROR << "VaapiInterop: No current EGL display";
            return EGL_NO_IMAGE_KHR;
        }
        EGLImageKHR image = eglCreateImageKHR_(
            currentDisplay, EGL_NO_CONTEXT,
            EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
        
        if (image == EGL_NO_IMAGE_KHR) {
            // Fall through to try without modifier
        } else {
            return image;
        }
    }
    
    // Without modifier (or fallback)
    EGLint attribs[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)fourcc,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)pitch,
        EGL_NONE
    };
    
    // Use eglGetCurrentDisplay() like mpv does
    EGLDisplay currentDisplay = eglGetCurrentDisplay();
    if (currentDisplay == EGL_NO_DISPLAY) {
        currentDisplay = eglDisplay_;  // Fallback
    }
    
    EGLImageKHR image = eglCreateImageKHR_(
        currentDisplay, EGL_NO_CONTEXT,
        EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    
    if (image == EGL_NO_IMAGE_KHR) {
        EGLint error = eglGetError();
        LOG_ERROR << "VaapiInterop: eglCreateImageKHR failed (error=0x" << std::hex << error << std::dec << ")";
    }
    
    return image;
}

void VaapiInterop::releaseCurrentFrame() {
    // MPV-style immediate release of VAAPI surface (but keep textures/EGL images for now)
    // 
    // CRITICAL INSIGHT: We CAN'T delete textures here because OpenGLRenderer still has their IDs!
    // The renderer will use those texture IDs until the NEXT frame is imported.
    // 
    // But we CAN unref the AVFrame immediately - the EGL images hold dup'd DMA-BUF FDs,
    // so they keep the surface data alive even after the AVFrame is unreffed!
    // The surface itself might go back to the pool, but the DMABUF memory stays valid
    // until we destroy the EGL images (which happens on next import)
    
    // Unref the AVFrame immediately - returns VAAPI surface to pool
    if (currentFrame_ && currentFrame_->buf[0]) {
        av_frame_unref(currentFrame_);
    }
    
    // Textures, EGL images, and DMA-BUF FDs stay alive until next import
    // (They're cleaned up in bindTexturesToImages() when creating new resources)
}

} // namespace videocomposer

#endif // HAVE_VAAPI_INTEROP

