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

#ifndef VIDEOCOMPOSER_DISPLAYMANAGER_H
#define VIDEOCOMPOSER_DISPLAYMANAGER_H

#include "DisplayBackend.h"
#include "XineramaHelper.h"
#include <vector>
#include <memory>

namespace videocomposer {

/**
 * DisplayManager - Manages multiple output displays
 * 
 * Supports:
 * - Single window spanning multiple monitors (Xinerama/Wayland)
 * - Individual windows per display
 */
class DisplayManager {
public:
    DisplayManager();
    ~DisplayManager();

    // Detect available displays
    bool detectDisplays();

    // Get display count
    size_t getDisplayCount() const { return displays_.size(); }

    // Create window for display
    // mode: 0 = single window spanning all, 1 = individual windows
    bool createWindows(DisplayBackend* backend, int mode = 0);

    // Get display info
    const DisplayInfo& getDisplay(size_t index) const;
    bool getDisplayInfo(size_t index, DisplayInfo& info) const;

    // Get primary display
    const DisplayInfo* getPrimaryDisplay() const;

private:
    XineramaHelper xineramaHelper_;
    std::vector<DisplayInfo> displays_;
    bool displaysDetected_;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_DISPLAYMANAGER_H

