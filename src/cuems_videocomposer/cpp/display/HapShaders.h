#ifndef VIDEOCOMPOSER_HAPSHADERS_H
#define VIDEOCOMPOSER_HAPSHADERS_H

namespace videocomposer {

// Vertex shader for HAP (compatible with standard renderer uniforms)
// Uses same uniform names and layout as VideoShaders.h for compatibility
static const char* HAP_VERTEX_SHADER = R"(
#version 330 core

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;

uniform mat4 uMVP;
uniform mat4 uHomography;
uniform bool uUseHomography;

out vec2 vTexCoord;

void main() {
    vec4 pos = vec4(aPos, 0.0, 1.0);
    if (uUseHomography) {
        pos = uHomography * pos;
    }
    gl_Position = uMVP * pos;
    vTexCoord = aTexCoord;
}
)";

// Fragment shader for HAP Q (YCoCg DXT5 to RGB conversion)
// Uses same uniform names as VideoShaders.h for compatibility
// Based on official Vidvox HAP shader implementation
static const char* HAP_Q_FRAGMENT_SHADER = R"(
#version 330 core

uniform sampler2D uTexture;
uniform float uOpacity;

in vec2 vTexCoord;
out vec4 FragColor;

void main() {
    // Sample YCoCg-DXT5 texture
    // Channel layout: R=Co, G=Cg, B=Scale, A=Y
    vec4 ycocg = texture(uTexture, vTexCoord);
    
    // YCoCg to RGB conversion (Vidvox HAP specification)
    // The scale factor is stored in the blue channel
    const float offset = 0.50196078431;  // 128/255
    float scale = (ycocg.b * 255.0 / 8.0) + 1.0;
    
    // Y luma is in the alpha channel
    float Y = ycocg.a;
    
    // Extract Co and Cg chrominance values (centered around 128/255)
    float Co = (ycocg.r - offset) / scale;
    float Cg = (ycocg.g - offset) / scale;
    
    // Convert YCoCg to RGB
    float R = Y + Co - Cg;
    float G = Y + Cg;
    float B = Y - Co - Cg;
    
    // Clamp to valid range and apply opacity
    FragColor = vec4(clamp(R, 0.0, 1.0), clamp(G, 0.0, 1.0), clamp(B, 0.0, 1.0), 1.0) * uOpacity;
}
)";

// Fragment shader for HAP Q Alpha (dual texture: YCoCg color + separate alpha)
// Uses same uniform names as VideoShaders.h for compatibility
// Based on official Vidvox HAP shader implementation
static const char* HAP_Q_ALPHA_FRAGMENT_SHADER = R"(
#version 330 core

uniform sampler2D uTexture;      // YCoCg DXT5 texture (texture unit 0)
uniform sampler2D uTextureAlpha; // Alpha RGTC1 texture (texture unit 1)
uniform float uOpacity;

in vec2 vTexCoord;
out vec4 FragColor;

void main() {
    // Sample YCoCg-DXT5 color texture
    // Channel layout: R=Co, G=Cg, B=Scale, A=Y
    vec4 ycocg = texture(uTexture, vTexCoord);
    
    // YCoCg to RGB conversion (Vidvox HAP specification)
    const float offset = 0.50196078431;  // 128/255
    float scale = (ycocg.b * 255.0 / 8.0) + 1.0;
    
    float Y = ycocg.a;
    float Co = (ycocg.r - offset) / scale;
    float Cg = (ycocg.g - offset) / scale;
    
    float R = Y + Co - Cg;
    float G = Y + Cg;
    float B = Y - Co - Cg;
    
    // Sample alpha from second texture (stored in R channel of RGTC1)
    vec4 alphaTex = texture(uTextureAlpha, vTexCoord);
    float alpha = alphaTex.r;
    
    // Clamp to valid range and apply opacity
    FragColor = vec4(clamp(R, 0.0, 1.0), clamp(G, 0.0, 1.0), clamp(B, 0.0, 1.0), alpha) * uOpacity;
}
)";

} // namespace videocomposer

#endif // VIDEOCOMPOSER_HAPSHADERS_H

