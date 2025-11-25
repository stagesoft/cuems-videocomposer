#include "GPUTextureFrameBuffer.h"
#include "../utils/Logger.h"
#include <cstring>
#include <sstream>
#include <iomanip>

// OpenGL includes
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

// HAP uses S3TC compressed textures
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

namespace videocomposer {

// Helper function to check OpenGL errors
static bool checkGLError(const char* operation) {
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        const char* errorStr = "Unknown";
        switch (error) {
            case GL_INVALID_ENUM: errorStr = "GL_INVALID_ENUM"; break;
            case GL_INVALID_VALUE: errorStr = "GL_INVALID_VALUE"; break;
            case GL_INVALID_OPERATION: errorStr = "GL_INVALID_OPERATION"; break;
            case GL_OUT_OF_MEMORY: errorStr = "GL_OUT_OF_MEMORY"; break;
            case GL_INVALID_FRAMEBUFFER_OPERATION: errorStr = "GL_INVALID_FRAMEBUFFER_OPERATION"; break;
        }
        std::ostringstream oss;
        oss << "OpenGL error in " << operation << ": " << errorStr << " (0x" << std::hex << error << ")";
        LOG_ERROR << oss.str();
        return false;
    }
    return true;
}

GPUTextureFrameBuffer::GPUTextureFrameBuffer()
    : textureId_(0)
    , textureFormat_(0)
    , isHAP_(false)
    , ownsTexture_(false)
{
}

GPUTextureFrameBuffer::~GPUTextureFrameBuffer() {
    // Only release if we own the texture
    if (ownsTexture_) {
        release();
    }
}

GPUTextureFrameBuffer::GPUTextureFrameBuffer(const GPUTextureFrameBuffer& other)
    : textureId_(other.textureId_)
    , textureFormat_(other.textureFormat_)
    , info_(other.info_)
    , isHAP_(other.isHAP_)
    , ownsTexture_(false)  // Copy does NOT own the texture
{
}

GPUTextureFrameBuffer& GPUTextureFrameBuffer::operator=(const GPUTextureFrameBuffer& other) {
    if (this != &other) {
        // Release our texture if we own it
        if (ownsTexture_) {
            release();
        }
        
        // Copy the other's data (but don't take ownership)
        textureId_ = other.textureId_;
        textureFormat_ = other.textureFormat_;
        info_ = other.info_;
        isHAP_ = other.isHAP_;
        ownsTexture_ = false;  // Copy does NOT own the texture
    }
    return *this;
}

GPUTextureFrameBuffer::GPUTextureFrameBuffer(GPUTextureFrameBuffer&& other) noexcept
    : textureId_(other.textureId_)
    , textureFormat_(other.textureFormat_)
    , info_(other.info_)
    , isHAP_(other.isHAP_)
    , ownsTexture_(other.ownsTexture_)  // Take ownership from other
{
    // Clear other so it doesn't delete the texture
    other.textureId_ = 0;
    other.textureFormat_ = 0;
    other.isHAP_ = false;
    other.ownsTexture_ = false;
}

GPUTextureFrameBuffer& GPUTextureFrameBuffer::operator=(GPUTextureFrameBuffer&& other) noexcept {
    if (this != &other) {
        // Release our texture if we own it
        if (ownsTexture_) {
            release();
        }
        
        // Take other's data and ownership
        textureId_ = other.textureId_;
        textureFormat_ = other.textureFormat_;
        info_ = other.info_;
        isHAP_ = other.isHAP_;
        ownsTexture_ = other.ownsTexture_;
        
        // Clear other so it doesn't delete the texture
        other.textureId_ = 0;
        other.textureFormat_ = 0;
        other.isHAP_ = false;
        other.ownsTexture_ = false;
    }
    return *this;
}

bool GPUTextureFrameBuffer::allocate(const FrameInfo& info, GLenum textureFormat, bool isHAP) {
    // Release old texture if we own it
    if (ownsTexture_) {
        release();
    }
    
    info_ = info;
    textureFormat_ = textureFormat;
    isHAP_ = isHAP;
    ownsTexture_ = true;  // We will own the new texture

    LOG_VERBOSE << "GPUTextureFrameBuffer: Allocating texture (width=" << info.width 
               << ", height=" << info.height << ", format=0x" << std::hex << textureFormat 
               << std::dec << ", isHAP=" << isHAP << ")";

    // Generate OpenGL texture
    // Check if OpenGL context is available
    GLenum error = glGetError(); // Clear any previous errors
    glGenTextures(1, &textureId_);
    error = glGetError();
    if (textureId_ == 0 || error != GL_NO_ERROR) {
        if (error == GL_NO_ERROR && textureId_ == 0) {
            LOG_WARNING << "GPUTextureFrameBuffer: glGenTextures returned 0 (no OpenGL context?)";
        } else {
            checkGLError("glGenTextures");
        }
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, textureId_);
    if (!checkGLError("glBindTexture")) {
        LOG_WARNING << "GPUTextureFrameBuffer: glBindTexture failed";
        glDeleteTextures(1, &textureId_);
        textureId_ = 0;
        return false;
    }
    
    // Set texture parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!checkGLError("glTexParameteri")) {
        LOG_WARNING << "GPUTextureFrameBuffer: glTexParameteri failed";
        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteTextures(1, &textureId_);
        textureId_ = 0;
        return false;
    }

    // Allocate texture storage (empty for now, will be filled by upload methods)
    if (isHAP) {
        // For HAP, we'll upload compressed data later
        // Just allocate the texture object
    } else {
        // For uncompressed, allocate empty texture
        glTexImage2D(GL_TEXTURE_2D, 0, textureFormat_, 
                     info.width, info.height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        if (!checkGLError("glTexImage2D")) {
            glBindTexture(GL_TEXTURE_2D, 0);
            glDeleteTextures(1, &textureId_);
            textureId_ = 0;
            return false;
        }
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    checkGLError("glBindTexture(unbind)");
    return true;
}

void GPUTextureFrameBuffer::release() {
    if (textureId_ != 0 && ownsTexture_) {
        glDeleteTextures(1, &textureId_);
    }
    textureId_ = 0;
    textureFormat_ = 0;
    isHAP_ = false;
    ownsTexture_ = false;
}

bool GPUTextureFrameBuffer::uploadCompressedData(const uint8_t* data, size_t size, int width, int height, GLenum format) {
    if (textureId_ == 0 || data == nullptr || size == 0) {
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, textureId_);
    if (!checkGLError("glBindTexture(upload)")) {
        return false;
    }
    
    // Upload compressed texture data (DXT1/DXT5 for HAP)
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, format,
                           width, height, 0,
                           static_cast<GLsizei>(size), data);
    if (!checkGLError("glCompressedTexImage2D")) {
        glBindTexture(GL_TEXTURE_2D, 0);
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    checkGLError("glBindTexture(unbind)");
    return true;
}

bool GPUTextureFrameBuffer::uploadUncompressedData(const uint8_t* data, size_t size, int width, int height, GLenum format, int stride) {
    if (textureId_ == 0 || data == nullptr || size == 0) {
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, textureId_);
    if (!checkGLError("glBindTexture(upload)")) {
        return false;
    }
    
    // If stride is provided and different from width, use pixel store
    if (stride > 0 && stride != width * 4) { // Assuming RGBA = 4 bytes per pixel
        glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
        if (!checkGLError("glPixelStorei")) {
            glBindTexture(GL_TEXTURE_2D, 0);
            return false;
        }
    }

    // Upload uncompressed texture data
    glTexImage2D(GL_TEXTURE_2D, 0, format,
                 width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, data);
    if (!checkGLError("glTexImage2D")) {
        // Reset pixel store before returning
        if (stride > 0 && stride != width * 4) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        return false;
    }

    // Reset pixel store
    if (stride > 0 && stride != width * 4) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        checkGLError("glPixelStorei(reset)");
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    checkGLError("glBindTexture(unbind)");
    return true;
}

} // namespace videocomposer

