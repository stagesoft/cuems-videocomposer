/**
 * OutputBlitShader.cpp - Canvas-to-output blit shader implementation
 */

#include "OutputBlitShader.h"
#include "../utils/Logger.h"

#include <GL/glew.h>
#include <GL/gl.h>
#include <cstring>

namespace videocomposer {

// Vertex shader - simple fullscreen quad
const char* OutputBlitShader::getVertexShaderSource() {
    return R"(
#version 330 core

layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;

out vec2 vTexCoord;

void main() {
    gl_Position = vec4(aPosition, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";
}

// Fragment shader with edge blending and warp support
const char* OutputBlitShader::getFragmentShaderSource() {
    return R"(
#version 330 core

uniform sampler2D uCanvasTex;
uniform vec2 uCanvasSize;        // Canvas dimensions in pixels
uniform vec4 uSourceRect;        // x, y, width, height in canvas pixels
uniform vec2 uOutputSize;        // Output dimensions in pixels
uniform vec4 uBlendWidths;       // left, right, top, bottom blend widths in pixels
uniform float uBlendGamma;       // Gamma for perceptual blending (typically 2.2)
uniform int uWarpEnabled;        // 0 = disabled, 1 = enabled
uniform sampler2D uWarpTex;      // Warp displacement texture

in vec2 vTexCoord;               // 0-1 across output
out vec4 fragColor;

void main() {
    vec2 outputPos = vTexCoord;
    
    // Apply warp displacement if enabled
    if (uWarpEnabled != 0) {
        // Warp texture stores displacement as RG values (0.5 = no displacement)
        vec2 warpOffset = texture(uWarpTex, vTexCoord).xy * 2.0 - 1.0;
        // Scale displacement (adjust 0.1 for warp strength)
        outputPos += warpOffset * 0.1;
        // Clamp to valid range
        outputPos = clamp(outputPos, vec2(0.0), vec2(1.0));
    }
    
    // Map output position to canvas coordinates
    vec2 canvasCoord = uSourceRect.xy + outputPos * uSourceRect.zw;
    
    // Normalize to texture coordinates (0-1)
    vec2 texCoord = canvasCoord / uCanvasSize;
    
    // Sample canvas texture
    vec4 color = texture(uCanvasTex, texCoord);
    
    // Calculate edge blending alpha
    float alpha = 1.0;
    vec2 pixelPos = vTexCoord * uOutputSize;
    
    // Left edge blend
    if (uBlendWidths.x > 0.0 && pixelPos.x < uBlendWidths.x) {
        alpha *= smoothstep(0.0, uBlendWidths.x, pixelPos.x);
    }
    
    // Right edge blend
    if (uBlendWidths.y > 0.0 && pixelPos.x > (uOutputSize.x - uBlendWidths.y)) {
        alpha *= smoothstep(0.0, uBlendWidths.y, uOutputSize.x - pixelPos.x);
    }
    
    // Top edge blend
    if (uBlendWidths.z > 0.0 && pixelPos.y < uBlendWidths.z) {
        alpha *= smoothstep(0.0, uBlendWidths.z, pixelPos.y);
    }
    
    // Bottom edge blend
    if (uBlendWidths.w > 0.0 && pixelPos.y > (uOutputSize.y - uBlendWidths.w)) {
        alpha *= smoothstep(0.0, uBlendWidths.w, uOutputSize.y - pixelPos.y);
    }
    
    // Apply gamma correction for perceptual blending
    // This ensures the overlap region appears correctly bright
    if (alpha < 1.0) {
        alpha = pow(alpha, uBlendGamma);
    }
    
    // Output with blending applied
    fragColor = vec4(color.rgb * alpha, 1.0);
}
)";
}

OutputBlitShader::OutputBlitShader() {
}

OutputBlitShader::~OutputBlitShader() {
    cleanup();
}

bool OutputBlitShader::init() {
    if (initialized_) {
        LOG_WARNING << "OutputBlitShader: Already initialized";
        return true;
    }
    
    // Compile shaders
    if (!compileShaders()) {
        LOG_ERROR << "OutputBlitShader: Failed to compile shaders";
        return false;
    }
    
    // Create quad VAO
    if (!createQuadVAO()) {
        LOG_ERROR << "OutputBlitShader: Failed to create quad VAO";
        cleanup();
        return false;
    }
    
    // Get uniform locations
    uCanvasTex_ = glGetUniformLocation(program_, "uCanvasTex");
    uCanvasSize_ = glGetUniformLocation(program_, "uCanvasSize");
    uSourceRect_ = glGetUniformLocation(program_, "uSourceRect");
    uOutputSize_ = glGetUniformLocation(program_, "uOutputSize");
    uBlendWidths_ = glGetUniformLocation(program_, "uBlendWidths");
    uBlendGamma_ = glGetUniformLocation(program_, "uBlendGamma");
    uWarpEnabled_ = glGetUniformLocation(program_, "uWarpEnabled");
    uWarpTex_ = glGetUniformLocation(program_, "uWarpTex");
    
    initialized_ = true;
    LOG_INFO << "OutputBlitShader: Initialized";
    
    return true;
}

void OutputBlitShader::cleanup() {
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    
    if (vbo_ != 0) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    
    if (vertexShader_ != 0) {
        glDeleteShader(vertexShader_);
        vertexShader_ = 0;
    }
    
    if (fragmentShader_ != 0) {
        glDeleteShader(fragmentShader_);
        fragmentShader_ = 0;
    }
    
    initialized_ = false;
}

void OutputBlitShader::blit(GLuint canvasTexture,
                            int canvasWidth, int canvasHeight,
                            const OutputRegion& region) {
    if (!initialized_) {
        LOG_ERROR << "OutputBlitShader: Not initialized";
        return;
    }
    
    // Use our shader program
    glUseProgram(program_);
    
    // Set uniforms
    glUniform2f(uCanvasSize_, 
                static_cast<float>(canvasWidth), 
                static_cast<float>(canvasHeight));
    
    glUniform4f(uSourceRect_,
                static_cast<float>(region.canvasX),
                static_cast<float>(region.canvasY),
                static_cast<float>(region.canvasWidth),
                static_cast<float>(region.canvasHeight));
    
    glUniform2f(uOutputSize_,
                static_cast<float>(region.physicalWidth),
                static_cast<float>(region.physicalHeight));
    
    // Blend configuration
    glUniform4f(uBlendWidths_,
                region.blend.left,
                region.blend.right,
                region.blend.top,
                region.blend.bottom);
    
    glUniform1f(uBlendGamma_, region.blend.gamma);
    
    // Warp configuration
    bool hasWarp = region.hasWarping();
    glUniform1i(uWarpEnabled_, hasWarp ? 1 : 0);
    
    // Bind canvas texture to unit 0
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, canvasTexture);
    glUniform1i(uCanvasTex_, 0);
    
    // Bind warp texture to unit 1 if enabled
    if (hasWarp && region.warpMesh) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, region.warpMesh->getTexture());
        glUniform1i(uWarpTex_, 1);
    }
    
    // Draw fullscreen quad
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    
    // Cleanup
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

void OutputBlitShader::blitSimple(GLuint canvasTexture,
                                   int canvasWidth, int canvasHeight,
                                   int srcX, int srcY, int srcWidth, int srcHeight,
                                   int dstWidth, int dstHeight) {
    // Create a temporary region for simple blit
    OutputRegion region;
    region.canvasX = srcX;
    region.canvasY = srcY;
    region.canvasWidth = srcWidth;
    region.canvasHeight = srcHeight;
    region.physicalWidth = dstWidth;
    region.physicalHeight = dstHeight;
    // No blending, no warping
    region.blend.reset();
    region.warpMesh = nullptr;
    
    blit(canvasTexture, canvasWidth, canvasHeight, region);
}

bool OutputBlitShader::compileShaders() {
    // Compile vertex shader
    vertexShader_ = compileShader(GL_VERTEX_SHADER, getVertexShaderSource());
    if (vertexShader_ == 0) {
        return false;
    }
    
    // Compile fragment shader
    fragmentShader_ = compileShader(GL_FRAGMENT_SHADER, getFragmentShaderSource());
    if (fragmentShader_ == 0) {
        return false;
    }
    
    // Create program
    program_ = glCreateProgram();
    glAttachShader(program_, vertexShader_);
    glAttachShader(program_, fragmentShader_);
    glLinkProgram(program_);
    
    // Check link status
    GLint linkStatus;
    glGetProgramiv(program_, GL_LINK_STATUS, &linkStatus);
    if (linkStatus != GL_TRUE) {
        GLchar infoLog[512];
        glGetProgramInfoLog(program_, sizeof(infoLog), nullptr, infoLog);
        LOG_ERROR << "OutputBlitShader: Program link failed: " << infoLog;
        return false;
    }
    
    LOG_INFO << "OutputBlitShader: Shaders compiled and linked";
    return true;
}

GLuint OutputBlitShader::compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    
    // Check compile status
    GLint compileStatus;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compileStatus);
    if (compileStatus != GL_TRUE) {
        GLchar infoLog[512];
        glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
        LOG_ERROR << "OutputBlitShader: Shader compile failed ("
                  << (type == GL_VERTEX_SHADER ? "vertex" : "fragment") 
                  << "): " << infoLog;
        glDeleteShader(shader);
        return 0;
    }
    
    return shader;
}

bool OutputBlitShader::createQuadVAO() {
    // Fullscreen quad vertices: position (x, y) + texcoord (u, v)
    // Triangle strip order: bottom-left, bottom-right, top-left, top-right
    float vertices[] = {
        // Position      // TexCoord
        -1.0f, -1.0f,    0.0f, 0.0f,  // Bottom-left
         1.0f, -1.0f,    1.0f, 0.0f,  // Bottom-right
        -1.0f,  1.0f,    0.0f, 1.0f,  // Top-left
         1.0f,  1.0f,    1.0f, 1.0f   // Top-right
    };
    
    // Create VAO
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    
    // Create VBO
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    
    // Position attribute (location 0)
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    // TexCoord attribute (location 1)
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    // Unbind
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    
    return true;
}

} // namespace videocomposer

