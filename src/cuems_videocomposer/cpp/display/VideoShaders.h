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

// Color correction functions (shared GLSL code)
//
// DESIGN DECISION: Always use shader path with uniform branching
// ---------------------------------------------------------------
// We always use the shader path for all layers (CPU and GPU frames) with uniform
// branching (uColorCorrectionEnabled) to skip color correction when disabled.
//
// Why this approach:
//   1. Consistent rendering - no visual artifacts from switching render paths mid-playback
//   2. Negligible overhead - uniform branch costs ~0.02% GPU per layer (1920x1080)
//   3. Simpler code - single render path to maintain
//
// On modern GPUs, uniform branches are very efficient (~1-2 clock cycles per fragment)
// because all threads in a warp/wavefront take the same path.
//
const std::string COLOR_CORRECTION_FUNCTIONS = R"(
// Apply brightness: -1.0 to 1.0 (0 = no change)
vec3 applyBrightness(vec3 color, float brightness) {
    return color + brightness;
}

// Apply contrast: 0.0 to 2.0 (1 = no change)
vec3 applyContrast(vec3 color, float contrast) {
    return (color - 0.5) * contrast + 0.5;
}

// Apply saturation: 0.0 to 2.0 (1 = no change, 0 = grayscale)
vec3 applySaturation(vec3 color, float saturation) {
    float luminance = dot(color, vec3(0.299, 0.587, 0.114));
    return mix(vec3(luminance), color, saturation);
}

// Apply hue rotation: -180 to 180 degrees
vec3 applyHue(vec3 color, float hueDegrees) {
    float hueRad = hueDegrees * 3.14159265359 / 180.0;
    float cosH = cos(hueRad);
    float sinH = sin(hueRad);
    
    // Hue rotation matrix
    mat3 hueMatrix = mat3(
        0.213 + cosH * 0.787 - sinH * 0.213,
        0.213 - cosH * 0.213 + sinH * 0.143,
        0.213 - cosH * 0.213 - sinH * 0.787,
        
        0.715 - cosH * 0.715 - sinH * 0.715,
        0.715 + cosH * 0.285 + sinH * 0.140,
        0.715 - cosH * 0.715 + sinH * 0.715,
        
        0.072 - cosH * 0.072 + sinH * 0.928,
        0.072 - cosH * 0.072 - sinH * 0.283,
        0.072 + cosH * 0.928 + sinH * 0.072
    );
    
    return hueMatrix * color;
}

// Apply gamma: 0.1 to 3.0 (1 = no change)
vec3 applyGamma(vec3 color, float gamma) {
    return pow(max(color, vec3(0.0)), vec3(1.0 / gamma));
}

// Apply all color corrections
vec3 applyColorCorrection(vec3 color, float brightness, float contrast, 
                          float saturation, float hue, float gamma) {
    vec3 result = color;
    result = applyBrightness(result, brightness);
    result = applyContrast(result, contrast);
    result = applySaturation(result, saturation);
    if (hue != 0.0) {
        result = applyHue(result, hue);
    }
    if (gamma != 1.0) {
        result = applyGamma(result, gamma);
    }
    return clamp(result, 0.0, 1.0);
}
)";

// Fragment shader for RGBA textures (CPU frames, HAP after decompression)
const std::string FRAGMENT_RGBA = R"(
#version 330 core

in vec2 vTexCoord;

uniform sampler2D uTexture;
uniform float uOpacity;

// Color correction uniforms
uniform float uBrightness;   // -1.0 to 1.0 (0 = no change)
uniform float uContrast;     // 0.0 to 2.0 (1 = no change)
uniform float uSaturation;   // 0.0 to 2.0 (1 = no change)
uniform float uHue;          // -180 to 180 degrees
uniform float uGamma;        // 0.1 to 3.0 (1 = no change)
uniform bool uColorCorrectionEnabled;

)" + COLOR_CORRECTION_FUNCTIONS + R"(

out vec4 FragColor;

void main() {
    vec4 color = texture(uTexture, vTexCoord);
    vec3 rgb = color.rgb;
    
    if (uColorCorrectionEnabled) {
        rgb = applyColorCorrection(rgb, uBrightness, uContrast, 
                                   uSaturation, uHue, uGamma);
    }
    
    FragColor = vec4(rgb, color.a * uOpacity);
}
)";

// High-quality RGBA fragment shader with anisotropic filtering for extreme warps
const std::string FRAGMENT_RGBA_HQ = R"(
#version 330 core

in vec2 vTexCoord;

uniform sampler2D uTexture;
uniform float uOpacity;
uniform float uAnisotropy;  // 1.0 = normal, 2.0-16.0 = enhanced filtering

// Color correction uniforms
uniform float uBrightness;
uniform float uContrast;
uniform float uSaturation;
uniform float uHue;
uniform float uGamma;
uniform bool uColorCorrectionEnabled;

)" + COLOR_CORRECTION_FUNCTIONS + R"(

out vec4 FragColor;

void main() {
    // Gradient-based anisotropic filtering for better quality on warped regions
    vec2 dx = dFdx(vTexCoord) * uAnisotropy;
    vec2 dy = dFdy(vTexCoord) * uAnisotropy;
    vec4 color = textureGrad(uTexture, vTexCoord, dx, dy);
    
    vec3 rgb = color.rgb;
    if (uColorCorrectionEnabled) {
        rgb = applyColorCorrection(rgb, uBrightness, uContrast, 
                                   uSaturation, uHue, uGamma);
    }
    
    FragColor = vec4(rgb, color.a * uOpacity);
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

// Color correction uniforms
uniform float uBrightness;
uniform float uContrast;
uniform float uSaturation;
uniform float uHue;
uniform float uGamma;
uniform bool uColorCorrectionEnabled;

)" + COLOR_CORRECTION_FUNCTIONS + R"(

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
    
    // Apply color correction
    if (uColorCorrectionEnabled) {
        rgb = applyColorCorrection(rgb, uBrightness, uContrast, 
                                   uSaturation, uHue, uGamma);
    }
    
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

// Color correction uniforms
uniform float uBrightness;
uniform float uContrast;
uniform float uSaturation;
uniform float uHue;
uniform float uGamma;
uniform bool uColorCorrectionEnabled;

)" + COLOR_CORRECTION_FUNCTIONS + R"(

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
    
    // Apply color correction
    if (uColorCorrectionEnabled) {
        rgb = applyColorCorrection(rgb, uBrightness, uContrast, 
                                   uSaturation, uHue, uGamma);
    }
    
    FragColor = vec4(rgb, uOpacity);
}
)";

// Master post-processing shader (for FBO composite with color correction)
// Simple vertex shader for fullscreen quad
const std::string MASTER_VERTEX_SHADER = R"(
#version 330 core

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;

out vec2 vTexCoord;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

// Master post-processing fragment shader
const std::string MASTER_FRAGMENT_SHADER = R"(
#version 330 core

in vec2 vTexCoord;

uniform sampler2D uTexture;
uniform float uOpacity;

// Color correction uniforms
uniform float uBrightness;
uniform float uContrast;
uniform float uSaturation;
uniform float uHue;
uniform float uGamma;
uniform bool uColorCorrectionEnabled;

)" + COLOR_CORRECTION_FUNCTIONS + R"(

out vec4 FragColor;

void main() {
    vec4 color = texture(uTexture, vTexCoord);
    vec3 rgb = color.rgb;
    
    if (uColorCorrectionEnabled) {
        rgb = applyColorCorrection(rgb, uBrightness, uContrast, 
                                   uSaturation, uHue, uGamma);
    }
    
    FragColor = vec4(rgb, color.a * uOpacity);
}
)";

} // namespace VideoShaders

} // namespace videocomposer

#endif // VIDEOCOMPOSER_VIDEOSHADERS_H

