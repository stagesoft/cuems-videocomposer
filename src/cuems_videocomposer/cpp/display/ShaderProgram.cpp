#include "ShaderProgram.h"
#include "../utils/Logger.h"
#include <vector>
#include <GL/glew.h>

namespace videocomposer {

ShaderProgram::ShaderProgram()
    : programId_(0)
    , vertexShaderId_(0)
    , fragmentShaderId_(0)
{
}

ShaderProgram::~ShaderProgram() {
    cleanup();
}

void ShaderProgram::cleanup() {
    if (vertexShaderId_ != 0) {
        glDeleteShader(vertexShaderId_);
        vertexShaderId_ = 0;
    }
    if (fragmentShaderId_ != 0) {
        glDeleteShader(fragmentShaderId_);
        fragmentShaderId_ = 0;
    }
    if (programId_ != 0) {
        glDeleteProgram(programId_);
        programId_ = 0;
    }
    uniformLocations_.clear();
}

bool ShaderProgram::createFromSource(const std::string& vertexSource, 
                                     const std::string& fragmentSource) {
    cleanup();
    
    // Compile vertex shader
    vertexShaderId_ = compileShader(GL_VERTEX_SHADER, vertexSource);
    if (vertexShaderId_ == 0) {
        LOG_ERROR << "Failed to compile vertex shader";
        return false;
    }
    
    // Compile fragment shader
    fragmentShaderId_ = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (fragmentShaderId_ == 0) {
        LOG_ERROR << "Failed to compile fragment shader";
        cleanup();
        return false;
    }
    
    // Link program
    if (!linkProgram()) {
        LOG_ERROR << "Failed to link shader program";
        cleanup();
        return false;
    }
    
    LOG_INFO << "Shader program created successfully (ID: " << programId_ << ")";
    return true;
}

GLuint ShaderProgram::compileShader(GLenum type, const std::string& source) {
    GLuint shaderId = glCreateShader(type);
    if (shaderId == 0) {
        LOG_ERROR << "glCreateShader failed for type " << type;
        return 0;
    }
    
    const char* sourcePtr = source.c_str();
    glShaderSource(shaderId, 1, &sourcePtr, nullptr);
    glCompileShader(shaderId);
    
    // Check compilation status
    GLint compiled = 0;
    glGetShaderiv(shaderId, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        std::string log = getShaderInfoLog(shaderId);
        LOG_ERROR << "Shader compilation failed (" 
                 << (type == GL_VERTEX_SHADER ? "vertex" : "fragment") 
                 << "):\n" << log;
        glDeleteShader(shaderId);
        return 0;
    }
    
    return shaderId;
}

bool ShaderProgram::linkProgram() {
    programId_ = glCreateProgram();
    if (programId_ == 0) {
        LOG_ERROR << "glCreateProgram failed";
        return false;
    }
    
    glAttachShader(programId_, vertexShaderId_);
    glAttachShader(programId_, fragmentShaderId_);
    glLinkProgram(programId_);
    
    // Check link status
    GLint linked = 0;
    glGetProgramiv(programId_, GL_LINK_STATUS, &linked);
    if (!linked) {
        std::string log = getProgramInfoLog(programId_);
        LOG_ERROR << "Shader program linking failed:\n" << log;
        glDeleteProgram(programId_);
        programId_ = 0;
        return false;
    }
    
    return true;
}

std::string ShaderProgram::getShaderInfoLog(GLuint shaderId) const {
    GLint logLength = 0;
    glGetShaderiv(shaderId, GL_INFO_LOG_LENGTH, &logLength);
    
    if (logLength > 0) {
        std::vector<char> log(logLength);
        glGetShaderInfoLog(shaderId, logLength, nullptr, log.data());
        return std::string(log.data());
    }
    
    return "";
}

std::string ShaderProgram::getProgramInfoLog(GLuint programId) const {
    GLint logLength = 0;
    glGetProgramiv(programId, GL_INFO_LOG_LENGTH, &logLength);
    
    if (logLength > 0) {
        std::vector<char> log(logLength);
        glGetProgramInfoLog(programId, logLength, nullptr, log.data());
        return std::string(log.data());
    }
    
    return "";
}

void ShaderProgram::use() const {
    if (programId_ != 0) {
        glUseProgram(programId_);
    }
}

void ShaderProgram::unbind() const {
    glUseProgram(0);
}

GLint ShaderProgram::getUniformLocation(const std::string& name) {
    // Check cache first
    auto it = uniformLocations_.find(name);
    if (it != uniformLocations_.end()) {
        return it->second;
    }
    
    // Query OpenGL
    GLint location = glGetUniformLocation(programId_, name.c_str());
    if (location == -1) {
        LOG_WARNING << "Uniform '" << name << "' not found in shader program " << programId_;
    }
    
    // Cache the result (even if -1, to avoid repeated queries)
    uniformLocations_[name] = location;
    return location;
}

GLint ShaderProgram::getAttribLocation(const std::string& name) const {
    return glGetAttribLocation(programId_, name.c_str());
}

void ShaderProgram::setUniform(const std::string& name, int value) {
    GLint location = getUniformLocation(name);
    if (location != -1) {
        glUniform1i(location, value);
    }
}

void ShaderProgram::setUniform(const std::string& name, float value) {
    GLint location = getUniformLocation(name);
    if (location != -1) {
        glUniform1f(location, value);
    }
}

void ShaderProgram::setUniform(const std::string& name, float x, float y) {
    GLint location = getUniformLocation(name);
    if (location != -1) {
        glUniform2f(location, x, y);
    }
}

void ShaderProgram::setUniform(const std::string& name, float x, float y, float z) {
    GLint location = getUniformLocation(name);
    if (location != -1) {
        glUniform3f(location, x, y, z);
    }
}

void ShaderProgram::setUniform(const std::string& name, float x, float y, float z, float w) {
    GLint location = getUniformLocation(name);
    if (location != -1) {
        glUniform4f(location, x, y, z, w);
    }
}

void ShaderProgram::setUniformMatrix4fv(const std::string& name, const float* matrix) {
    GLint location = getUniformLocation(name);
    if (location != -1) {
        glUniformMatrix4fv(location, 1, GL_FALSE, matrix);
    }
}

} // namespace videocomposer

