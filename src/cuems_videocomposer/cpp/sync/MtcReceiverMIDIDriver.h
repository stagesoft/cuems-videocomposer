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

/**
 * MtcReceiverMIDIDriver.h - Adapter for mtcreceiver to MIDIDriver interface
 *
 * This adapter wraps the mtcreceiver library to provide MTC synchronization
 * using the proven mtcreceiver implementation from cuems-audioplayer.
 */

#ifndef VIDEOCOMPOSER_MTCRECEIVER_MIDI_DRIVER_H
#define VIDEOCOMPOSER_MTCRECEIVER_MIDI_DRIVER_H

#include "MIDIDriver.h"
#include "../../mtcreceiver/mtcreceiver.h"
#include <memory>
#include <mutex>

namespace videocomposer {

/**
 * MIDI driver using mtcreceiver library
 */
class MtcReceiverMIDIDriver : public MIDIDriver {
public:
    MtcReceiverMIDIDriver();
    ~MtcReceiverMIDIDriver() override;

    bool open(const std::string& portId = "") override;
    void close() override;
    bool isConnected() const override;
    int64_t pollFrame() override;
    
    // MIDIDriver interface
    const char* getName() const override { return "mtcreceiver"; }
    bool isSupported() const override { return true; }  // Always supported if compiled in
    
    // Additional configuration (not in base interface, but used by MIDISyncSource)
    void setFramerate(double framerate);
    void setVerbose(bool verbose);
    void setClockAdjustment(bool enable);
    
    // Check if a full frame SYSEX was just received (indicates position jump/seek needed)
    bool wasFullFrameReceived();
    
private:
    std::unique_ptr<MtcReceiver> mtcReceiver_;
    double framerate_;
    bool verbose_;
    bool clockAdjustment_;
    bool lastFullFrameReceived_;
    mutable std::mutex mutex_;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_MTCRECEIVER_MIDI_DRIVER_H

