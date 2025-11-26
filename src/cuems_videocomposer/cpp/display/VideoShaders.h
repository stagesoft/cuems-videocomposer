#ifndef VIDEOCOMPOSER_VIDEOSHADERS_H
#define VIDEOCOMPOSER_VIDEOSHADERS_H

#include <string>

namespace videocomposer {

/**
 * Video shader source code (GLSL 330 core)
 * Based on mpv's video_shaders.c approach
 */
namespace VideoShaders {

// Vertex shader (shared by all video shaders)
const std::string VERTEX_SHADER = R"(
#version 330 core

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;

uniform mat4 uMVP;
uniform mat4 uHomography;      // 4x4 homography for corner warping
uniform bool uUseHomography;   // Enable homography transformation

out vec2 vTexCoord;

void main() {
    vec4 pos = vec4(aPos, 0.0, 1.0);
    
    if (uUseHomography) {
        pos = uHomography * pos;  // Apply warping first
    }
    
    gl_Position = uMVP * pos;  // Then apply positioning/scaling
    vTexCoord = aTexCoord;
}
)";

// Fragment shader for RGBA textures (CPU frames, HAP after decompression)
const std::string FRAGMENT_RGBA = R"(
#version 330 core

in vec2 vTexCoord;

uniform sampler2D uTexture;
uniform float uOpacity;

out vec4 FragColor;

void main() {
    vec4 color = texture(uTexture, vTexCoord);
    FragColor = vec4(color.rgb, color.a * uOpacity);
}
)";

// High-quality RGBA fragment shader with anisotropic filtering for extreme warps
const std::string FRAGMENT_RGBA_HQ = R"(
#version 330 core

in vec2 vTexCoord;

uniform sampler2D uTexture;
uniform float uOpacity;
uniform float uAnisotropy;  // 1.0 = normal, 2.0-16.0 = enhanced filtering

out vec4 FragColor;

void main() {
    // Gradient-based anisotropic filtering for better quality on warped regions
    vec2 dx = dFdx(vTexCoord) * uAnisotropy;
    vec2 dy = dFdy(vTexCoord) * uAnisotropy;
    vec4 color = textureGrad(uTexture, vTexCoord, dx, dy);
    
    FragColor = vec4(color.rgb, color.a * uOpacity);
}
)";

// Fragment shader for NV12 format (VAAPI, CUDA output)
// NV12: Y plane (full resolution) + interleaved UV plane (half resolution)
// Note: Uses shared VERTEX_SHADER which supports homography warping
const std::string FRAGMENT_NV12 = R"(
#version 330 core

in vec2 vTexCoord;

uniform sampler2D uTexY;   // Luma plane (R8)
uniform sampler2D uTexUV;  // Chroma plane (RG8)
uniform float uOpacity;

out vec4 FragColor;

// BT.709 YUV to RGB conversion matrix
// Based on mpv's color matrix handling
const mat3 yuv2rgb = mat3(
    1.0,      1.0,       1.0,
    0.0,     -0.18732,   1.8556,
    1.5748,  -0.46812,   0.0
);

void main() {
    float y = texture(uTexY, vTexCoord).r;
    vec2 uv = texture(uTexUV, vTexCoord).rg;
    
    // YUV values are in range [0, 1], adjust to standard range
    vec3 yuv = vec3(y - 0.0625, uv.r - 0.5, uv.g - 0.5);
    
    // Convert to RGB
    vec3 rgb = yuv2rgb * yuv;
    
    // Clamp to valid range
    rgb = clamp(rgb, 0.0, 1.0);
    
    FragColor = vec4(rgb, uOpacity);
}
)";

// Fragment shader for YUV420P format (fallback, 3 separate planes)
// Note: Uses shared VERTEX_SHADER which supports homography warping
const std::string FRAGMENT_YUV420P = R"(
#version 330 core

in vec2 vTexCoord;

uniform sampler2D uTexY;  // Luma plane (R8)
uniform sampler2D uTexU;  // U chroma plane (R8)
uniform sampler2D uTexV;  // V chroma plane (R8)
uniform float uOpacity;

out vec4 FragColor;

// BT.709 YUV to RGB conversion matrix
const mat3 yuv2rgb = mat3(
    1.0,      1.0,       1.0,
    0.0,     -0.18732,   1.8556,
    1.5748,  -0.46812,   0.0
);

void main() {
    float y = texture(uTexY, vTexCoord).r;
    float u = texture(uTexU, vTexCoord).r;
    float v = texture(uTexV, vTexCoord).r;
    
    // YUV values are in range [0, 1], adjust to standard range
    vec3 yuv = vec3(y - 0.0625, u - 0.5, v - 0.5);
    
    // Convert to RGB
    vec3 rgb = yuv2rgb * yuv;
    
    // Clamp to valid range
    rgb = clamp(rgb, 0.0, 1.0);
    
    FragColor = vec4(rgb, uOpacity);
}
)";

} // namespace VideoShaders

} // namespace videocomposer

#endif // VIDEOCOMPOSER_VIDEOSHADERS_H

