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

#ifndef VIDEOCOMPOSER_XINERAMAHELPER_H
#define VIDEOCOMPOSER_XINERAMAHELPER_H

#include <vector>
#include <cstdint>
#include <cstddef>

namespace videocomposer {

/**
 * DisplayInfo - Information about a single display
 */
struct DisplayInfo {
    int x;          // X position
    int y;          // Y position
    int width;      // Width
    int height;     // Height
    int screen;     // Screen number
    bool primary;   // Is this the primary display?
};

/**
 * XineramaHelper - Xinerama detection and management
 * 
 * Detects and manages multi-display setups using Xinerama extension.
 * Supports both single window spanning multiple monitors and
 * individual windows per display.
 */
class XineramaHelper {
public:
    XineramaHelper();
    ~XineramaHelper();

    // Detect displays using Xinerama
    bool detectDisplays();

    // Get display count
    size_t getDisplayCount() const { return displays_.size(); }

    // Get display info
    const DisplayInfo& getDisplay(size_t index) const;
    bool getDisplayInfo(size_t index, DisplayInfo& info) const;

    // Get primary display
    const DisplayInfo* getPrimaryDisplay() const;

    // Check if Xinerama is available
    bool isAvailable() const { return available_; }

    // Get combined display area (all displays)
    void getCombinedArea(int& x, int& y, int& width, int& height) const;

private:
    std::vector<DisplayInfo> displays_;
    bool available_;
    
    void clearDisplays();
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_XINERAMAHELPER_H

