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
#include <ctime>

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
                long timeMs = wrappedSyncSource_->getTimeMs();
                if (timeMs >= 0) {
                    double seconds = static_cast<double>(timeMs) / 1000.0;
                    int64_t rawFrame = static_cast<int64_t>(std::floor(seconds * inputFps));

                    // Vsync-aware cadence smoother.
                    // When display_rate / video_fps is close to an integer N
                    // (e.g. 60/29.97 ≈ 2), enforce a minimum hold of N vsyncs
                    // per frame so the phase drift between vsync and frame
                    // boundaries never causes visible 1-vsync / (N+1)-vsync
                    // stutter pairs.  When the ratio is NOT near-integer
                    // (e.g. 60/24 = 2.5, 60/25 = 2.4), use floor(ratio) and
                    // let the drift catch-up produce the natural N:(N+1)
                    // pulldown pattern (e.g. 3:2 for 24fps @ 60Hz).
                    //
                    // The display refresh rate is measured from the actual call
                    // interval (pollFrame is called once per vsync) instead of
                    // being hardcoded, so this works for any display rate.
                    static int64_t s_displayFrame  = -1;
                    static int     s_vsyncCount    =  0;
                    static double  s_vsyncPeriodMs =  16.667; // initial guess
                    static long long s_lastCallUs  =  0;

                    struct timespec _now;
                    clock_gettime(CLOCK_MONOTONIC, &_now);
                    long long nowUs = static_cast<long long>(_now.tv_sec) * 1000000LL
                                    + _now.tv_nsec / 1000LL;

                    if (s_lastCallUs > 0) {
                        double deltaMs = static_cast<double>(nowUs - s_lastCallUs) / 1000.0;
                        // Only accept plausible vsync intervals (5–50ms)
                        if (deltaMs > 5.0 && deltaMs < 50.0) {
                            s_vsyncPeriodMs = 0.95 * s_vsyncPeriodMs + 0.05 * deltaMs;
                        }
                    }
                    s_lastCallUs = nowUs;

                    double frameDurMs = 1000.0 / inputFps;
                    double ratio = frameDurMs / s_vsyncPeriodMs;
                    int idealVsyncs = static_cast<int>(std::floor(ratio));
                    if (idealVsyncs < 1) idealVsyncs = 1;

                    if (s_displayFrame < 0
                        || rawFrame < s_displayFrame
                        || rawFrame > s_displayFrame + 2) {
                        s_displayFrame = rawFrame;
                        s_vsyncCount   = 0;
                    } else {
                        s_vsyncCount++;
                        int64_t drift = rawFrame - s_displayFrame;

                        if (drift >= 1 && s_vsyncCount >= idealVsyncs) {
                            s_displayFrame++;
                            s_vsyncCount = 0;
                        } else if (drift >= 2) {
                            s_displayFrame++;
                            s_vsyncCount = 0;
                        }
                    }

                    syncFrame = s_displayFrame;
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

