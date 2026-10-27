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

#ifndef VIDEOCOMPOSER_COLORMATRIX_H
#define VIDEOCOMPOSER_COLORMATRIX_H

#include "OutputColor.h"

namespace videocomposer {

/**
 * The five color trims as the shaders get them (programme phase 3, rev 4).
 *
 * Brightness, contrast, saturation and hue are affine maps with no clamp between them,
 * so their composition is exactly M * c + off. It is computed here, on the CPU, once per
 * draw, instead of per pixel in the shader; gamma and the final clamp stay per pixel
 * (applyColorMatrix in ColorCorrectionGlsl.h). Used by the layer and master shaders and
 * by the per-output blit program B.
 *
 * GL-free: the tests build it without a context.
 */

// Uniform names, shared by the GLSL declarations (ColorCorrectionGlsl.h) and every
// C++ lookup, so the two can never disagree (a lookup that misses is location -1).
inline constexpr char kUniformColorMatrix[] = "uColorMatrix";
inline constexpr char kUniformColorOffset[] = "uColorOffset";
inline constexpr char kUniformInvGamma[] = "uInvGamma";

struct ColorMatrix {
    float m[9];       ///< column-major, as glUniformMatrix3fv(..., GL_FALSE, m) expects
    float off[3];
    float invGamma;   ///< 1 / gamma; exactly 1 when gamma is 1, and the shader then skips pow
};

/**
 * M and off such that applyLinearTrims(c) = M * c + off: off is the chain at the origin,
 * column j of M is the chain at the unit vector e_j minus off.
 */
inline ColorMatrix composeColorMatrix(float brightness, float contrast, float saturation,
                                      float hue, float gamma) {
    ColorMatrix cm{};
    float o[3] = {0.0f, 0.0f, 0.0f};
    applyLinearTrims(brightness, contrast, saturation, hue, o[0], o[1], o[2]);
    for (int j = 0; j < 3; ++j) {
        float e[3] = {0.0f, 0.0f, 0.0f};
        e[j] = 1.0f;
        applyLinearTrims(brightness, contrast, saturation, hue, e[0], e[1], e[2]);
        for (int i = 0; i < 3; ++i) {
            cm.m[j * 3 + i] = e[i] - o[i];
        }
    }
    for (int i = 0; i < 3; ++i) {
        cm.off[i] = o[i];
    }
    cm.invGamma = 1.0f / gamma;
    return cm;
}

inline ColorMatrix composeColorMatrix(const OutputColor& c) {
    return composeColorMatrix(c.brightness, c.contrast, c.saturation, c.hue, c.gamma);
}

/** What the shader computes from a ColorMatrix (the tests' model of applyColorMatrix). */
inline void applyColorMatrixCpu(const ColorMatrix& cm, float& r, float& g, float& b) {
    float v[3];
    for (int i = 0; i < 3; ++i) {
        v[i] = cm.m[0 * 3 + i] * r + cm.m[1 * 3 + i] * g + cm.m[2 * 3 + i] * b + cm.off[i];
    }
    if (cm.invGamma != 1.0f) {
        for (float& x : v) {
            x = std::pow(std::max(x, 0.0f), cm.invGamma);
        }
    }
    r = std::clamp(v[0], 0.0f, 1.0f);
    g = std::clamp(v[1], 0.0f, 1.0f);
    b = std::clamp(v[2], 0.0f, 1.0f);
}

} // namespace videocomposer

#endif // VIDEOCOMPOSER_COLORMATRIX_H
