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
 * OutputBlitShader.cpp - Canvas-to-output blit shader implementation
 */

#include "OutputBlitShader.h"
#include "OutputBlitGlsl.h"
#include "ColorMatrix.h"
#include "OutputLutTexture.h"
#include "../utils/Logger.h"

#include <GL/glew.h>
#include <GL/gl.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace videocomposer {

// Vertex shader - simple fullscreen quad (shared by programs A and B)
const char* OutputBlitShader::getVertexShaderSource() {
    return OutputBlitGlsl::kVertex;
}

// Fragment shader with edge blending and warp support (program A, the 0.1.2-11
// text verbatim; program B is assembled from it in OutputBlitGlsl.h)
const char* OutputBlitShader::getFragmentShaderSource() {
    return OutputBlitGlsl::fragmentA();
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
    
    // Color program (B). Its failure never fails init(): every output then
    // keeps program A, i.e. runs without color (the canvas layout is kept).
    compileColorProgram();
    
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
    
    if (colorProgram_ != 0) {
        glDeleteProgram(colorProgram_);
        colorProgram_ = 0;
    }
    colorProgramAvailable_ = false;
    
    if (colorFragmentShader_ != 0) {
        glDeleteShader(colorFragmentShader_);
        colorFragmentShader_ = 0;
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
    
    // Program B only for an output with color, while B is usable and its first
    // draw on this output did not fail; program A (below, unchanged) otherwise.
    if (region.hasColor() && colorProgramAvailable_ &&
        !(region.colorDraw && region.colorDraw->failed)) {
        blitColor(canvasTexture, canvasWidth, canvasHeight, region);
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

namespace {

// glGetError() returns one flag per call and flags are sticky until read: drop
// stale ones so a check only sees what the call it brackets raised. Bounded,
// because a lost context may keep returning an error.
void clearStaleGlErrors() {
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
    }
}

// Compile one shader, returning its info log instead of logging it, so the
// color program's failure is reported as a single ERROR line.
GLuint compileShaderWithLog(GLenum type, const char* source, std::string& log) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    
    GLint compileStatus = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compileStatus);
    if (compileStatus != GL_TRUE) {
        GLchar infoLog[1024];
        infoLog[0] = '\0';
        glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
        log = infoLog;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

void OutputBlitShader::compileColorProgram() {
    colorProgramAvailable_ = false;
    
    const std::string source = OutputBlitGlsl::assembleFragmentB();
    std::string reason;
#ifdef VC_TEST_FORCE_COLOR_PROGRAM_FAIL
    // Test builds only (-DVC_TEST_FORCE_COLOR_PROGRAM_FAIL=ON, never in the deb):
    // B "fails to link" so the rig can check the no-B path end to end -- every
    // output on A, applied=false and no output_color token on the contract.
    reason = "forced off by the VC_TEST_FORCE_COLOR_PROGRAM_FAIL test build";
#else
    if (source.empty()) {
        reason = "its text could not be assembled from program A (anchor not found exactly once)";
    } else {
        colorFragmentShader_ = compileShaderWithLog(GL_FRAGMENT_SHADER, source.c_str(), reason);
        if (colorFragmentShader_ == 0) {
            reason = "fragment shader compile failed: " + reason;
        } else {
            colorProgram_ = glCreateProgram();
            glAttachShader(colorProgram_, vertexShader_);
            glAttachShader(colorProgram_, colorFragmentShader_);
            glLinkProgram(colorProgram_);
            
            GLint linkStatus = GL_FALSE;
            glGetProgramiv(colorProgram_, GL_LINK_STATUS, &linkStatus);
            if (linkStatus != GL_TRUE) {
                GLchar infoLog[1024];
                infoLog[0] = '\0';
                glGetProgramInfoLog(colorProgram_, sizeof(infoLog), nullptr, infoLog);
                reason = std::string("link failed: ") + infoLog;
            }
        }
    }
#endif
    
    if (!reason.empty()) {
        LOG_ERROR << "OutputBlitShader: color program unavailable (" << reason
                  << ") — every output runs without color";
        if (colorProgram_ != 0) {
            glDeleteProgram(colorProgram_);
            colorProgram_ = 0;
        }
        if (colorFragmentShader_ != 0) {
            glDeleteShader(colorFragmentShader_);
            colorFragmentShader_ = 0;
        }
        return;
    }
    
    // B's own locations for every uniform it uses (A's are not valid in B)
    colorLoc_.canvasTex = glGetUniformLocation(colorProgram_, "uCanvasTex");
    colorLoc_.canvasSize = glGetUniformLocation(colorProgram_, "uCanvasSize");
    colorLoc_.sourceRect = glGetUniformLocation(colorProgram_, "uSourceRect");
    colorLoc_.outputSize = glGetUniformLocation(colorProgram_, "uOutputSize");
    colorLoc_.blendWidths = glGetUniformLocation(colorProgram_, "uBlendWidths");
    colorLoc_.blendGamma = glGetUniformLocation(colorProgram_, "uBlendGamma");
    colorLoc_.warpEnabled = glGetUniformLocation(colorProgram_, "uWarpEnabled");
    colorLoc_.warpTex = glGetUniformLocation(colorProgram_, "uWarpTex");
    colorLoc_.colorEnabled = glGetUniformLocation(colorProgram_, "uColorEnabled");
    colorLoc_.colorMatrix = glGetUniformLocation(colorProgram_, kUniformColorMatrix);
    colorLoc_.colorOffset = glGetUniformLocation(colorProgram_, kUniformColorOffset);
    colorLoc_.invGamma = glGetUniformLocation(colorProgram_, kUniformInvGamma);
    colorLoc_.lutEnabled = glGetUniformLocation(colorProgram_, "uLutEnabled");
    colorLoc_.lutTex = glGetUniformLocation(colorProgram_, "uLutTex");
    colorLoc_.lutScale = glGetUniformLocation(colorProgram_, "uLutScale");
    colorLoc_.lutOffset = glGetUniformLocation(colorProgram_, "uLutOffset");
    
    // Every uniform B uses must be live: a location of -1 makes glUniform* a silent
    // no-op, so a misnamed uColorMatrix would leave the matrix at zero and the output
    // black with no GL error for the draw check to see. Such a B is not used.
    const std::pair<const char*, GLint> required[] = {
        {"uCanvasTex", colorLoc_.canvasTex},     {"uCanvasSize", colorLoc_.canvasSize},
        {"uSourceRect", colorLoc_.sourceRect},   {"uOutputSize", colorLoc_.outputSize},
        {"uBlendWidths", colorLoc_.blendWidths}, {"uBlendGamma", colorLoc_.blendGamma},
        {"uWarpEnabled", colorLoc_.warpEnabled}, {"uWarpTex", colorLoc_.warpTex},
        {"uColorEnabled", colorLoc_.colorEnabled},
        {kUniformColorMatrix, colorLoc_.colorMatrix}, {kUniformColorOffset, colorLoc_.colorOffset},
        {kUniformInvGamma, colorLoc_.invGamma},
        {"uLutEnabled", colorLoc_.lutEnabled},   {"uLutTex", colorLoc_.lutTex},
        {"uLutScale", colorLoc_.lutScale},       {"uLutOffset", colorLoc_.lutOffset},
    };
    std::string missing;
    for (const auto& [name, location] : required) {
        if (location < 0) {
            missing += missing.empty() ? name : std::string(", ") + name;
        }
    }
    if (!missing.empty()) {
        LOG_ERROR << "OutputBlitShader: color program unavailable (uniform not found: " << missing
                  << ") — every output runs without color";
        glDeleteProgram(colorProgram_);
        colorProgram_ = 0;
        glDeleteShader(colorFragmentShader_);
        colorFragmentShader_ = 0;
        return;
    }
    
    colorProgramAvailable_ = true;
    LOG_INFO << "OutputBlitShader: Color program compiled and linked";
}

void OutputBlitShader::blitColor(GLuint canvasTexture,
                                 int canvasWidth, int canvasHeight,
                                 const OutputRegion& region) {
    // The LUT, or the placeholder (the sampler3D must always see a 3D texture).
    // Fetched first: creating the placeholder reads glGetError() itself.
    const OutputLutTexture* lut = region.lut.get();
    const GLuint lutTexture = lut ? lut->texture() : OutputLutTexture::placeholderTexture();
    
    // One-shot draw check per output: a program that links can still fail every
    // draw (e.g. a sampler3D and a sampler2D on one unit), which would otherwise
    // be a silent black output. Stale errors are dropped first, so the check
    // covers this program's whole setup and draw and nothing before it.
    ColorDrawState* drawState = region.colorDraw.get();
    const bool checkDraw = drawState && !drawState->checked;
    if (checkDraw) {
        clearStaleGlErrors();
    }
    
    glUseProgram(colorProgram_);
    
    // Same uniforms as program A, through B's own locations
    glUniform2f(colorLoc_.canvasSize,
                static_cast<float>(canvasWidth),
                static_cast<float>(canvasHeight));
    
    glUniform4f(colorLoc_.sourceRect,
                static_cast<float>(region.canvasX),
                static_cast<float>(region.canvasY),
                static_cast<float>(region.canvasWidth),
                static_cast<float>(region.canvasHeight));
    
    glUniform2f(colorLoc_.outputSize,
                static_cast<float>(region.physicalWidth),
                static_cast<float>(region.physicalHeight));
    
    glUniform4f(colorLoc_.blendWidths,
                region.blend.left,
                region.blend.right,
                region.blend.top,
                region.blend.bottom);
    
    glUniform1f(colorLoc_.blendGamma, region.blend.gamma);
    
    bool hasWarp = region.hasWarping();
    glUniform1i(colorLoc_.warpEnabled, hasWarp ? 1 : 0);
    
    // Trims (the shader skips them entirely when off)
    const OutputColor& color = region.color;
    const bool trims = color.trimsActive();
    glUniform1i(colorLoc_.colorEnabled, trims ? 1 : 0);
    if (trims) {
        // Composed per draw, as for the layers: a handful of flops, no cache to invalidate
        const ColorMatrix cm = composeColorMatrix(color);
        glUniformMatrix3fv(colorLoc_.colorMatrix, 1, GL_FALSE, cm.m);
        glUniform3f(colorLoc_.colorOffset, cm.off[0], cm.off[1], cm.off[2]);
        glUniform1f(colorLoc_.invGamma, cm.invGamma);
    }
    
    // LUT: size and domain come from the texture object itself
    if (lut) {
        const LutTransform& t = lut->transform();
        glUniform1i(colorLoc_.lutEnabled, 1);
        glUniform3f(colorLoc_.lutScale, t.scale[0], t.scale[1], t.scale[2]);
        glUniform3f(colorLoc_.lutOffset, t.offset[0], t.offset[1], t.offset[2]);
    } else {
        glUniform1i(colorLoc_.lutEnabled, 0);
        glUniform3f(colorLoc_.lutScale, 1.0f, 1.0f, 1.0f);
        glUniform3f(colorLoc_.lutOffset, 0.0f, 0.0f, 0.0f);
    }
    
    // Bind canvas texture to unit 0. uLutTex is set to its own unit every time
    // B is bound: a sampler3D left on unit 0 next to uCanvasTex's sampler2D
    // makes every draw fail with GL_INVALID_OPERATION (and init() never binds
    // the program, so it cannot be set there).
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, canvasTexture);
    glUniform1i(colorLoc_.canvasTex, 0);
    glUniform1i(colorLoc_.lutTex, kLutUnit);
    
    // Bind warp texture to unit 1 if enabled
    if (hasWarp && region.warpMesh) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, region.warpMesh->getTexture());
        glUniform1i(colorLoc_.warpTex, 1);
    }
    
    // The LUT (or the placeholder) on its own unit
    glActiveTexture(GL_TEXTURE0 + kLutUnit);
    glBindTexture(GL_TEXTURE_3D, lutTexture);
    
    // Draw fullscreen quad
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    
    if (checkDraw) {
        const GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            char code[16];
            std::snprintf(code, sizeof(code), "0x%04X", static_cast<unsigned>(err));
            LOG_ERROR << "OutputColor: " << region.name << " color program draw failed (GL "
                      << code << ") — this output runs without color";
            drawState->failed = true;
        }
        drawState->checked = true;
    }
    
    // Cleanup. Unit 2 first: the final active unit must be 0, because the layer
    // renderer binds its textures without glActiveTexture.
    glActiveTexture(GL_TEXTURE0 + kLutUnit);
    glBindTexture(GL_TEXTURE_3D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
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

