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

#ifndef VIDEOCOMPOSER_CLEGACYBRIDGE_H
#define VIDEOCOMPOSER_CLEGACYBRIDGE_H

/**
 * CLegacyBridge - Bridge between C++ code and legacy C code
 * 
 * Provides access to C globals that are still used by the legacy C display
 * backend code (display.c, display_x11.c, display_glx.c).
 * 
 * NOTE: This is a minimal bridge for the remaining C display code.
 * - Video file globals have been removed (C++ uses per-layer FrameInfo)
 * - MIDI functions have been removed (C++ uses MIDISyncSource directly)
 * - SMPTEWrapper.cpp still uses some globals for C compatibility
 */

extern "C" {
    // Framerate (used by C display code for screensaver timing, and SMPTEWrapper)
    extern double framerate;
    extern int have_dropframes;
    
    // Configuration flags (used by C display code for logging)
    extern int want_quiet;
    extern int want_verbose;
    extern int want_debug;
}

#endif // VIDEOCOMPOSER_CLEGACYBRIDGE_H

