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

#ifndef VIDEOCOMPOSER_OUTPUTFIT_H
#define VIDEOCOMPOSER_OUTPUTFIT_H

// Placement of a layer on one output region of the virtual canvas
// (/videocomposer/layer/<id>/fit_output). Pure arithmetic, no GL: the unit
// tests check it against the canvas geometry of real rigs.
//
// Coordinates follow the renderer (OpenGLRenderer::computeMVPMatrix):
// position is in canvas pixels from the canvas centre, Y negated, and a
// layer at scale 1 is the media letterboxed against the whole canvas. The
// cue path computes the same thing in the engine
// (cuems-engine players/VideoPlayer.py: get_layer_placement /
// get_layer_scale); "native" mode reproduces it.

#include <algorithm>
#include <string>

namespace videocomposer {

enum class FitMode {
    Fill,    // as large as the region allows, aspect kept (letterboxed)
    Native,  // media pixels 1:1, shrunk only if larger than the region
};

inline const char* fitModeName(FitMode mode) {
    return mode == FitMode::Native ? "native" : "fill";
}

struct FitRegion {
    std::string name;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct FitPlacement {
    int x = 0;
    int y = 0;
    double scale = 1.0;
};

// mediaWidth/mediaHeight <= 0: dimensions unknown (no input yet). The layer
// is then centred on the region at the region-height scale (the engine's
// fallback), and placed again once the dimensions are known.
inline FitPlacement computeOutputFit(const FitRegion& region, int canvasWidth, int canvasHeight,
                                     int mediaWidth, int mediaHeight, FitMode mode) {
    FitPlacement p;
    p.x = (region.x + region.width / 2) - canvasWidth / 2;
    p.y = canvasHeight / 2 - (region.y + region.height / 2);
    if (canvasWidth <= 0 || canvasHeight <= 0) {
        return p;
    }
    if (mediaWidth <= 0 || mediaHeight <= 0) {
        p.scale = static_cast<double>(region.height) / canvasHeight;
        return p;
    }
    // Size of the layer at scale 1: the media letterboxed against the canvas.
    const double mediaAspect = static_cast<double>(mediaWidth) / mediaHeight;
    const double canvasAspect = static_cast<double>(canvasWidth) / canvasHeight;
    double baseWidth, baseHeight;
    if (canvasAspect > mediaAspect) {
        baseHeight = canvasHeight;
        baseWidth = canvasHeight * mediaAspect;
    } else {
        baseWidth = canvasWidth;
        baseHeight = canvasWidth / mediaAspect;
    }
    const double fill = std::min(region.width / baseWidth, region.height / baseHeight);
    p.scale = (mode == FitMode::Native) ? std::min(mediaHeight / baseHeight, fill) : fill;
    return p;
}

}  // namespace videocomposer

#endif  // VIDEOCOMPOSER_OUTPUTFIT_H
