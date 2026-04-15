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

#include "MIDIDriver.h"
#include "NullMIDIDriver.h"
#include "ALSASeqMIDIDriver.h"
#ifdef HAVE_MTCRECEIVER
#include "MtcReceiverMIDIDriver.h"
#endif
#include <algorithm>

namespace videocomposer {

std::unique_ptr<MIDIDriver> MIDIDriverFactory::create(const std::string& driverName) {
    if (driverName == "None" || driverName.empty()) {
        return std::make_unique<NullMIDIDriver>();
    }
    
    // Try case-insensitive match
    std::string lowerName = driverName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
    
#ifdef HAVE_MTCRECEIVER
    // mtcreceiver driver (preferred, proven implementation)
    if (lowerName == "mtcreceiver" || lowerName == "mtc-receiver" || lowerName == "mtc") {
        return std::make_unique<MtcReceiverMIDIDriver>();
    }
#endif
    
    // ALSA Sequencer driver
    if (lowerName == "alsa-sequencer" || lowerName == "alsa-seq" || lowerName == "alsa") {
        auto driver = std::make_unique<ALSASeqMIDIDriver>();
        if (driver->isSupported()) {
            return driver;
        }
        return nullptr;
    }
    
    return nullptr;
}

std::unique_ptr<MIDIDriver> MIDIDriverFactory::createFirstAvailable() {
    // Try drivers in order of preference
    // Following cuems-audioplayer pattern: mtcreceiver is preferred (proven working)
    
#ifdef HAVE_MTCRECEIVER
    // 1. mtcreceiver (preferred, proven working implementation from cuems-audioplayer)
    auto mtcDriver = std::make_unique<MtcReceiverMIDIDriver>();
    if (mtcDriver->isSupported()) {
        return mtcDriver;
    }
#endif
    
    // 2. ALSA Sequencer (fallback, Linux only, works with aconnect/Midi Through)
    auto alsaDriver = std::make_unique<ALSASeqMIDIDriver>();
    if (alsaDriver->isSupported()) {
        return alsaDriver;
    }
    
    // Fallback to null driver (always available)
    return std::make_unique<NullMIDIDriver>();
}

std::vector<std::string> MIDIDriverFactory::getAvailableDrivers() {
    std::vector<std::string> drivers;
    
    // Null driver is always available
    drivers.push_back("None");
    
#ifdef HAVE_MTCRECEIVER
    // mtcreceiver (preferred)
    auto mtcDriver = std::make_unique<MtcReceiverMIDIDriver>();
    if (mtcDriver->isSupported()) {
        drivers.push_back("mtcreceiver");
    }
#endif
    
    // ALSA Sequencer (check if supported)
    auto alsaDriver = std::make_unique<ALSASeqMIDIDriver>();
    if (alsaDriver->isSupported()) {
        drivers.push_back("ALSA-Sequencer");
    }
    
    return drivers;
}

} // namespace videocomposer

