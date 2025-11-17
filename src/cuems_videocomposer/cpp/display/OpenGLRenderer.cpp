#include "OpenGLRenderer.h"
#include "../layer/VideoLayer.h"
#include "../osd/OSDRenderer.h"
#include "../utils/Logger.h"
#include "../input/HAPVideoInput.h"
#include "../input/InputSource.h"
#include <cstring>
#include <iomanip>
#include <sstream>

// HAP texture format constants
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_TEXTURE_2D
#define GL_TEXTURE_2D 0x0DE1
#endif

extern "C" {
#include <GL/gl.h>
}

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_TEXTURE_RECTANGLE_ARB
#define GL_TEXTURE_RECTANGLE_ARB 0x84F5
#endif

namespace videocomposer {

OpenGLRenderer::OpenGLRenderer()
    : textureId_(0)
    , textureWidth_(0)
    , textureHeight_(0)
    , viewportWidth_(0)
    , viewportHeight_(0)
    , letterbox_(true)
    , initialized_(false)
{
}

OpenGLRenderer::~OpenGLRenderer() {
    cleanup();
}

bool OpenGLRenderer::init() {
    if (initialized_) {
        return true;
    }

    // Initialize OpenGL state (matches original xjadeo)
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_RECTANGLE_ARB);

    // Generate texture
    glGenTextures(1, &textureId_);
    if (textureId_ == 0) {
        return false;
    }

    initialized_ = true;
    return true;
}

void OpenGLRenderer::cleanup() {
    if (textureId_ != 0) {
        glDeleteTextures(1, &textureId_);
        textureId_ = 0;
    }
    initialized_ = false;
}

void OpenGLRenderer::setViewport(int x, int y, int width, int height) {
    viewportWidth_ = width;
    viewportHeight_ = height;
    glViewport(x, y, width, height);
    setupOrthoProjection();
}

void OpenGLRenderer::setupOrthoProjection() {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

bool OpenGLRenderer::uploadFrameToTexture(const FrameBuffer& frame) {
    if (!frame.isValid() || textureId_ == 0) {
        return false;
    }

    const FrameInfo& info = frame.info();

    // Update texture if size changed (matches original xjadeo)
    if (textureWidth_ != info.width || textureHeight_ != info.height) {
        textureWidth_ = info.width;
        textureHeight_ = info.height;

        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, textureId_);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);

        // Allocate texture storage (BGRA format, matches original xjadeo)
        glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA,
                     textureWidth_, textureHeight_, 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    }

    // Upload frame data (matches original xjadeo: BGRA format)
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, textureId_);
    
    // Use BGRA format (matches original xjadeo)
    GLenum format = GL_BGRA;
    GLenum type = GL_UNSIGNED_BYTE;
    
    // Performance optimization: Use glTexSubImage2D when texture size hasn't changed
    // This is faster than glTexImage2D because it doesn't reallocate texture storage
    // Only use glTexImage2D when texture size changes (handled above)
    glTexSubImage2D(GL_TEXTURE_RECTANGLE_ARB, 0,
                    0, 0,  // xoffset, yoffset
                    textureWidth_, textureHeight_,
                    format, type, frame.data());

    return true;
}

bool OpenGLRenderer::bindGPUTexture(const GPUTextureFrameBuffer& gpuFrame) {
    if (!gpuFrame.isValid()) {
        return false;
    }

    GLuint textureId = gpuFrame.getTextureId();
    if (textureId == 0) {
        return false;
    }

    // All GPU textures (HAP and hardware-decoded) use GL_TEXTURE_2D
    // GPUTextureFrameBuffer always allocates textures as GL_TEXTURE_2D
    GLenum target = GL_TEXTURE_2D;
    
    glEnable(target);
    glBindTexture(target, textureId);
    
    // Set texture parameters
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Update texture dimensions for rendering
    const FrameInfo& info = gpuFrame.info();
    textureWidth_ = info.width;
    textureHeight_ = info.height;
    
    return true;
}

void OpenGLRenderer::renderQuad(float x, float y, float width, float height) {
    // Use pixel coordinates for GL_TEXTURE_RECTANGLE_ARB (matches original xjadeo)
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, (GLfloat)textureHeight_); glVertex2f(x, y);
    glTexCoord2f((GLfloat)textureWidth_, (GLfloat)textureHeight_); glVertex2f(x + width, y);
    glTexCoord2f((GLfloat)textureWidth_, 0.0f); glVertex2f(x + width, y + height);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(x, y + height);
    glEnd();
}

void OpenGLRenderer::renderQuadWithCrop(float x, float y, float width, float height,
                                       float texX, float texY, float texWidth, float texHeight) {
    // This function is used for both GL_TEXTURE_RECTANGLE_ARB (pixel coords) and GL_TEXTURE_2D (normalized coords)
    // For GL_TEXTURE_2D (HAP), texX/texY/texWidth/texHeight are already normalized (0.0-1.0)
    // For GL_TEXTURE_RECTANGLE_ARB, we need to convert to pixel coordinates
    // We'll detect which one based on whether the texture is bound as GL_TEXTURE_2D or GL_TEXTURE_RECTANGLE_ARB
    // For now, assume normalized coordinates (GL_TEXTURE_2D) since this is called for HAP
    
    // Use normalized texture coordinates (0.0-1.0) for GL_TEXTURE_2D
    glBegin(GL_QUADS);
    glTexCoord2f(texX, texY + texHeight); glVertex2f(x, y);
    glTexCoord2f(texX + texWidth, texY + texHeight); glVertex2f(x + width, y);
    glTexCoord2f(texX + texWidth, texY); glVertex2f(x + width, y + height);
    glTexCoord2f(texX, texY); glVertex2f(x, y + height);
    glEnd();
}

void OpenGLRenderer::calculateCropCoordinates(const VideoLayer* layer, float& texX, float& texY, 
                                             float& texWidth, float& texHeight) {
    if (!layer) {
        return;
    }

    const auto& props = layer->properties();
    const FrameInfo& frameInfo = layer->getFrameInfo();
    
    calculateCropCoordinatesFromProps(props, frameInfo, texX, texY, texWidth, texHeight);
}

void OpenGLRenderer::calculateCropCoordinatesFromProps(const LayerProperties& props, const FrameInfo& frameInfo,
                                                       float& texX, float& texY, float& texWidth, float& texHeight) {
    if (frameInfo.width == 0 || frameInfo.height == 0) {
        texX = 0.0f;
        texY = 0.0f;
        texWidth = 1.0f;
        texHeight = 1.0f;
        return;
    }

    // Panorama mode: crop to 50% width with pan offset
    if (props.panoramaMode) {
        float cropWidth = frameInfo.width / 2.0f;
        float maxOffset = frameInfo.width - cropWidth;
        
        // Clamp pan offset
        int panOffset = props.panOffset;
        if (panOffset < 0) panOffset = 0;
        if (panOffset > static_cast<int>(maxOffset)) panOffset = static_cast<int>(maxOffset);
        
        // Calculate texture coordinates
        texX = static_cast<float>(panOffset) / frameInfo.width;
        texY = 0.0f;
        texWidth = cropWidth / frameInfo.width;
        texHeight = 1.0f;
    }
    // General crop
    else if (props.crop.enabled) {
        // Calculate crop rectangle in texture coordinates (0.0 to 1.0)
        texX = static_cast<float>(props.crop.x) / frameInfo.width;
        texY = static_cast<float>(props.crop.y) / frameInfo.height;
        texWidth = static_cast<float>(props.crop.width) / frameInfo.width;
        texHeight = static_cast<float>(props.crop.height) / frameInfo.height;
        
        // Clamp to valid range
        if (texX < 0.0f) texX = 0.0f;
        if (texY < 0.0f) texY = 0.0f;
        if (texX + texWidth > 1.0f) texWidth = 1.0f - texX;
        if (texY + texHeight > 1.0f) texHeight = 1.0f - texY;
    }
    else {
        // No crop - use full texture
        texX = 0.0f;
        texY = 0.0f;
        texWidth = 1.0f;
        texHeight = 1.0f;
    }
}

void OpenGLRenderer::applyLayerTransform(const VideoLayer* layer) {
    if (!layer) return;

    const auto& props = layer->properties();
    
    glPushMatrix();
    
    // Apply scale
    glScalef(props.scaleX, props.scaleY, 1.0f);
    
    // Apply rotation (around center)
    if (props.rotation != 0.0f) {
        glTranslatef(0.5f, 0.5f, 0.0f);
        glRotatef(props.rotation, 0.0f, 0.0f, 1.0f);
        glTranslatef(-0.5f, -0.5f, 0.0f);
    }
}

void OpenGLRenderer::applyBlendMode(const VideoLayer* layer) {
    if (!layer) return;

    const auto& props = layer->properties();
    
    switch (props.blendMode) {
        case LayerProperties::NORMAL:
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            break;
        case LayerProperties::MULTIPLY:
            glBlendFunc(GL_DST_COLOR, GL_ZERO);
            break;
        case LayerProperties::SCREEN:
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
            break;
        case LayerProperties::OVERLAY:
            // Simplified overlay blend
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            break;
    }
}

bool OpenGLRenderer::renderLayer(const VideoLayer* layer) {
    if (!layer || !layer->isReady()) {
        return false;
    }

    const auto& props = layer->properties();
    if (!props.visible) {
        return false;
    }

            // Check if frame is on GPU (hardware-decoded)
            FrameBuffer cpuBuffer;
            GPUTextureFrameBuffer gpuBuffer;
            bool isOnGPU = layer->getPreparedFrame(cpuBuffer, gpuBuffer);
            
            if (isOnGPU && gpuBuffer.isValid()) {
                // Frame is on GPU - use GPU rendering path
                const FrameInfo& frameInfo = layer->getFrameInfo();
                return renderLayerFromGPU(gpuBuffer, props, frameInfo);
            } else if (!isOnGPU && cpuBuffer.isValid()) {
                // Frame is on CPU - use standard upload
                // HAP frames are now uncompressed RGBA (matches mpv), not compressed DXT
                if (!uploadFrameToTexture(cpuBuffer)) {
                    return false;
                }
        
        // Apply layer transform
        applyLayerTransform(layer);

        // Apply blend mode
        applyBlendMode(layer);

        // Set opacity
        glColor4f(1.0f, 1.0f, 1.0f, props.opacity);

        // Calculate quad size with letterboxing (matches original xjadeo)
        // Original uses _gl_quad_x and _gl_quad_y for letterboxing
        float quad_x = 1.0f;
        float quad_y = 1.0f;
        
        // Get frame info for aspect ratio calculation
        const FrameInfo& frameInfo = layer->getFrameInfo();
        
        if (letterbox_) {
            // Calculate aspect ratios (matches original xjadeo logic)
            float asp_src = frameInfo.aspect > 0.0f ? frameInfo.aspect : (float)props.width / (float)props.height;
            float asp_dst = (float)viewportWidth_ / (float)viewportHeight_;
            
            if (asp_dst > asp_src) {
                // Destination is wider - letterbox left/right
                quad_x = asp_src / asp_dst;
                quad_y = 1.0f;
            } else {
                // Destination is taller - letterbox top/bottom
                quad_x = 1.0f;
                quad_y = asp_dst / asp_src;
            }
        }
        
        // Render centered quad (matches original xjadeo)
        float x = -quad_x;
        float y = -quad_y;
        float w = 2.0f * quad_x;
        float h = 2.0f * quad_y;

                // Calculate texture coordinates for cropping/panorama
                float texX = 0.0f, texY = 0.0f, texWidth = 1.0f, texHeight = 1.0f;
                calculateCropCoordinates(layer, texX, texY, texWidth, texHeight);

                // Regular texture uses GL_TEXTURE_RECTANGLE_ARB (already bound in uploadFrameToTexture)
                // HAP is now treated as regular RGBA, not compressed
                // Use pixel coordinates for GL_TEXTURE_RECTANGLE_ARB
                renderQuad(x, y, w, h);
                // Disable texture (matches original xjadeo)
                glDisable(GL_TEXTURE_2D);

                glPopMatrix();

                return true;
    } else {
        // No valid frame available
        return false;
    }
}

void OpenGLRenderer::compositeLayers(const std::vector<const VideoLayer*>& layers) {
    glClear(GL_COLOR_BUFFER_BIT);

    // Render layers in z-order (already sorted by LayerManager)
    for (const VideoLayer* layer : layers) {
        if (layer && layer->isReady()) {
            bool rendered = renderLayer(layer);
            // If no frame was rendered, clear will show black screen (expected)
            // This is normal when waiting for MTC or when no frames are available yet
        }
    }
}

void OpenGLRenderer::updateTexture(int width, int height) {
    textureWidth_ = width;
    textureHeight_ = height;
    
    if (textureId_ != 0) {
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, textureId_);
        glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA, 
                     width, height, 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    }
}

bool OpenGLRenderer::renderLayerFromGPU(const GPUTextureFrameBuffer& gpuFrame, const LayerProperties& properties, const FrameInfo& frameInfo) {
    if (!gpuFrame.isValid()) {
        return false;
    }

    if (!properties.visible) {
        return false;
    }

    // Bind GPU texture (HAP or hardware-decoded)
    if (!bindGPUTexture(gpuFrame)) {
        return false;
    }

    // Apply layer transform
    glPushMatrix();
    glScalef(properties.scaleX, properties.scaleY, 1.0f);
    
    // Apply rotation (around center)
    if (properties.rotation != 0.0f) {
        glTranslatef(0.5f, 0.5f, 0.0f);
        glRotatef(properties.rotation, 0.0f, 0.0f, 1.0f);
        glTranslatef(-0.5f, -0.5f, 0.0f);
    }

    // Apply blend mode
    switch (properties.blendMode) {
        case LayerProperties::NORMAL:
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            break;
        case LayerProperties::MULTIPLY:
            glBlendFunc(GL_DST_COLOR, GL_ZERO);
            break;
        case LayerProperties::SCREEN:
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
            break;
        case LayerProperties::OVERLAY:
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            break;
    }

    // Set opacity
    glColor4f(1.0f, 1.0f, 1.0f, properties.opacity);

    // Calculate quad size with letterboxing
    float quad_x = 1.0f;
    float quad_y = 1.0f;
    
    if (letterbox_) {
        float asp_src = frameInfo.aspect > 0.0f ? frameInfo.aspect : (float)properties.width / (float)properties.height;
        float asp_dst = (float)viewportWidth_ / (float)viewportHeight_;
        
        if (asp_dst > asp_src) {
            quad_x = asp_src / asp_dst;
            quad_y = 1.0f;
        } else {
            quad_x = 1.0f;
            quad_y = asp_dst / asp_src;
        }
    }
    
    // Render centered quad
    float x = -quad_x;
    float y = -quad_y;
    float w = 2.0f * quad_x;
    float h = 2.0f * quad_y;

    // Calculate texture coordinates for cropping/panorama
    float texX = 0.0f, texY = 0.0f, texWidth = 1.0f, texHeight = 1.0f;
    calculateCropCoordinatesFromProps(properties, frameInfo, texX, texY, texWidth, texHeight);

    // Render quad with texture coordinates
    // All GPU textures (HAP and hardware-decoded) use GL_TEXTURE_2D
    // GL_TEXTURE_2D uses normalized texture coordinates (0.0-1.0)
    GLenum target = GL_TEXTURE_2D;
    
    // Use normalized texture coordinates (0.0-1.0) for GL_TEXTURE_2D
    glBegin(GL_QUADS);
    glTexCoord2f(texX, texY + texHeight); glVertex2f(x, y);
    glTexCoord2f(texX + texWidth, texY + texHeight); glVertex2f(x + w, y);
    glTexCoord2f(texX + texWidth, texY); glVertex2f(x + w, y + h);
    glTexCoord2f(texX, texY); glVertex2f(x, y + h);
    glEnd();

    // Disable texture
    glDisable(target);
    glPopMatrix();

    return true;
}

void OpenGLRenderer::renderOSDItems(const std::vector<OSDRenderItem>& items) {
    if (items.empty()) {
        return;
    }

    // Save current OpenGL state before modifying for OSD rendering
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TEXTURE_BIT);
    
    // CRITICAL: Disable GL_TEXTURE_RECTANGLE_ARB (used by video layers) before rendering OSD
    // OSD uses GL_TEXTURE_2D, and both cannot be active simultaneously
    glDisable(GL_TEXTURE_RECTANGLE_ARB);
    
    // Save and set projection matrix for OSD (2D overlay in normalized coordinates)
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);

    // Save and set modelview matrix for OSD
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    // Disable depth test (we're rendering 2D overlay on top of everything)
    glDisable(GL_DEPTH_TEST);
    // Ensure OSD is rendered on top by using a high depth value if depth test were enabled
    // Since depth test is disabled, this is just for clarity

    // Enable blending for OSD
    glEnable(GL_BLEND);
    // Use standard alpha blending for correct rendering of textures with alpha
    // GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA works for non-premultiplied alpha
    // This ensures transparent pixels (A=0) are completely invisible
    // When A=0: src contribution = 0, dst contribution = 1.0 * dst (unchanged)
    // When A=255: src contribution = 1.0 * src, dst contribution = 0 (fully opaque)
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // Disable face culling
    glDisable(GL_CULL_FACE);
    
    // Enable texture 2D (OSD uses GL_TEXTURE_2D, not GL_TEXTURE_RECTANGLE_ARB)
    glEnable(GL_TEXTURE_2D);
    
    // Set color to white (texture provides color)
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    // Render each OSD item
    for (const auto& item : items) {
        if (item.textureId == 0) {
            LOG_WARNING << "OSD: Skipping item with textureId=0";
            continue;
        }

        glBindTexture(GL_TEXTURE_2D, item.textureId);
        
        // Set texture environment to MODULATE for proper alpha blending
        // Video layers use GL_DECAL which ignores alpha - we need MODULATE for transparency
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

        // Calculate normalized coordinates
        // OpenGL coordinates: -1 to 1, with origin at center, Y up (top=1.0, bottom=-1.0)
        // Screen coordinates: 0 to viewportWidth/Height, with origin at top-left, Y down (top=0, bottom=height)
        // item.x and item.y are in screen pixels from top-left (item.y is top of text)
        float x = -1.0f + (2.0f * item.x / viewportWidth_);
        // Convert screen Y (top=0) to OpenGL Y (top=1.0): y_gl = 1.0 - 2.0 * (y_screen / height)
        float y = 1.0f - (2.0f * item.y / viewportHeight_);
        float w = 2.0f * item.width / viewportWidth_;
        // Height is negative because we're going down from top
        float h = -2.0f * item.height / viewportHeight_;

        // Ensure texture is bound and enabled
        glEnable(GL_TEXTURE_2D);
        
        // Set color to white with full opacity for text
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

        // Render textured quad with proper texture coordinates
        // OpenGL texture coordinates: (0,0) = bottom-left, (1,1) = top-right
        // Our bitmap is stored with (0,0) at top-left, so we need to flip Y
        // Top-left vertex -> (0,0) texture (bitmap top-left), bottom-left -> (0,1) texture (bitmap bottom-left)
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(x, y);           // Top-left vertex -> bitmap top-left (0,0)
        glTexCoord2f(1.0f, 0.0f); glVertex2f(x + w, y);       // Top-right vertex -> bitmap top-right (1,0)
        glTexCoord2f(1.0f, 1.0f); glVertex2f(x + w, y + h);   // Bottom-right vertex -> bitmap bottom-right (1,1)
        glTexCoord2f(0.0f, 1.0f); glVertex2f(x, y + h);       // Bottom-left vertex -> bitmap bottom-left (0,1)
        glEnd();
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
    
    // Restore matrices (must restore in reverse order)
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    
    // Restore all OpenGL state (including GL_TEXTURE_RECTANGLE_ARB if it was enabled)
    glPopAttrib();
    
    // Re-enable GL_TEXTURE_RECTANGLE_ARB for video layers (they need it for next frame)
    glEnable(GL_TEXTURE_RECTANGLE_ARB);
}

} // namespace videocomposer

