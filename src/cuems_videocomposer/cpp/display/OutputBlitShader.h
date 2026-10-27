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

/**
 * OutputBlitShader.h - Shader for blitting canvas regions to outputs
 * 
 * Part of the Virtual Canvas architecture for cuems-videocomposer.
 * 
 * Extracts a region from the virtual canvas and renders it to the
 * current framebuffer (output surface) with optional:
 * - Edge blending (soft edges for projector overlap)
 * - Geometric warping (for keystone/curved surface correction)
 * - Per-output color: five trims and a 3D LUT before the blend ramp
 *   (programme phase 3). Drawn with a second program (B) only on outputs
 *   that have color; every other output keeps the original program (A).
 */

#ifndef VIDEOCOMPOSER_OUTPUTBLITSHADER_H
#define VIDEOCOMPOSER_OUTPUTBLITSHADER_H

#include "OutputRegion.h"
#include <GL/glew.h>

namespace videocomposer {

/**
 * OutputBlitShader - GPU shader for canvas-to-output blitting
 * 
 * Usage:
 *   blitShader.init();
 *   
 *   // For each output:
 *   outputSurface->makeCurrent();
 *   blitShader.blit(canvas->getTexture(), 
 *                   canvas->getWidth(), canvas->getHeight(),
 *                   outputRegion);
 *   outputSurface->swapBuffers();
 */
class OutputBlitShader {
public:
    OutputBlitShader();
    ~OutputBlitShader();
    
    // Disable copy
    OutputBlitShader(const OutputBlitShader&) = delete;
    OutputBlitShader& operator=(const OutputBlitShader&) = delete;
    
    /**
     * Initialize shader program and resources
     * @return true on success
     */
    bool init();
    
    /**
     * Cleanup resources
     */
    void cleanup();
    
    /**
     * Check if initialized
     */
    bool isInitialized() const { return initialized_; }
    
    /**
     * Check if the color program (B) compiled and linked. When false every
     * output is drawn with program A, i.e. without color.
     */
    bool colorProgramAvailable() const { return colorProgramAvailable_; }
    
    /**
     * Blit a region of the canvas to the current framebuffer
     * 
     * The current framebuffer should already be bound (typically the output surface).
     * This renders a full-screen quad with the canvas region mapped to it.
     * 
     * @param canvasTexture Source texture from VirtualCanvas
     * @param canvasWidth Canvas width in pixels
     * @param canvasHeight Canvas height in pixels
     * @param region Output region configuration (position, size, blend, warp)
     */
    void blit(GLuint canvasTexture,
              int canvasWidth, int canvasHeight,
              const OutputRegion& region);
    
    /**
     * Simple blit without blend/warp (for testing)
     * 
     * @param canvasTexture Source texture
     * @param canvasWidth Canvas width
     * @param canvasHeight Canvas height
     * @param srcX Source X in canvas
     * @param srcY Source Y in canvas
     * @param srcWidth Source width
     * @param srcHeight Source height
     * @param dstWidth Destination width (viewport)
     * @param dstHeight Destination height (viewport)
     */
    void blitSimple(GLuint canvasTexture,
                    int canvasWidth, int canvasHeight,
                    int srcX, int srcY, int srcWidth, int srcHeight,
                    int dstWidth, int dstHeight);
    
private:
    bool initialized_ = false;
    
    // Shader program
    GLuint program_ = 0;
    GLuint vertexShader_ = 0;
    GLuint fragmentShader_ = 0;
    
    // Vertex array and buffer for fullscreen quad
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    
    // Uniform locations
    GLint uCanvasTex_ = -1;
    GLint uCanvasSize_ = -1;
    GLint uSourceRect_ = -1;      // x, y, width, height in canvas pixels
    GLint uOutputSize_ = -1;      // output width, height in pixels
    GLint uBlendWidths_ = -1;     // left, right, top, bottom blend widths
    GLint uBlendGamma_ = -1;      // gamma for perceptual blending
    GLint uWarpEnabled_ = -1;     // 0 or 1
    GLint uWarpTex_ = -1;         // warp displacement texture
    
    // Color program (B): A's text plus trims and a 3D LUT (OutputBlitGlsl.h).
    // Uniform locations are per program, so B has its own set for every
    // uniform it uses; A's locations above are never used while B is bound.
    GLuint colorProgram_ = 0;
    GLuint colorFragmentShader_ = 0;
    bool colorProgramAvailable_ = false;
    
    struct ColorLocations {
        GLint canvasTex = -1;
        GLint canvasSize = -1;
        GLint sourceRect = -1;
        GLint outputSize = -1;
        GLint blendWidths = -1;
        GLint blendGamma = -1;
        GLint warpEnabled = -1;
        GLint warpTex = -1;
        GLint colorEnabled = -1;
        GLint colorMatrix = -1;   // the trims as M * rgb + off (ColorMatrix.h)
        GLint colorOffset = -1;
        GLint invGamma = -1;
        GLint lutEnabled = -1;
        GLint lutTex = -1;
        GLint lutScale = -1;
        GLint lutOffset = -1;
    } colorLoc_;
    
    // Texture unit of the LUT. Never 0 or 1: a sampler3D on the unit of a
    // sampler2D (uCanvasTex = 0, uWarpTex = 1) makes every draw fail.
    static constexpr GLint kLutUnit = 2;
    
    // ===== Private Methods =====
    
    /**
     * Compile and link shaders
     */
    bool compileShaders();
    
    /**
     * Compile and link the color program (B). On failure logs one ERROR and
     * leaves colorProgramAvailable_ false; never fails init().
     */
    void compileColorProgram();
    
    /**
     * blit() with the color program bound (outputs with color only)
     */
    void blitColor(GLuint canvasTexture,
                   int canvasWidth, int canvasHeight,
                   const OutputRegion& region);
    
    /**
     * Create fullscreen quad VAO
     */
    bool createQuadVAO();
    
    /**
     * Compile a shader
     */
    GLuint compileShader(GLenum type, const char* source);
    
    /**
     * Get vertex shader source
     */
    static const char* getVertexShaderSource();
    
    /**
     * Get fragment shader source
     */
    static const char* getFragmentShaderSource();
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_OUTPUTBLITSHADER_H

