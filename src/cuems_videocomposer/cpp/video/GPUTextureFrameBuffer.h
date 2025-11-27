#ifndef VIDEOCOMPOSER_GPUTEXTUREFRAMEBUFFER_H
#define VIDEOCOMPOSER_GPUTEXTUREFRAMEBUFFER_H

#include "FrameFormat.h"
#include <cstdint>
#include <cstddef>

// Forward declaration to avoid including OpenGL headers here
// OpenGL types will be defined when needed
typedef unsigned int GLuint;
typedef unsigned int GLenum;

namespace videocomposer {

/**
 * Texture plane type for multi-plane formats
 */
enum class TexturePlaneType {
    SINGLE,         // Single-plane RGB/RGBA
    YUV_NV12,       // 2 planes: Y (R8) + UV (RG8)
    YUV_420P,       // 3 planes: Y (R8) + U (R8) + V (R8)
    HAP_Q_ALPHA     // 2 planes: YCoCg DXT5 + Alpha RGTC1 (HAP Q Alpha)
};

/**
 * HAP variant types (for texture metadata)
 */
enum class HapVariant {
    NONE = 0,
    HAP,            // Standard HAP (DXT1)
    HAP_Q,          // HAP Q (DXT5 YCoCg)
    HAP_ALPHA,      // HAP Alpha (DXT5 RGBA)
    HAP_Q_ALPHA,    // HAP Q Alpha (DXT5 YCoCg + RGTC1 Alpha)
    HAP_R           // HAP R (BPTC/BC7 RGBA - best quality + alpha) - UNTESTED
};

/**
 * GPUTextureFrameBuffer - Frame buffer that stores frames as GPU textures
 * 
 * This class is used for:
 * - HAP codec frames (DXT1/DXT5 compressed textures)
 * - Hardware-decoded frames (already on GPU)
 * - Multi-plane YUV textures (NV12, YUV420P) for zero-copy hardware decoding
 * 
 * Key feature: Zero-copy - frames stay on GPU, no CPU→GPU transfer needed
 * 
 * Copy semantics: Copying creates a non-owning reference to the same texture.
 * Only the original owner (the instance that called allocate()) will delete
 * the texture when destroyed. Copies are "views" that don't own the texture.
 */
class GPUTextureFrameBuffer {
public:
    GPUTextureFrameBuffer();
    ~GPUTextureFrameBuffer();
    
    // Copy constructor - creates non-owning copy (view)
    GPUTextureFrameBuffer(const GPUTextureFrameBuffer& other);
    
    // Copy assignment - creates non-owning copy (view)
    GPUTextureFrameBuffer& operator=(const GPUTextureFrameBuffer& other);
    
    // Move constructor - transfers ownership
    GPUTextureFrameBuffer(GPUTextureFrameBuffer&& other) noexcept;
    
    // Move assignment - transfers ownership
    GPUTextureFrameBuffer& operator=(GPUTextureFrameBuffer&& other) noexcept;

    // Allocate GPU texture for given format and dimensions
    bool allocate(const FrameInfo& info, GLenum textureFormat, bool isHAP = false);
    
    // Release GPU texture
    void release();

    // Get OpenGL texture ID (single-plane or plane 0)
    GLuint getTextureId() const { return textureIds_[0]; }
    
    // Get texture ID for specific plane (for multi-plane formats)
    GLuint getTextureId(int plane) const;
    
    // Get number of texture planes
    int getNumPlanes() const { return numPlanes_; }
    
    // Get plane type
    TexturePlaneType getPlaneType() const { return planeType_; }

    // Get texture format (GL_COMPRESSED_RGB_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, etc.)
    GLenum getTextureFormat() const { return textureFormat_; }

    // Check if this is a HAP texture (DXT1/DXT5 compressed)
    bool isHAPTexture() const { return isHAP_; }
    
    // Get HAP variant
    HapVariant getHapVariant() const { return hapVariant_; }

    // Check if this is a GPU texture (vs CPU buffer)
    bool isGPUTexture() const { return textureIds_[0] != 0; }

    // Get frame info
    const FrameInfo& info() const { return info_; }

    // Check if buffer is valid
    bool isValid() const { return textureIds_[0] != 0; }

    // Upload compressed texture data (for HAP)
    // data: compressed texture data (DXT1/DXT5)
    // size: size of compressed data in bytes
    bool uploadCompressedData(const uint8_t* data, size_t size, int width, int height, GLenum format);
    
    // Allocate HAP Q Alpha dual-texture (YCoCg color + alpha)
    bool allocateHapQAlpha(const FrameInfo& info);
    
    // Upload HAP Q Alpha dual-texture data
    bool uploadHapQAlphaData(const uint8_t* colorData, size_t colorSize,
                             const uint8_t* alphaData, size_t alphaSize,
                             int width, int height);
    
    // Set HAP variant
    void setHapVariant(HapVariant variant) { hapVariant_ = variant; }

    // Upload uncompressed texture data (for hardware decoded frames)
    bool uploadUncompressedData(const uint8_t* data, size_t size, int width, int height, GLenum format, int stride = 0);
    
    // Allocate multi-plane YUV texture (for zero-copy VAAPI/CUDA)
    // NV12: 2 planes (Y plane R8, UV plane RG8)
    // YUV420P: 3 planes (Y, U, V all R8)
    bool allocateMultiPlane(const FrameInfo& info, TexturePlaneType planeType);
    
    // Upload multi-plane YUV data
    // For NV12: yData (full res), uvData (half res)
    // For YUV420P: yData (full res), uData (quarter res), vData (quarter res)
    bool uploadMultiPlaneData(const uint8_t* yData, const uint8_t* uData, const uint8_t* vData,
                             int yStride, int uStride, int vStride);
    
    // Set external NV12 textures (for VAAPI zero-copy)
    // This does NOT take ownership of the textures - caller must keep them alive
    // Used when VaapiInterop provides texture IDs directly from DMA-BUF import
    bool setExternalNV12Textures(GLuint texY, GLuint texUV, const FrameInfo& info);

private:
    static constexpr int MAX_PLANES = 3;  // Maximum 3 planes (YUV420P)
    
    GLuint textureIds_[MAX_PLANES];  // OpenGL texture IDs (one per plane)
    int numPlanes_;                  // Number of texture planes (1, 2, or 3)
    TexturePlaneType planeType_;     // Plane type (single, NV12, YUV420P)
    GLenum textureFormat_;           // OpenGL texture format
    FrameInfo info_;                 // Frame information
    bool isHAP_;                     // True if this is a HAP texture (DXT1/DXT5)
    bool ownsTexture_;               // True if this instance owns the texture (should delete on destruction)
    HapVariant hapVariant_;          // HAP variant (if isHAP_ is true)
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_GPUTEXTUREFRAMEBUFFER_H

