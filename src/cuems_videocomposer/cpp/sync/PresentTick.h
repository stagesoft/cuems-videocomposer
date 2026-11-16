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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file PresentTick.h
 * @brief The display backend's vblank tick and current-vblank sample for the cadence engine.
 *
 * Plan 1amfwm7gpz7 (cuems-RELATIONS Plans/2026-11-04-vc-cadence-exact-ratio-min-hold-1amfwm7gpz7.md, §5.1).
 * Process-wide values written by the DRM backend and read by FramerateConverterSyncSource; both run on the
 * render thread (updateLayers() then render()), so there is no locking. A tick with msc < 0 means "none": the
 * smoother keeps its legacy code path.
 */

#pragma once

#include <cstdint>

namespace videocomposer {

/// The pacing CRTC's last completed flip (published after the flip wait of the coupled render path).
struct PresentTick {
    int64_t msc = -1;       ///< vblank sequence of the flip (kernel u32 widened); < 0 = no tick
    int64_t ustNs = 0;      ///< CLOCK_MONOTONIC time of that vblank, ns
    int64_t periodNs = 0;   ///< the mode's vsync period (clock*1000/(htotal*vtotal)), ns
    uint32_t seq = 0;       ///< publication counter
};

/// The pacing CRTC's state at the poll (decision S, option (c)).
struct VblankNow {
    int64_t msc = -1;         ///< the vblank that began most recently (drmCrtcGetSequence); < 0 = none
    int64_t ustNs = 0;        ///< its CLOCK_MONOTONIC time, ns
    int64_t prevLanding = -1; ///< the vblank the previous submit lands on (sequence at submit + 1); < 0 = unknown
    int64_t sampledAtNs = 0;  ///< CLOCK_MONOTONIC time of the sample (the poll's offset from the vblank)
    bool fromIoctl = false;   ///< true when drmCrtcGetSequence answered; false = derived from the tick (fallback)
};

PresentTick currentPresentTick();
void publishPresentTick(const PresentTick& tick);

VblankNow currentVblank();
void publishVblankNow(const VblankNow& now);

/// Tests (RAII TickScope) and the kill switch: both values back to "none".
void resetPresentTick();

/// The presentation vblank of a frame chosen at a poll that read `now` (plan §5.2 rule 1).
inline int64_t presentationTarget(const VblankNow& now, const PresentTick& tick, int pipelineDepth) {
    if (now.msc < 0) {
        return tick.msc < 0 ? -1 : tick.msc + pipelineDepth;  // option (b): no current-vblank sample
    }
    const int64_t prev = now.prevLanding >= 0 ? now.prevLanding : now.msc + 1;
    return (prev > now.msc ? prev : now.msc) + 1;              // option (c)
}

/// VIDEOCOMPOSER_CADENCE_TRACE=1: one journal line per poll and per flip (step 1 of the plan; a firehose).
bool cadenceTraceEnabled();

/// VIDEOCOMPOSER_CADENCE_LEGACY=1: the backend publishes nothing; every layer runs the legacy smoother.
bool cadenceLegacyForced();

/// Frames in flight between the poll and the screen in the coupled loop (poll -> draw -> wait -> submit -> next vblank).
constexpr int kPipelineDepth = 2;

/// RAII helper for tests: resets the tick and the current-vblank sample on construction and destruction.
struct TickScope {
    TickScope() { resetPresentTick(); }
    ~TickScope() { resetPresentTick(); }
};

} // namespace videocomposer
