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

#include "SMPTEWrapper.h"
#include "SMPTEUtils.h"
#include <string>
#include <cstring>

extern "C" {

// External globals from C codebase (needed for compatibility)
extern double framerate;
extern int want_dropframes;
extern int want_autodrop;
extern int have_dropframes;
#ifdef HAVE_MIDI
extern int midi_clkconvert;
#else
static int midi_clkconvert = 0;
#endif

int64_t smptestring_to_frame(const char* str) {
    if (!str) {
        return 0;
    }
    
    std::string smpteStr(str);
    return videocomposer::SMPTEUtils::smpteStringToFrame(
        smpteStr, 
        framerate, 
        have_dropframes != 0,
        want_dropframes != 0,
        want_autodrop != 0
    );
}

int frame_to_smptestring(char* smptestring, int64_t frame, uint8_t add_sign) {
    if (!smptestring) {
        return 0;
    }
    
    std::string result = videocomposer::SMPTEUtils::frameToSmpteString(
        frame,
        framerate,
        add_sign != 0,
        have_dropframes != 0,
        want_dropframes != 0,
        want_autodrop != 0
    );
    
    // Copy to output buffer (ensure null termination)
    strncpy(smptestring, result.c_str(), 13);
    smptestring[13] = '\0';
    
    // Return overflow value (from original implementation)
    // The original function returned s.v[SMPTE_OVERFLOW], but our implementation
    // doesn't expose this. Return 0 for compatibility.
    return 0;
}

int64_t smpte_to_frame(int type, int f, int s, int m, int h, int overflow) {
    return videocomposer::SMPTEUtils::smpteToFrame(
        type, f, s, m, h, overflow,
        framerate,
        want_dropframes != 0,
        midi_clkconvert
    );
}

} // extern "C"

