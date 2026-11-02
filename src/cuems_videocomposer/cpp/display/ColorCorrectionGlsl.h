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
 * The color-trim GLSL shared by the per-layer shaders (VideoShaders.h), the master
 * shader and the per-output color blit program (OutputBlitGlsl.h), so all are built
 * from one text.
 *
 * Rev 4 of programme phase 3: the five trims arrive as a matrix composed on the CPU
 * (ColorMatrix.h), M * c + off for brightness, contrast, saturation and hue, then the
 * gamma pow and the clamp. Until rev 4 this text computed the four affine trims per
 * pixel (cos/sin of the hue and five function calls); the result is the same within
 * float rounding, at a fraction of the GPU cost (K1c, 2026-10-27).
 *
 * kColorMatrixUniformsGlsl declares the uniforms; their names are ColorMatrix.h's
 * kUniform* constants, and the unit suite checks that every shader declares each one
 * exactly once.
 *
 * Constexpr char arrays on purpose, never `extern` and never a std::string: they are
 * constant-initialised, so the layer shaders that VideoShaders.h concatenates at
 * static-initialisation time can never read them before they exist.
 */
inline constexpr char kColorMatrixUniformsGlsl[] = R"(// Color trims as a CPU-composed matrix (ColorMatrix.h)
uniform mat3 uColorMatrix;       // brightness, contrast, saturation, hue: rgb' = M * rgb + off
uniform vec3 uColorOffset;
uniform float uInvGamma;         // 1 / gamma; 1.0 = no gamma
)";

inline constexpr char kColorCorrectionGlsl[] = R"(
// Apply the color trims: the affine part as one matrix, then gamma, then the clamp
vec3 applyColorMatrix(vec3 c, mat3 m, vec3 off, float invGamma) {
    vec3 result = m * c + off;
    if (invGamma != 1.0) {
        result = pow(max(result, vec3(0.0)), vec3(invGamma));
    }
    return clamp(result, 0.0, 1.0);
}
)";

} // namespace videocomposer

#endif // VIDEOCOMPOSER_COLORCORRECTIONGLSL_H
