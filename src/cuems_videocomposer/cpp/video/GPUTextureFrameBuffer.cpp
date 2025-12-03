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

// HAP uses S3TC compressed textures (DXT1/DXT5) and BPTC (BC7) for HAP R
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM
#define GL_COMPRESSED_RGBA_BPTC_UNORM 0x8E8C
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
    : textureIds_{0, 0, 0}
    , numPlanes_(1)
    , planeType_(TexturePlaneType::SINGLE)
    , textureFormat_(0)
    , isHAP_(false)
    , ownsTexture_(false)
    , hapVariant_(HapVariant::NONE)
{
}

GPUTextureFrameBuffer::~GPUTextureFrameBuffer() {
    // Only release if we own the texture
    if (ownsTexture_) {
        release();
    }
}

GPUTextureFrameBuffer::GPUTextureFrameBuffer(const GPUTextureFrameBuffer& other)
    : textureIds_{other.textureIds_[0], other.textureIds_[1], other.textureIds_[2]}
    , numPlanes_(other.numPlanes_)
    , planeType_(other.planeType_)
    , textureFormat_(other.textureFormat_)
    , info_(other.info_)
    , isHAP_(other.isHAP_)
    , ownsTexture_(false)  // Copy does NOT own the texture
    , hapVariant_(other.hapVariant_)
{
}

GPUTextureFrameBuffer& GPUTextureFrameBuffer::operator=(const GPUTextureFrameBuffer& other) {
    if (this != &other) {
        // Release our texture if we own it
        if (ownsTexture_) {
            release();
        }
        
        // Copy the other's data (but don't take ownership)
        textureIds_[0] = other.textureIds_[0];
        textureIds_[1] = other.textureIds_[1];
        textureIds_[2] = other.textureIds_[2];
        numPlanes_ = other.numPlanes_;
        planeType_ = other.planeType_;
        textureFormat_ = other.textureFormat_;
        info_ = other.info_;
        isHAP_ = other.isHAP_;
        ownsTexture_ = false;  // Copy does NOT own the texture
        hapVariant_ = other.hapVariant_;
    }
    return *this;
}

GPUTextureFrameBuffer::GPUTextureFrameBuffer(GPUTextureFrameBuffer&& other) noexcept
    : textureIds_{other.textureIds_[0], other.textureIds_[1], other.textureIds_[2]}
    , numPlanes_(other.numPlanes_)
    , planeType_(other.planeType_)
    , textureFormat_(other.textureFormat_)
    , info_(other.info_)
    , isHAP_(other.isHAP_)
    , ownsTexture_(other.ownsTexture_)  // Take ownership from other
    , hapVariant_(other.hapVariant_)
{
    // Clear other so it doesn't delete the texture
    other.textureIds_[0] = 0;
    other.textureIds_[1] = 0;
    other.textureIds_[2] = 0;
    other.numPlanes_ = 1;
    other.planeType_ = TexturePlaneType::SINGLE;
    other.textureFormat_ = 0;
    other.isHAP_ = false;
    other.ownsTexture_ = false;
    other.hapVariant_ = HapVariant::NONE;
}

GPUTextureFrameBuffer& GPUTextureFrameBuffer::operator=(GPUTextureFrameBuffer&& other) noexcept {
    if (this != &other) {
        // Release our texture if we own it
        if (ownsTexture_) {
            release();
        }
        
        // Take other's data and ownership
        textureIds_[0] = other.textureIds_[0];
        textureIds_[1] = other.textureIds_[1];
        textureIds_[2] = other.textureIds_[2];
        numPlanes_ = other.numPlanes_;
        planeType_ = other.planeType_;
        textureFormat_ = other.textureFormat_;
        info_ = other.info_;
        isHAP_ = other.isHAP_;
        ownsTexture_ = other.ownsTexture_;
        hapVariant_ = other.hapVariant_;
        
        // Clear other so it doesn't delete the texture
        other.textureIds_[0] = 0;
        other.textureIds_[1] = 0;
        other.textureIds_[2] = 0;
        other.numPlanes_ = 1;
        other.planeType_ = TexturePlaneType::SINGLE;
        other.textureFormat_ = 0;
        other.isHAP_ = false;
        other.ownsTexture_ = false;
        other.hapVariant_ = HapVariant::NONE;
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

    // Single-plane allocation
    numPlanes_ = 1;
    planeType_ = TexturePlaneType::SINGLE;
    
    // Generate OpenGL texture
    // Clear ALL pending GL errors first (important for DRM/KMS)
    while (glGetError() != GL_NO_ERROR) {}
    GLenum error;
    glGenTextures(1, &textureIds_[0]);
    error = glGetError();
    if (textureIds_[0] == 0 || error != GL_NO_ERROR) {
        if (error == GL_NO_ERROR && textureIds_[0] == 0) {
            LOG_WARNING << "GPUTextureFrameBuffer: glGenTextures returned 0 (no OpenGL context?)";
        } else {
            checkGLError("glGenTextures");
        }
        return false;
    }

    glBindTexture(GL_TEXTURE_2D, textureIds_[0]);
    if (!checkGLError("glBindTexture")) {
        LOG_WARNING << "GPUTextureFrameBuffer: glBindTexture failed";
        glDeleteTextures(1, &textureIds_[0]);
        textureIds_[0] = 0;
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
        glDeleteTextures(1, &textureIds_[0]);
        textureIds_[0] = 0;
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
            glDeleteTextures(1, &textureIds_[0]);
            textureIds_[0] = 0;
            return false;
        }
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    checkGLError("glBindTexture(unbind)");
    return true;
}

void GPUTextureFrameBuffer::release() {
    if (ownsTexture_) {
        // Delete all allocated planes
        for (int i = 0; i < numPlanes_; i++) {
            if (textureIds_[i] != 0) {
                glDeleteTextures(1, &textureIds_[i]);
                textureIds_[i] = 0;
            }
        }
    }
    textureIds_[0] = 0;
    textureIds_[1] = 0;
    textureIds_[2] = 0;
    numPlanes_ = 1;
    planeType_ = TexturePlaneType::SINGLE;
    textureFormat_ = 0;
    isHAP_ = false;
    ownsTexture_ = false;
    hapVariant_ = HapVariant::NONE;
}

GLuint GPUTextureFrameBuffer::getTextureId(int plane) const {
    if (plane >= 0 && plane < numPlanes_) {
        return textureIds_[plane];
    }
    return 0;
}

bool GPUTextureFrameBuffer::uploadCompressedData(const uint8_t* data, size_t size, int width, int height, GLenum format) {
    if (textureIds_[0] == 0 || data == nullptr || size == 0) {
        return false;
    }

    // Clear any pending GL errors (similar fix as VAAPI interop)
    while (glGetError() != GL_NO_ERROR) {}

    glBindTexture(GL_TEXTURE_2D, textureIds_[0]);
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
    if (textureIds_[0] == 0 || data == nullptr || size == 0) {
        return false;
    }

    // Clear any pending GL errors
    while (glGetError() != GL_NO_ERROR) {}

    glBindTexture(GL_TEXTURE_2D, textureIds_[0]);
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

bool GPUTextureFrameBuffer::allocateMultiPlane(const FrameInfo& info, TexturePlaneType planeType) {
    // Release old texture if we own it
    if (ownsTexture_) {
        release();
    }
    
    info_ = info;
    planeType_ = planeType;
    isHAP_ = false;
    ownsTexture_ = true;
    
    // Determine number of planes
    switch (planeType) {
        case TexturePlaneType::SINGLE:
            numPlanes_ = 1;
            break;
        case TexturePlaneType::YUV_NV12:
            numPlanes_ = 2;
            break;
        case TexturePlaneType::YUV_420P:
            numPlanes_ = 3;
            break;
        default:
            LOG_ERROR << "GPUTextureFrameBuffer: Unknown plane type";
            return false;
    }
    
    LOG_VERBOSE << "GPUTextureFrameBuffer: Allocating multi-plane texture (width=" << info.width 
               << ", height=" << info.height << ", planes=" << numPlanes_ << ")";
    
    // Generate textures for all planes
    glGenTextures(numPlanes_, textureIds_);
    if (!checkGLError("glGenTextures(multi-plane)")) {
        return false;
    }
    
    // Setup each plane
    for (int i = 0; i < numPlanes_; i++) {
        if (textureIds_[i] == 0) {
            LOG_ERROR << "GPUTextureFrameBuffer: glGenTextures returned 0 for plane " << i;
            release();
            return false;
        }
        
        // Bind texture
        glBindTexture(GL_TEXTURE_2D, textureIds_[i]);
        if (!checkGLError("glBindTexture(multi-plane)")) {
            release();
            return false;
        }
        
        // Set texture parameters
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        
        // Determine plane dimensions and format
        int planeWidth = info.width;
        int planeHeight = info.height;
        GLenum internalFormat = GL_R8;
        GLenum format = GL_RED;
        
        if (planeType == TexturePlaneType::YUV_NV12) {
            if (i == 0) {
                // Y plane: full resolution, R8
                internalFormat = GL_R8;
                format = GL_RED;
            } else {
                // UV plane: half resolution, RG8
                planeWidth = info.width / 2;
                planeHeight = info.height / 2;
                internalFormat = GL_RG8;
                format = GL_RG;
            }
        } else if (planeType == TexturePlaneType::YUV_420P) {
            if (i == 0) {
                // Y plane: full resolution, R8
                internalFormat = GL_R8;
                format = GL_RED;
            } else {
                // U/V planes: quarter resolution, R8
                planeWidth = info.width / 2;
                planeHeight = info.height / 2;
                internalFormat = GL_R8;
                format = GL_RED;
            }
        }
        
        // Allocate texture storage
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, planeWidth, planeHeight, 
                    0, format, GL_UNSIGNED_BYTE, nullptr);
        if (!checkGLError("glTexImage2D(multi-plane)")) {
            release();
            return false;
        }
    }
    
    glBindTexture(GL_TEXTURE_2D, 0);
    
    LOG_VERBOSE << "GPUTextureFrameBuffer: Multi-plane texture allocated successfully";
    return true;
}

bool GPUTextureFrameBuffer::uploadMultiPlaneData(const uint8_t* yData, const uint8_t* uData, const uint8_t* vData,
                                                 int yStride, int uStride, int vStride) {
    if (textureIds_[0] == 0) {
        LOG_ERROR << "GPUTextureFrameBuffer: No texture allocated for multi-plane upload";
        return false;
    }
    
    if (planeType_ == TexturePlaneType::YUV_NV12) {
        // NV12: 2 planes (Y, UV)
        if (!yData || !uData) {
            LOG_ERROR << "GPUTextureFrameBuffer: Invalid data pointers for NV12";
            return false;
        }
        
        // Upload Y plane
        glBindTexture(GL_TEXTURE_2D, textureIds_[0]);
        if (yStride != info_.width) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, yStride);
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, info_.width, info_.height,
                       GL_RED, GL_UNSIGNED_BYTE, yData);
        if (yStride != info_.width) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }
        
        // Upload UV plane
        glBindTexture(GL_TEXTURE_2D, textureIds_[1]);
        int uvWidth = info_.width / 2;
        int uvHeight = info_.height / 2;
        if (uStride != uvWidth * 2) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, uStride / 2);
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvWidth, uvHeight,
                       GL_RG, GL_UNSIGNED_BYTE, uData);
        if (uStride != uvWidth * 2) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }
        
        glBindTexture(GL_TEXTURE_2D, 0);
        return checkGLError("uploadMultiPlaneData(NV12)");
        
    } else if (planeType_ == TexturePlaneType::YUV_420P) {
        // YUV420P: 3 planes (Y, U, V)
        if (!yData || !uData || !vData) {
            LOG_ERROR << "GPUTextureFrameBuffer: Invalid data pointers for YUV420P";
            return false;
        }
        
        int uvWidth = info_.width / 2;
        int uvHeight = info_.height / 2;
        
        // Upload Y plane
        glBindTexture(GL_TEXTURE_2D, textureIds_[0]);
        if (yStride != info_.width) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, yStride);
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, info_.width, info_.height,
                       GL_RED, GL_UNSIGNED_BYTE, yData);
        if (yStride != info_.width) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }
        
        // Upload U plane
        glBindTexture(GL_TEXTURE_2D, textureIds_[1]);
        if (uStride != uvWidth) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, uStride);
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvWidth, uvHeight,
                       GL_RED, GL_UNSIGNED_BYTE, uData);
        if (uStride != uvWidth) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }
        
        // Upload V plane
        glBindTexture(GL_TEXTURE_2D, textureIds_[2]);
        if (vStride != uvWidth) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, vStride);
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvWidth, uvHeight,
                       GL_RED, GL_UNSIGNED_BYTE, vData);
        if (vStride != uvWidth) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }
        
        glBindTexture(GL_TEXTURE_2D, 0);
        return checkGLError("uploadMultiPlaneData(YUV420P)");
    }
    
    LOG_ERROR << "GPUTextureFrameBuffer: Invalid plane type for multi-plane upload";
    return false;
}

bool GPUTextureFrameBuffer::setExternalNV12Textures(GLuint texY, GLuint texUV, const FrameInfo& info) {
    if (texY == 0 || texUV == 0) {
        LOG_ERROR << "GPUTextureFrameBuffer: Invalid external texture IDs";
        return false;
    }
    
    // Release any existing owned textures
    if (ownsTexture_) {
        release();
    }
    
    // Set up as non-owning reference to external textures
    textureIds_[0] = texY;
    textureIds_[1] = texUV;
    textureIds_[2] = 0;
    numPlanes_ = 2;
    planeType_ = TexturePlaneType::YUV_NV12;
    textureFormat_ = GL_RG;  // NV12 UV plane format
    info_ = info;
    isHAP_ = false;
    ownsTexture_ = false;  // Don't own these textures - VaapiInterop owns them
    hapVariant_ = HapVariant::NONE;
    
    return true;
}

bool GPUTextureFrameBuffer::allocateHapQAlpha(const FrameInfo& info) {
    // Release old texture if we own it
    if (ownsTexture_) {
        release();
    }
    
    info_ = info;
    planeType_ = TexturePlaneType::HAP_Q_ALPHA;
    isHAP_ = true;
    ownsTexture_ = true;
    hapVariant_ = HapVariant::HAP_Q_ALPHA;
    numPlanes_ = 2;
    
    LOG_VERBOSE << "GPUTextureFrameBuffer: Allocating HAP Q Alpha dual-texture (width=" << info.width 
               << ", height=" << info.height << ")";
    
    // Generate textures for both planes
    glGenTextures(2, textureIds_);
    if (!checkGLError("glGenTextures(HAP Q Alpha)")) {
        return false;
    }
    
    // Setup first texture (YCoCg DXT5)
    glBindTexture(GL_TEXTURE_2D, textureIds_[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!checkGLError("glTexParameteri(HAP Q Alpha color)")) {
        release();
        return false;
    }
    
    // Setup second texture (Alpha RGTC1)
    glBindTexture(GL_TEXTURE_2D, textureIds_[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!checkGLError("glTexParameteri(HAP Q Alpha alpha)")) {
        release();
        return false;
    }
    
    glBindTexture(GL_TEXTURE_2D, 0);
    
    LOG_VERBOSE << "GPUTextureFrameBuffer: HAP Q Alpha dual-texture allocated successfully";
    return true;
}

bool GPUTextureFrameBuffer::uploadHapQAlphaData(const uint8_t* colorData, size_t colorSize,
                                                const uint8_t* alphaData, size_t alphaSize,
                                                int width, int height) {
    if (textureIds_[0] == 0 || textureIds_[1] == 0) {
        LOG_ERROR << "GPUTextureFrameBuffer: No HAP Q Alpha textures allocated";
        return false;
    }
    
    if (!colorData || !alphaData || colorSize == 0 || alphaSize == 0) {
        LOG_ERROR << "GPUTextureFrameBuffer: Invalid HAP Q Alpha data";
        return false;
    }
    
    // Upload YCoCg color texture (DXT5)
    glBindTexture(GL_TEXTURE_2D, textureIds_[0]);
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,
                           width, height, 0,
                           static_cast<GLsizei>(colorSize), colorData);
    if (!checkGLError("glCompressedTexImage2D(HAP Q Alpha color)")) {
        glBindTexture(GL_TEXTURE_2D, 0);
        return false;
    }
    
    // Upload alpha texture (RGTC1/BC4)
    glBindTexture(GL_TEXTURE_2D, textureIds_[1]);
    // GL_COMPRESSED_RED_RGTC1 = 0x8DBB (same as HapTextureFormat_A_RGTC1)
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, 0x8DBB,
                           width, height, 0,
                           static_cast<GLsizei>(alphaSize), alphaData);
    if (!checkGLError("glCompressedTexImage2D(HAP Q Alpha alpha)")) {
        glBindTexture(GL_TEXTURE_2D, 0);
        return false;
    }
    
    glBindTexture(GL_TEXTURE_2D, 0);
    
    
    return true;
}

} // namespace videocomposer

