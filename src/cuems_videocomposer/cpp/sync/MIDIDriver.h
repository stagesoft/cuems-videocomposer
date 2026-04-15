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

#ifndef VIDEOCOMPOSER_MIDIDRIVER_H
#define VIDEOCOMPOSER_MIDIDRIVER_H

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace videocomposer {

/**
 * MIDIDriver - Abstract base class for MIDI driver implementations
 * 
 * Provides a common interface for different MIDI backends:
 * - ALSA Sequencer
 * - PortMidi
 * - ALSA Raw MIDI
 */
class MIDIDriver {
public:
    virtual ~MIDIDriver() = default;

    /**
     * Open MIDI connection
     * @param portId Port identifier (device name, port number, or "-1" for autodetect)
     * @return true if connection successful
     */
    virtual bool open(const std::string& portId) = 0;

    /**
     * Close MIDI connection
     */
    virtual void close() = 0;

    /**
     * Check if MIDI is connected
     * @return true if connected
     */
    virtual bool isConnected() const = 0;

    /**
     * Poll for MIDI messages and return current frame number
     * @return Frame number, or -1 if not available
     */
    virtual int64_t pollFrame() = 0;

    /**
     * Get driver name
     * @return Driver name string
     */
    virtual const char* getName() const = 0;

    /**
     * Check if this driver is supported (compiled in)
     * @return true if supported
     */
    virtual bool isSupported() const = 0;
};

/**
 * MIDIDriverFactory - Factory for creating MIDI drivers
 */
class MIDIDriverFactory {
public:
    /**
     * Create a MIDI driver by name
     * @param driverName Driver name (e.g., "ALSA-Sequencer", "PORTMIDI")
     * @return MIDI driver instance, or nullptr if not found/unsupported
     */
    static std::unique_ptr<MIDIDriver> create(const std::string& driverName);

    /**
     * Create the first available MIDI driver
     * @return MIDI driver instance, or nullptr if none available
     */
    static std::unique_ptr<MIDIDriver> createFirstAvailable();

    /**
     * Get list of available driver names
     * @return Vector of driver names
     */
    static std::vector<std::string> getAvailableDrivers();
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_MIDIDRIVER_H

