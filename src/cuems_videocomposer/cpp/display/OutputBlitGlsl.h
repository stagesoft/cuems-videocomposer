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
 * OutputBlitGlsl.h - GLSL text of the canvas-to-output blit programs. GL-free.
 *
 * Program A is the blit as shipped up to 0.1.2-11: its fragment text is kept here
 * verbatim (moved out of OutputBlitShader.cpp, not one character changed) and every
 * output without per-output color draws with it.
 *
 * Program B is A's text plus two uniform-gated stages between the canvas sample and
 * the edge-blend ramp (programme phase 3, plan section 2.1): the five trims (the
 * shared GLSL of ColorCorrectionGlsl.h, the same text the layer shaders use), then a
 * 3D LUT sampled with an explicit LOD. Everything else of A - warp, ramp, output -
 * stays verbatim in B. B is assembled from A's text at run time, so the two can
 * never drift apart.
 *
 * GL-free on purpose: OutputBlitShader.cpp compiles these strings, the unit suite
 * pins them (A's checksum, B embeds the shared text), and tools/dump_blit_glsl.cpp
 * writes them out for glslangValidator at build time.
 */

#ifndef VIDEOCOMPOSER_OUTPUTBLITGLSL_H
#define VIDEOCOMPOSER_OUTPUTBLITGLSL_H

#include "ColorCorrectionGlsl.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace videocomposer {
namespace OutputBlitGlsl {

// Vertex shader - simple fullscreen quad (shared by A and B)
inline constexpr char kVertex[] = R"(
#version 330 core

layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;

out vec2 vTexCoord;

void main() {
    gl_Position = vec4(aPosition, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

// Program A: fragment shader with edge blending and warp support.
// Not one character of this text may change (OutputBlitShader_ProgramATextUnchanged).
inline constexpr char kFragmentA[] = R"(
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

/** Where B's declarations go: in front of this line of A. */
inline constexpr char kDeclarationAnchor[] = "in vec2 vTexCoord;";

/** Where B's color stages go: right after this line of A (the canvas sample). */
inline constexpr char kSampleAnchor[] = "    vec4 color = texture(uCanvasTex, texCoord);\n";

/** Start of A's edge-blend ramp; from here to the end of main() B is A, verbatim. */
inline constexpr char kRampMarker[] = "    // Calculate edge blending alpha";

/** B's uniforms, inserted in front of kDeclarationAnchor together with the shared trims. */
inline constexpr char kColorDeclarations[] = R"(// Per-output color (programme phase 3): trims, then a 3D LUT, before the ramp
uniform int uColorEnabled;       // 0 = trims off, 1 = on
uniform float uBrightness;       // -1.0 to 1.0 (0 = no change)
uniform float uContrast;         // 0.0 to 2.0 (1 = no change)
uniform float uSaturation;       // 0.0 to 2.0 (1 = no change)
uniform float uHue;              // -180 to 180 degrees
uniform float uGamma;            // 0.1 to 3.0 (1 = no change, > 1 brightens)
uniform int uLutEnabled;         // 0 = no LUT, 1 = sample uLutTex
uniform sampler3D uLutTex;       // always unit 2 (the output's LUT or a 1x1x1 placeholder)
uniform vec3 uLutScale;          // texel-centre transform of the LUT domain (LutTransform)
uniform vec3 uLutOffset;
)";

/** B's two stages, inserted right after kSampleAnchor. */
inline constexpr char kColorStages[] = R"(    
    // Per-output color: the five trims, then the 3D LUT (explicit LOD 0: one level)
    if (uColorEnabled != 0) {
        color.rgb = applyColorCorrection(color.rgb, uBrightness, uContrast,
                                         uSaturation, uHue, uGamma);
    }
    if (uLutEnabled != 0) {
        color.rgb = textureLod(uLutTex, color.rgb * uLutScale + uLutOffset, 0.0).rgb;
    }
)";

/** Number of (possibly overlapping) occurrences of `needle` in `haystack`. */
inline std::size_t countOccurrences(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return 0;
    std::size_t n = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string_view::npos;
         pos = haystack.find(needle, pos + 1)) {
        ++n;
    }
    return n;
}

/** Program A's fragment text (the 0.1.2-11 blit). */
inline const char* fragmentA() { return kFragmentA; }

/**
 * Program B's fragment text, assembled from A's. Returns an empty string when either
 * anchor is not found exactly once in A - the caller then reports B unavailable and
 * every output keeps running on A.
 */
inline std::string assembleFragmentB() {
    const std::string_view a(kFragmentA);
    if (countOccurrences(a, kDeclarationAnchor) != 1 || countOccurrences(a, kSampleAnchor) != 1) {
        return std::string();
    }
    const std::size_t decl = a.find(kDeclarationAnchor);
    const std::size_t stages = a.find(kSampleAnchor) + std::string_view(kSampleAnchor).size();
    if (stages <= decl) {
        return std::string();
    }

    std::string b;
    b.reserve(a.size() + sizeof(kColorDeclarations) + sizeof(kColorCorrectionGlsl) +
              sizeof(kColorStages));
    b.append(a.substr(0, decl));
    b.append(kColorDeclarations);
    b.append(kColorCorrectionGlsl);
    b.append("\n");
    b.append(a.substr(decl, stages - decl));
    b.append(kColorStages);
    b.append(a.substr(stages));
    return b;
}

} // namespace OutputBlitGlsl
} // namespace videocomposer

#endif // VIDEOCOMPOSER_OUTPUTBLITGLSL_H
