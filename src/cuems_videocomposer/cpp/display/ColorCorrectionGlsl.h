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

#ifndef VIDEOCOMPOSER_COLORCORRECTIONGLSL_H
#define VIDEOCOMPOSER_COLORCORRECTIONGLSL_H

namespace videocomposer {

/**
 * The five-trim color correction GLSL (applyBrightness ... applyColorCorrection),
 * shared by the per-layer shaders (VideoShaders::COLOR_CORRECTION_FUNCTIONS) and
 * the per-output color blit program (OutputBlitGlsl.h), so both are built from
 * one text. Moved here from VideoShaders.h byte for byte (programme phase 3, S2).
 *
 * A constexpr char array on purpose, never `extern` and never a std::string: it is
 * constant-initialised, so the layer shaders that VideoShaders.h concatenates at
 * static-initialisation time can never read it before it exists.
 *
 * Every byte here is part of the layer shaders' text: the unit suite pins its
 * checksum (VideoShaders_ColorFunctionsUnchanged).
 */
inline constexpr char kColorCorrectionGlsl[] = R"(
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

} // namespace videocomposer

#endif // VIDEOCOMPOSER_COLORCORRECTIONGLSL_H
