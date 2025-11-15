#include "OpenGLRenderer.h"
#include "../layer/VideoLayer.h"
#include "../osd/OSDRenderer.h"
#include <cstring>

extern "C" {
#ifdef __APPLE__
#include "OpenGL/glu.h"
#else
#include <GL/glu.h>
#endif
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
    
    // Upload texture (using glTexImage2D like original, not glTexSubImage2D)
    // Original xjadeo uploads texture every frame
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA,
                 textureWidth_, textureHeight_, 0,
                    format, type, frame.data());

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
    // Use pixel coordinates for GL_TEXTURE_RECTANGLE_ARB
    float texX_px = texX * textureWidth_;
    float texY_px = texY * textureHeight_;
    float texW_px = texWidth * textureWidth_;
    float texH_px = texHeight * textureHeight_;
    
    glBegin(GL_QUADS);
    glTexCoord2f(texX_px, texY_px + texH_px); glVertex2f(x, y);
    glTexCoord2f(texX_px + texW_px, texY_px + texH_px); glVertex2f(x + width, y);
    glTexCoord2f(texX_px + texW_px, texY_px); glVertex2f(x + width, y + height);
    glTexCoord2f(texX_px, texY_px); glVertex2f(x, y + height);
    glEnd();
}

void OpenGLRenderer::calculateCropCoordinates(const VideoLayer* layer, float& texX, float& texY, 
                                             float& texWidth, float& texHeight) {
    if (!layer) {
        return;
    }

    const auto& props = layer->properties();
    const FrameInfo& frameInfo = layer->getFrameInfo();
    
    if (frameInfo.width == 0 || frameInfo.height == 0) {
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

    // Get layer's frame buffer
    const FrameBuffer& frameBuffer = layer->getFrameBuffer();
    if (!frameBuffer.isValid()) {
        return false;
    }

    // Upload frame to texture
    if (!uploadFrameToTexture(frameBuffer)) {
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

    // Render quad (matches original xjadeo - always use renderQuad, crop is handled in texture coords)
        renderQuad(x, y, w, h);

    // Disable texture (matches original xjadeo)
    glDisable(GL_TEXTURE_2D);

    glPopMatrix();

    return true;
}

void OpenGLRenderer::compositeLayers(const std::vector<const VideoLayer*>& layers) {
    glClear(GL_COLOR_BUFFER_BIT);

    // Render layers in z-order (already sorted by LayerManager)
    for (const VideoLayer* layer : layers) {
        if (layer && layer->isReady()) {
            renderLayer(layer);
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

void OpenGLRenderer::renderOSDItems(const std::vector<OSDRenderItem>& items) {
    if (items.empty()) {
        return;
    }

    // Save current matrix state
    glPushMatrix();
    glLoadIdentity();

    // Enable blending for OSD
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Render each OSD item
    for (const auto& item : items) {
        if (item.textureId == 0) {
            continue;
        }

        glBindTexture(GL_TEXTURE_2D, item.textureId);

        // Calculate normalized coordinates
        // OpenGL coordinates: -1 to 1, with origin at center
        // Screen coordinates: 0 to viewportWidth/Height, with origin at top-left
        float x = -1.0f + (2.0f * item.x / viewportWidth_);
        float y = 1.0f - (2.0f * item.y / viewportHeight_);
        float w = 2.0f * item.width / viewportWidth_;
        float h = -2.0f * item.height / viewportHeight_; // Negative because Y is flipped

        // Render textured quad
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(x, y);
        glTexCoord2f(1.0f, 0.0f); glVertex2f(x + w, y);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(x + w, y + h);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(x, y + h);
        glEnd();
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glPopMatrix();
}

} // namespace videocomposer

