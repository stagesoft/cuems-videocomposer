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

#ifndef VIDEOCOMPOSER_NULLMIDIDRIVER_H
#define VIDEOCOMPOSER_NULLMIDIDRIVER_H

#include "MIDIDriver.h"

namespace videocomposer {

/**
 * NullMIDIDriver - Null/dummy MIDI driver implementation
 * 
 * Used when no MIDI drivers are available or as a fallback.
 */
class NullMIDIDriver : public MIDIDriver {
public:
    bool open(const std::string& portId) override { return false; }
    void close() override {}
    bool isConnected() const override { return false; }
    int64_t pollFrame() override { return -1; }
    const char* getName() const override { return "None"; }
    bool isSupported() const override { return true; }  // Always available
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_NULLMIDIDRIVER_H

