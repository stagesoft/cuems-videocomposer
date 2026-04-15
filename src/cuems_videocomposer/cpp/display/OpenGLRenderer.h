/*
 * SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
 *
 * This file is part of cuems-videocomposer.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef VIDEOCOMPOSER_OPENGLRENDERER_H
#define VIDEOCOMPOSER_OPENGLRENDERER_H

#include "../video/FrameBuffer.h"
#include "../video/GPUTextureFrameBuffer.h"
#include "../layer/VideoLayer.h"
#include "ShaderProgram.h"
#include "MasterProperties.h"
#include <vector>
#include <map>
#include <cstdint>
#include <memory>

// Forward declaration for OpenGL types
typedef unsigned int GLuint;

namespace videocomposer {

// Forward declaration
struct OSDRenderItem;

/**
 * OpenGLRenderer - Handles OpenGL rendering for layers
 * 
 * Manages shared OpenGL context, layer compositing, blending, and transforms.
 * This class handles the actual OpenGL rendering operations.
 */
class OpenGLRenderer {
public:
    OpenGLRenderer();
    ~OpenGLRenderer();

    // Initialize OpenGL state
    bool init();

    // Cleanup OpenGL resources
    void cleanup();

    // Render a single layer
    bool renderLayer(const VideoLayer* layer);
    
    // Render a layer from GPU texture (for HAP and hardware-decoded frames)
    bool renderLayerFromGPU(const GPUTextureFrameBuffer& gpuFrame, const LayerProperties& properties, const FrameInfo& frameInfo);

    // Composite all layers (applies master transforms if active)
    void compositeLayers(const std::vector<const VideoLayer*>& layers);
    
    // Master properties access
    MasterProperties& masterProperties() { return masterProperties_; }
    const MasterProperties& masterProperties() const { return masterProperties_; }

    // Set viewport
    void setViewport(int x, int y, int width, int height);

    // Update texture when video source changes
    void updateTexture(int width, int height);

    // Set letterbox mode
    void setLetterbox(bool enabled) { letterbox_ = enabled; }
    bool getLetterbox() const { return letterbox_; }

    // Render OSD items
    void renderOSDItems(const std::vector<struct OSDRenderItem>& items);
    
    // Cleanup deferred texture deletions (call after swapBuffers)
    void cleanupDeferredTextures();

private:
    // OpenGL state
    unsigned int textureId_;        // For GL_TEXTURE_RECTANGLE_ARB (regular textures)
    int textureWidth_;
    int textureHeight_;
    int viewportWidth_;
    int viewportHeight_;
    bool letterbox_;
    bool initialized_;
    bool isCoreProfile_;  // True if OpenGL Core Profile (DRM/EGL mode)
    
    // Shader-based rendering (VBO/VAO)
    GLuint quadVAO_;                // Vertex Array Object for quad
    GLuint quadVBO_;                // Vertex Buffer Object for quad
    
    // Shader programs
    // All shaders include color correction with uniform branch (uColorCorrectionEnabled).
    // We always use the shader path for consistent rendering - no switching between
    // fixed-function and shader paths to avoid visual artifacts mid-playback.
    // Overhead is negligible (~0.02% GPU per layer when color correction disabled).
    std::unique_ptr<ShaderProgram> rgbaShader_;      // For CPU frames, HAP, HAP Alpha
    std::unique_ptr<ShaderProgram> rgbaShaderHQ_;    // High-quality variant for extreme warps
    std::unique_ptr<ShaderProgram> nv12Shader_;      // For VAAPI/CUDA NV12
    std::unique_ptr<ShaderProgram> yuv420pShader_;   // For YUV420P fallback
    std::unique_ptr<ShaderProgram> hapQShader_;      // For HAP Q (YCoCg→RGB)
    std::unique_ptr<ShaderProgram> hapQAlphaShader_; // For HAP Q Alpha (dual texture)
    std::unique_ptr<ShaderProgram> masterShader_;    // For master FBO post-processing
    bool useShaders_;               // Enable shader rendering (vs fixed-function)
    
    // Deferred texture deletion (textures to delete after swapBuffers)
    std::vector<GLuint> texturesToDelete_;
    
    // Cached textures per layer (layerId -> texture info)
    struct LayerTextureCache {
        GLuint textureId;
        int width;
        int height;
        // PBO double-buffering for async texture upload
        GLuint pbo[2];          // Double-buffered PBOs
        int pboIndex;           // Current PBO index (0 or 1)
        bool pboInitialized;    // PBOs have been created
    };
    std::map<int, LayerTextureCache> layerTextureCache_;
    
    // PBO helper methods
    bool initLayerPBOs(LayerTextureCache& cache, int width, int height);
    void cleanupLayerPBOs(LayerTextureCache& cache);
    
    // Master layer properties (for composite output)
    MasterProperties masterProperties_;
    
    // FBO for off-screen rendering (used when master transforms are active)
    GLuint masterFBO_;              // Framebuffer Object
    GLuint masterFBOTexture_;       // Texture attached to FBO
    int masterFBOWidth_;            // FBO texture width
    int masterFBOHeight_;           // FBO texture height
    bool masterFBOInitialized_;     // FBO is ready to use

    // Internal methods
    void setupOrthoProjection();
    void renderQuad(float x, float y, float width, float height);
    void renderQuadWithCrop(float x, float y, float width, float height,
                           float texX, float texY, float texWidth, float texHeight);
    void applyLayerTransform(const VideoLayer* layer);
    void applyLayerTransform(const VideoLayer* layer, float quad_x, float quad_y);
    void applyLayerTransformFromProps(const LayerProperties& props, float quad_x, float quad_y);
    void applyBlendMode(const VideoLayer* layer);
    void applyBlendModeFromProps(const LayerProperties& props);
    bool uploadFrameToTexture(const FrameBuffer& frame);
    bool bindGPUTexture(const GPUTextureFrameBuffer& gpuFrame);
    void calculateCropCoordinates(const VideoLayer* layer, float& texX, float& texY, 
                                  float& texWidth, float& texHeight);
    void calculateCropCoordinatesFromProps(const LayerProperties& props, const FrameInfo& frameInfo,
                                           float& texX, float& texY, float& texWidth, float& texHeight);
    
    // VBO/VAO helpers
    bool initQuadVBO();
    void cleanupQuadVBO();
    
    // Shader helpers
    bool initShaders();
    void cleanupShaders();
    void computeMVPMatrix(float* mvp, float x, float y, float width, float height,
                         const LayerProperties& props);
    void renderQuadWithShader(ShaderProgram* shader, float x, float y, 
                             float width, float height, const LayerProperties& props);
    
    // FBO helpers (for master layer rendering)
    bool initMasterFBO(int width, int height);
    void cleanupMasterFBO();
    void renderMasterQuadWithTransforms();
    
    // Color correction helpers
    void setColorCorrectionUniforms(ShaderProgram* shader, 
                                    const LayerProperties::ColorAdjustment& colorAdjust);
    void setMasterColorCorrectionUniforms(ShaderProgram* shader,
                                          const MasterProperties::ColorAdjustment& colorAdjust);
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_OPENGLRENDERER_H

