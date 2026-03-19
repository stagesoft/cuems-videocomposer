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

#ifndef VIDEOCOMPOSER_FRAMERATECONVERTERSYNCSOURCE_H
#define VIDEOCOMPOSER_FRAMERATECONVERTERSYNCSOURCE_H

#include "SyncSource.h"
#include "../input/InputSource.h"

namespace videocomposer {

/**
 * FramerateConverterSyncSource - Wraps any SyncSource and converts framerate
 * 
 * This adapter converts frames from the sync source's framerate to the input
 * source's framerate using Option 2 (resample): videoFrame = rint(syncFrame * inputFps / syncFps)
 * 
 * This is timecode-agnostic and works with:
 * - Any sync source (MIDI, LTC, JACK, etc.)
 * - Any input source (video files, live feeds, streams, etc.)
 * 
 * The conversion is applied automatically if both framerates are known and different.
 */
class FramerateConverterSyncSource : public SyncSource {
public:
    /**
     * Create a framerate converter that wraps a sync source (non-owning reference)
     * @param syncSource The sync source to wrap (keeps reference, does not take ownership)
     * @param inputSource The input source to get target framerate from (keeps reference)
     */
    FramerateConverterSyncSource(SyncSource* syncSource, InputSource* inputSource);
    
    virtual ~FramerateConverterSyncSource() = default;

    // SyncSource interface - delegates to wrapped sync source
    bool connect(const char* param = nullptr) override;
    void disconnect() override;
    bool isConnected() const override;
    int64_t pollFrame(uint8_t* rolling = nullptr) override;
    int64_t getCurrentFrame() const override;
    const char* getName() const override;
    double getFramerate() const override;

    /**
     * Update the input source reference (e.g., when input source changes)
     * @param inputSource New input source (can be nullptr)
     */
    void setInputSource(InputSource* inputSource) { inputSource_ = inputSource; }
    
    /**
     * Get the underlying sync source's framerate (e.g. MTC 25fps) before conversion.
     * Returns <= 0 if unknown.
     */
    double getSourceFramerate() const;

    /**
     * Check if a full frame SYSEX was just received - delegates to wrapped sync source
     */
    bool wasFullFrameReceived() override;
    
    /**
     * Delegate to wrapped sync source's getTimeMs()
     */
    long getTimeMs() const override;

private:
    SyncSource* wrappedSyncSource_;  // Non-owning reference to sync source
    InputSource* inputSource_;  // Non-owning reference
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_FRAMERATECONVERTERSYNCSOURCE_H

