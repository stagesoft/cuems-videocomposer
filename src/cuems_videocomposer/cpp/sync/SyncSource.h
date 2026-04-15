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

#ifndef VIDEOCOMPOSER_SYNCSOURCE_H
#define VIDEOCOMPOSER_SYNCSOURCE_H

#include <cstdint>

namespace videocomposer {

/**
 * Abstract base class for all synchronization sources.
 * 
 * Each layer can have its own sync source, allowing independent
 * synchronization (JACK, LTC, MIDI, manual, etc.)
 */
class SyncSource {
public:
    virtual ~SyncSource() = default;

    /**
     * Connect to the sync source
     * @param param Optional parameter (e.g., MIDI port name)
     * @return true on success, false on failure
     */
    virtual bool connect(const char* param = nullptr) = 0;

    /**
     * Disconnect from the sync source
     */
    virtual void disconnect() = 0;

    /**
     * Check if connected to sync source
     * @return true if connected, false otherwise
     */
    virtual bool isConnected() const = 0;

    /**
     * Poll for current frame number from sync source
     * @param rolling Optional pointer to set rolling state (true if playing)
     * @return Current frame number, or -1 if not available
     */
    virtual int64_t pollFrame(uint8_t* rolling = nullptr) = 0;

    /**
     * Get the current frame number (last polled value)
     * @return Current frame number, or -1 if not available
     */
    virtual int64_t getCurrentFrame() const = 0;

    /**
     * Get name/identifier of this sync source type
     * @return String identifier (e.g., "MIDI", "LTC")
     */
    virtual const char* getName() const = 0;

    /**
     * Get the framerate of this sync source's timecode
     * @return Framerate in frames per second, or -1.0 if unknown/variable
     */
    virtual double getFramerate() const { return -1.0; }
    
    /**
     * Check if a full frame SYSEX was just received (indicates position jump/seek needed)
     * This is used by MTC sync sources to signal that an explicit position command was received.
     * @return true if a full frame was received since last check
     */
    virtual bool wasFullFrameReceived() { return false; }
    
    /**
     * Get the current timecode position in milliseconds.
     * Used by FramerateConverterSyncSource to compute video frames directly
     * from continuous time, avoiding double-quantization when converting
     * between different frame rates.
     * @return Time in milliseconds, or -1 if not available
     */
    virtual long getTimeMs() const { return -1; }
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_SYNCSOURCE_H

