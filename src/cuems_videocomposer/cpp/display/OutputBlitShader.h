/**
 * OutputBlitShader.h - Shader for blitting canvas regions to outputs
 * 
 * Part of the Virtual Canvas architecture for cuems-videocomposer.
 * 
 * Extracts a region from the virtual canvas and renders it to the
 * current framebuffer (output surface) with optional:
 * - Edge blending (soft edges for projector overlap)
 * - Geometric warping (for keystone/curved surface correction)
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
    
    // ===== Private Methods =====
    
    /**
     * Compile and link shaders
     */
    bool compileShaders();
    
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

