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

#ifndef VIDEOCOMPOSER_REMOTECONTROL_H
#define VIDEOCOMPOSER_REMOTECONTROL_H

#include <string>

namespace videocomposer {

/**
 * RemoteControl - Abstract base class for remote control protocols
 * 
 * Interface for all remote control protocols (OSC, MessageQueue, IPC, etc.)
 * Only OSCRemoteControl is implemented for now, but architecture is ready
 * for future implementations.
 */
class RemoteControl {
public:
    virtual ~RemoteControl() = default;

    /**
     * Initialize remote control
     * @param port Port number or identifier (protocol-specific)
     * @return true on success, false on failure
     */
    virtual bool initialize(int port) = 0;

    /**
     * Process incoming messages
     * Should be called regularly from main loop
     * @return Number of messages processed
     */
    virtual int process() = 0;

    /**
     * Shutdown remote control
     */
    virtual void shutdown() = 0;

    /**
     * Check if remote control is active
     * @return true if active, false otherwise
     */
    virtual bool isActive() const = 0;

    /**
     * Get protocol name
     * @return String identifier (e.g., "OSC", "MQ", "IPC")
     */
    virtual const char* getProtocolName() const = 0;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_REMOTECONTROL_H

