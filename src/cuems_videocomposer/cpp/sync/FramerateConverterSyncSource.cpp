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

#include "FramerateConverterSyncSource.h"
#include <cmath>

namespace videocomposer {

FramerateConverterSyncSource::FramerateConverterSyncSource(
    SyncSource* syncSource,
    InputSource* inputSource)
    : wrappedSyncSource_(syncSource)
    , inputSource_(inputSource)
{
    if (!wrappedSyncSource_) {
        // Can't work without a sync source
        wrappedSyncSource_ = nullptr;
    }
}

bool FramerateConverterSyncSource::connect(const char* param) {
    if (!wrappedSyncSource_) {
        return false;
    }
    return wrappedSyncSource_->connect(param);
}

void FramerateConverterSyncSource::disconnect() {
    if (wrappedSyncSource_) {
    wrappedSyncSource_->disconnect();
    }
}

bool FramerateConverterSyncSource::isConnected() const {
    if (!wrappedSyncSource_) {
        return false;
    }
    return wrappedSyncSource_->isConnected();
}

int64_t FramerateConverterSyncSource::pollFrame(uint8_t* rolling) {
    if (!wrappedSyncSource_) {
        return -1;
    }
    // Get frame from wrapped sync source
    int64_t syncFrame = wrappedSyncSource_->pollFrame(rolling);
    
    if (syncFrame >= 0 && inputSource_) {
        double syncFps = wrappedSyncSource_->getFramerate();
        if (syncFps > 0) {
            FrameInfo info = inputSource_->getFrameInfo();
            double inputFps = info.framerate;
            
            if (inputFps > 0 && std::abs(syncFps - inputFps) > 0.01) {
                // When framerates differ, compute video frame directly from
                // the continuous millisecond time to avoid double-quantization.
                // floor(floor(ms*syncFps/1000) * inputFps/syncFps) skips frames
                // because the first floor() loses sub-frame precision.
                long timeMs = wrappedSyncSource_->getTimeMs();
                if (timeMs >= 0) {
                    double seconds = static_cast<double>(timeMs) / 1000.0;
                    syncFrame = static_cast<int64_t>(std::floor(seconds * inputFps));
                } else {
                    // Fallback: frame-based conversion (may skip frames)
                    syncFrame = static_cast<int64_t>(std::floor(
                        static_cast<double>(syncFrame) * inputFps / syncFps));
                }
            }
        }
    }
    
    return syncFrame;
}

int64_t FramerateConverterSyncSource::getCurrentFrame() const {
    if (!wrappedSyncSource_) {
        return -1;
    }
    return wrappedSyncSource_->getCurrentFrame();
}

const char* FramerateConverterSyncSource::getName() const {
    if (!wrappedSyncSource_) {
        return "None";
    }
    return wrappedSyncSource_->getName();
}

double FramerateConverterSyncSource::getFramerate() const {
    // Return the input source's framerate if available, otherwise sync source's framerate
    if (inputSource_) {
        FrameInfo info = inputSource_->getFrameInfo();
        if (info.framerate > 0) {
            return info.framerate;
        }
    }
    if (!wrappedSyncSource_) {
        return -1.0;
    }
    return wrappedSyncSource_->getFramerate();
}

double FramerateConverterSyncSource::getSourceFramerate() const {
    if (wrappedSyncSource_) {
        return wrappedSyncSource_->getFramerate();
    }
    return -1.0;
}

bool FramerateConverterSyncSource::wasFullFrameReceived() {
    if (!wrappedSyncSource_) {
        return false;
    }
    return wrappedSyncSource_->wasFullFrameReceived();
}

long FramerateConverterSyncSource::getTimeMs() const {
    if (wrappedSyncSource_) {
        return wrappedSyncSource_->getTimeMs();
    }
    return -1;
}

} // namespace videocomposer

