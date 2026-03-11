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

#ifndef VIDEOCOMPOSER_MTCDECODER_H
#define VIDEOCOMPOSER_MTCDECODER_H

#include <cstdint>
#include <cstring>

namespace videocomposer {

/**
 * MTCDecoder - Decodes MIDI Time Code (MTC) messages
 * 
 * Parses MTC quarter-frame messages and converts them to frame numbers.
 */
class MTCDecoder {
public:
    struct SMPTETimecode {
        int frame;
        int sec;
        int min;
        int hour;
        int type;  // 0=24fps, 1=25fps, 2=29fps, 3=30fps
        int tick;  // Quarter-frame tick (0-7)
    };

    MTCDecoder();
    
    /**
     * Process a MIDI message byte
     * @param data MIDI message byte
     * @return true if complete timecode received
     */
    bool processByte(uint8_t data);

    /**
     * Get the decoded SMPTE timecode
     * @return SMPTE timecode structure
     */
    const SMPTETimecode& getTimecode() const { return lastTC_; }

    /**
     * Convert SMPTE timecode to frame number
     * @param framerate Video framerate
     * @return Frame number, or -1 if invalid
     */
    int64_t timecodeToFrame(double framerate) const;

    /**
     * Reset decoder state
     */
    void reset();

private:
    static const char* MTCTYPE[4];
    
    void parseTimecode(uint8_t data);
    
    SMPTETimecode tc_;
    SMPTETimecode lastTC_;
    int fullTC_;  // Bitmask tracking which quarter-frames received
    int prevTick_;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_MTCDECODER_H

