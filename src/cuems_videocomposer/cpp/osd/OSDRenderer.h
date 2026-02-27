/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (C) 2020-2026 Stage Lab Coop.
 * Author: Ion Reguera <ion@stagelab.coop>
 *
 * This file is part of cuems-videocomposer.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef VIDEOCOMPOSER_OSDRENDERER_H
#define VIDEOCOMPOSER_OSDRENDERER_H

#include "OSDManager.h"
#include <string>
#include <memory>
#include <vector>
#include <cstdint>

namespace videocomposer {

/**
 * OSDRenderer - Renders OSD text using Freetype to OpenGL textures
 * 
 * This class handles the actual rendering of OSD elements onto the display.
 * It renders text using Freetype to bitmaps, then creates OpenGL textures
 * that can be composited by OpenGLRenderer.
 */
struct OSDRenderItem {
    unsigned int textureId;
    int x, y;
    int width, height;
    bool hasBox;  // Draw black box background
};

class OSDRenderer {
public:
    OSDRenderer();
    ~OSDRenderer();

    // Initialize renderer
    bool init();
    
    // Cleanup
    void cleanup();

    // Prepare OSD elements for rendering
    // Returns a list of render items that OpenGLRenderer can render
    std::vector<OSDRenderItem> prepareOSDRender(OSDManager* osd, int windowWidth, int windowHeight);

    // Check if Freetype is available
    bool isFreetypeAvailable() const { return freetypeAvailable_; }

    // Load font file
    bool loadFont(const std::string& fontFile);

private:
    bool freetypeAvailable_;
    bool initialized_;
    
    // Freetype state (if available)
    void* ftLibrary_;  // FT_Library
    void* ftFace_;     // FT_Face
    std::string currentFontFile_;
    int fontSize_;
    
    // Text rendering to texture
    unsigned int renderTextToTexture(const std::string& text, int& width, int& height, bool createBox);
    void cleanupTexture(unsigned int textureId);
    
    // Calculate text position based on alignment
    int calculateXPosition(int xAlign, int textWidth, int windowWidth);
    int calculateYPosition(int yPercent, int textHeight, int windowHeight);
    
    // Measure text dimensions
    void measureText(const std::string& text, int& width, int& height);
    
    // Calculate font size based on video/window height (matches xjadeo)
    int calculateFontSize(int videoHeight) const;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_OSDRENDERER_H

