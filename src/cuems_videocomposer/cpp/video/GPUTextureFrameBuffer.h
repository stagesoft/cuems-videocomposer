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
 * GPUTextureFrameBuffer - Frame buffer that stores frames as GPU textures
 * 
 * This class is used for:
 * - HAP codec frames (DXT1/DXT5 compressed textures)
 * - Hardware-decoded frames (already on GPU)
 * 
 * Key feature: Zero-copy - frames stay on GPU, no CPU→GPU transfer needed
 */
class GPUTextureFrameBuffer {
public:
    GPUTextureFrameBuffer();
    ~GPUTextureFrameBuffer();

    // Allocate GPU texture for given format and dimensions
    bool allocate(const FrameInfo& info, GLenum textureFormat, bool isHAP = false);
    
    // Release GPU texture
    void release();

    // Get OpenGL texture ID
    GLuint getTextureId() const { return textureId_; }

    // Get texture format (GL_COMPRESSED_RGB_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, etc.)
    GLenum getTextureFormat() const { return textureFormat_; }

    // Check if this is a HAP texture (DXT1/DXT5 compressed)
    bool isHAPTexture() const { return isHAP_; }

    // Check if this is a GPU texture (vs CPU buffer)
    bool isGPUTexture() const { return textureId_ != 0; }

    // Get frame info
    const FrameInfo& info() const { return info_; }

    // Check if buffer is valid
    bool isValid() const { return textureId_ != 0; }

    // Upload compressed texture data (for HAP)
    // data: compressed texture data (DXT1/DXT5)
    // size: size of compressed data in bytes
    bool uploadCompressedData(const uint8_t* data, size_t size, int width, int height, GLenum format);

    // Upload uncompressed texture data (for hardware decoded frames)
    bool uploadUncompressedData(const uint8_t* data, size_t size, int width, int height, GLenum format, int stride = 0);

private:
    GLuint textureId_;        // OpenGL texture ID
    GLenum textureFormat_;    // OpenGL texture format
    FrameInfo info_;          // Frame information
    bool isHAP_;              // True if this is a HAP texture (DXT1/DXT5)
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_GPUTEXTUREFRAMEBUFFER_H

