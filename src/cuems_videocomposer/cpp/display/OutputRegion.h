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
 * OutputRegion.h - Output region configuration for Virtual Canvas
 * 
 * Defines where an output sits in the virtual canvas and its
 * blend/warp configuration for projection mapping.
 */

#ifndef VIDEOCOMPOSER_OUTPUTREGION_H
#define VIDEOCOMPOSER_OUTPUTREGION_H

#include "OutputColor.h"

#include <string>
#include <memory>

namespace videocomposer {

/**
 * BlendEdges - Edge blending configuration for projector overlap
 * 
 * Specifies the width of the soft-edge blend zone on each edge.
 * Values are in pixels. 0 = no blending on that edge.
 */
struct BlendEdges {
    float left = 0.0f;      // Left edge blend width in pixels
    float right = 0.0f;     // Right edge blend width in pixels
    float top = 0.0f;       // Top edge blend width in pixels
    float bottom = 0.0f;    // Bottom edge blend width in pixels
    float gamma = 2.2f;     // Gamma correction for perceptual blending
    
    bool hasBlending() const {
        return left > 0.0f || right > 0.0f || top > 0.0f || bottom > 0.0f;
    }
    
    void reset() {
        left = right = top = bottom = 0.0f;
        gamma = 2.2f;
    }
};

// Forward declarations
class WarpMesh;
class OutputLutTexture;

/**
 * ColorDrawState - the one-shot draw check of an output's color program.
 *
 * The first draw with the color program on an output is bracketed by glGetError():
 * a link that succeeds can still fail every draw (e.g. two sampler types on one
 * unit), which would otherwise be a silent black output. `failed` sends that output
 * back to the plain program for good. Held through a shared_ptr so that every copy
 * of the region (DRMBackend::outputRegions_ is the owner; MultiOutputRenderer keeps
 * two copies) sees a flag set on the renderer's copy.
 */
struct ColorDrawState {
    bool checked = false;   // the first color draw has been checked
    bool failed = false;    // ...and raised a GL error: draw this output without color
};

/**
 * OutputRegion - Defines an output's position and configuration in the virtual canvas
 * 
 * The virtual canvas is a large FBO that contains all outputs.
 * Each OutputRegion specifies:
 * - Where in the canvas this output's content comes from (canvasX/Y)
 * - The size of the region (which may differ from physical output size for scaling)
 * - Edge blending configuration
 * - Optional warp mesh for geometric correction
 */
struct OutputRegion {
    // ===== Identification =====
    std::string name;               // Output name: "HDMI-A-1", "DP-1", etc.
    
    // ===== Canvas Position =====
    // Where this output's content comes from in the virtual canvas
    int canvasX = 0;                // X position in virtual canvas (pixels)
    int canvasY = 0;                // Y position in virtual canvas (pixels)
    int canvasWidth = 0;            // Width in canvas (pixels)
    int canvasHeight = 0;           // Height in canvas (pixels)
    
    // ===== Physical Output =====
    // The actual output resolution (may differ from canvas region for scaling)
    int physicalWidth = 0;          // Physical output width
    int physicalHeight = 0;         // Physical output height
    
    // ===== Blending =====
    BlendEdges blend;               // Edge blending configuration
    
    // ===== Warping =====
    std::shared_ptr<WarpMesh> warpMesh;  // Optional warp mesh for geometric correction
    std::string warpMeshPath;       // Path to warp mesh file (for persistence)
    
    // ===== Color (programme phase 3) =====
    // Trims -> 3D LUT -> blend ramp, inside the output blit. DRMBackend::outputRegions_
    // is the one owner: every apply writes there first and every rebuild copies those
    // regions wholesale, so all three fields must survive a plain copy (they do: the
    // LUT and the draw state are shared, not duplicated).
    OutputColor color;                          // the five trims + configured LUT path/status
    std::shared_ptr<OutputLutTexture> lut;      // uploaded LUT, null = none
    std::shared_ptr<ColorDrawState> colorDraw = std::make_shared<ColorDrawState>();
    
    // ===== State =====
    bool enabled = true;            // Is this output active?
    
    // ===== Helper Methods =====
    
    /**
     * Check if this region has edge blending enabled
     */
    bool hasBlending() const {
        return blend.hasBlending();
    }
    
    /**
     * Check if this region has warping enabled
     */
    bool hasWarping() const {
        return warpMesh != nullptr;
    }
    
    /**
     * Check if this region needs the color blit program (trims active or a LUT loaded)
     */
    bool hasColor() const {
        return color.trimsActive() || lut != nullptr;
    }
    
    /**
     * Get the canvas rect as floats (for shader uniforms)
     * Returns: x, y, width, height in canvas pixels
     */
    void getCanvasRect(float& x, float& y, float& w, float& h) const {
        x = static_cast<float>(canvasX);
        y = static_cast<float>(canvasY);
        w = static_cast<float>(canvasWidth);
        h = static_cast<float>(canvasHeight);
    }
    
    /**
     * Check if canvas region matches physical size (no scaling)
     */
    bool isOneToOne() const {
        return canvasWidth == physicalWidth && canvasHeight == physicalHeight;
    }
    
    /**
     * Get scale factor (canvas to physical)
     */
    float getScaleX() const {
        return canvasWidth > 0 ? static_cast<float>(physicalWidth) / canvasWidth : 1.0f;
    }
    
    float getScaleY() const {
        return canvasHeight > 0 ? static_cast<float>(physicalHeight) / canvasHeight : 1.0f;
    }
    
    /**
     * Create a default 1:1 region for an output
     */
    static OutputRegion createDefault(const std::string& outputName,
                                      int width, int height, int canvasX = 0, int canvasY = 0) {
        OutputRegion region;
        region.name = outputName;
        region.canvasX = canvasX;
        region.canvasY = canvasY;
        region.canvasWidth = width;
        region.canvasHeight = height;
        region.physicalWidth = width;
        region.physicalHeight = height;
        region.enabled = true;
        return region;
    }
};

/**
 * WarpMesh - Placeholder for geometric warp mesh
 * 
 * TODO: Implement mesh loading and GPU representation
 * Supports common mesh formats for keystone/geometric correction.
 */
class WarpMesh {
public:
    WarpMesh() = default;
    virtual ~WarpMesh() = default;
    
    /**
     * Load mesh from file
     * @param path Path to mesh file (.obj, .mesh, etc.)
     * @return true on success
     */
    virtual bool loadFromFile(const std::string& path) { 
        (void)path;
        return false; 
    }
    
    /**
     * Get mesh GPU texture (displacement map)
     * @return OpenGL texture ID, or 0 if not loaded
     */
    virtual unsigned int getTexture() const { return 0; }
    
    /**
     * Check if mesh is loaded and valid
     */
    virtual bool isValid() const { return false; }
    
    /**
     * Get mesh dimensions
     */
    virtual void getDimensions(int& width, int& height) const {
        width = 0;
        height = 0;
    }
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_OUTPUTREGION_H

