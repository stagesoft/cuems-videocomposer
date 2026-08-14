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

#ifndef VIDEOCOMPOSER_SHADERPROGRAM_H
#define VIDEOCOMPOSER_SHADERPROGRAM_H

#include <string>
#include <map>
#include <GL/glew.h>
#include <GL/gl.h>

namespace videocomposer {

/**
 * ShaderProgram - Manages OpenGL shader compilation, linking, and uniforms
 * 
 * Based on mpv's shader approach but simplified for our use case.
 */
class ShaderProgram {
public:
    ShaderProgram();
    ~ShaderProgram();

    // Compile and link shaders from source strings
    bool createFromSource(const std::string& vertexSource, 
                         const std::string& fragmentSource);
    
    // Use this shader program
    void use() const;
    
    // Unbind shader program
    void unbind() const;
    
    // Check if program is valid
    bool isValid() const { return programId_ != 0; }
    
    // Get program ID
    GLuint getProgramId() const { return programId_; }
    
    // Uniform setters
    void setUniform(const std::string& name, int value);
    void setUniform(const std::string& name, float value);
    void setUniform(const std::string& name, float x, float y);
    void setUniform(const std::string& name, float x, float y, float z);
    void setUniform(const std::string& name, float x, float y, float z, float w);
    void setUniformMatrix4fv(const std::string& name, const float* matrix);
    
    // Get uniform location (cached)
    GLint getUniformLocation(const std::string& name);
    
    // Get attribute location
    GLint getAttribLocation(const std::string& name) const;

private:
    GLuint programId_;
    GLuint vertexShaderId_;
    GLuint fragmentShaderId_;
    
    // Cached uniform locations
    std::map<std::string, GLint> uniformLocations_;
    
    // Helper methods
    GLuint compileShader(GLenum type, const std::string& source);
    bool linkProgram();
    void cleanup();
    std::string getShaderInfoLog(GLuint shaderId) const;
    std::string getProgramInfoLog(GLuint programId) const;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_SHADERPROGRAM_H

