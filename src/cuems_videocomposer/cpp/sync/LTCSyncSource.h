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

#ifndef VIDEOCOMPOSER_LTCSYNCSOURCE_H
#define VIDEOCOMPOSER_LTCSYNCSOURCE_H

#include "SyncSource.h"
#include <cstdint>

namespace videocomposer {

/**
 * LTCSyncSource - LTC (Linear Time Code) synchronization source
 * 
 * Implements SyncSource interface for LTC synchronization.
 * Uses libltc to decode LTC timecode from audio signal.
 */
class LTCSyncSource : public SyncSource {
public:
    LTCSyncSource();
    virtual ~LTCSyncSource();

    // SyncSource interface
    bool connect(const char* param = nullptr) override;
    void disconnect() override;
    bool isConnected() const override;
    int64_t pollFrame(uint8_t* rolling = nullptr) override;
    int64_t getCurrentFrame() const override;
    const char* getName() const override { return "LTC"; }

    /**
     * Set framerate for frame calculation
     * This should match the video file's framerate
     * @param fps Framerate in frames per second
     */
    void setFramerate(double fps);

private:
    double framerate_;
    int64_t currentFrame_;
    bool connected_;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_LTCSYNCSOURCE_H

