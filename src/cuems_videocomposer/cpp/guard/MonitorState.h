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

#ifndef VIDEOCOMPOSER_MONITOR_STATE_H
#define VIDEOCOMPOSER_MONITOR_STATE_H

#include <cstdint>
#include <string>

namespace videocomposer {

/** Saturation level, as the monitor last decided it. */
enum class SaturationLevel {
    clear,
    warn,
    alarm
};

/**
 * MonitorState - everything the guard and the saturation monitor know, in one
 * non-blocking read.
 *
 * ## Why this shape
 *
 * F9 (the engine's load-time health ping) answers on the OSC thread, so the
 * answer has to be cached state rather than a computation: every field here is
 * read from a producer-owned atomic and nothing acquires a lock. The fields
 * may tear across each other - two counts read microseconds apart can belong
 * to different instants - and that is accepted, because no consumer needs a
 * consistent cross-field snapshot and promising one would cost a lock on the
 * OSC thread.
 *
 * ## Why every input is a separate field
 *
 * The operator-facing plan for this data is a green/amber/red load monitor in
 * the frontend. That collapse is the UI's job, not this program's: the light
 * is a pure function of these fields, so the mapping can be changed - or a
 * future policy can re-derive it differently - without touching the
 * compositor. Concretely, the intended mapping is
 *
 *   green   armedFourK <= cap AND activeFourK <= cap AND saturation clear
 *   amber   advisoryLatched OR saturation == warn
 *   red     refusals grew, OR saturation == alarm, OR a page-fault alarm
 *
 * with one caveat worth stating where the fields live: on an ordinary chained
 * show the engine arms the whole cue chain at load, so `advisoryLatched` is
 * true for the entire show. Amber is therefore the *normal* colour for
 * exactly the content reveal-counting exists to permit, and a UI that wants
 * to stay readable should distinguish a standing exposure from a fresh
 * crossing rather than treating amber as an alert.
 *
 * ## Availability, which is not the same as "fine"
 *
 * A channel that cannot be read must not be indistinguishable from a channel
 * reading clean. The page-fault channel in particular depends on /dev/kmsg
 * being readable by the cuems user; where it is not, `pageFaultAvailable` is
 * false and a UI must show that channel as unavailable rather than green.
 * The same applies to decode occupancy on i915, which never reports it.
 */
struct MonitorState {
    // --- the guard -------------------------------------------------------
    /** Actively-decoding 4K-class sessions. The quantity the cap acts on. */
    int activeFourK = 0;
    /** Loaded, 4K-class-classified sources - decoding or not. Drives the ADVISORY. */
    int armedFourK = 0;
    /** Actively-decoding exempt (<= HD) sessions. Never refused at any count. */
    int activeExempt = 0;
    /** Concurrent 4K-class sessions allowed. 0 means no cap: monitor-only. */
    int cap = 0;
    /** Resolved machine profile name. */
    std::string profile = "unknown";
    /** Whether a refusal can currently happen (profile armed and capped). */
    bool guardArmed = false;
    /** True while armedFourK > cap. Latched state, not an event: a polling UI cannot miss it. */
    bool advisoryLatched = false;
    /** Layers refused since start. */
    long refusals = 0;

    // --- the saturation monitor -----------------------------------------
    /** Last decided saturation level. */
    SaturationLevel saturation = SaturationLevel::clear;
    /** Saturation alarms raised since start. */
    long saturationAlarms = 0;
    /** False when no saturation signal can be read at all. */
    bool saturationAvailable = false;

    /** Decode engine occupancy percent, or -1 when the platform does not report it. */
    double decodeOccupancyPercent = -1.0;
    /** False on i915, which does not expose drm-engine-dec. */
    bool decodeOccupancyAvailable = false;

    /** VRAM/GTT in MiB, -1 when unreadable. */
    long vramUsedMb = -1;
    long vramTotalMb = -1;
    long gttUsedMb = -1;
    bool memoryAvailable = false;

    /** IOMMU page-fault alarms observed since start. */
    long pageFaultAlarms = 0;
    /**
     * False when the kernel ring buffer could not be opened. The sharpest
     * "it is happening now" signal we have is then simply absent, and a UI
     * must say so rather than imply the channel is clear.
     */
    bool pageFaultAvailable = false;

    // --- reserved for F9 -------------------------------------------------
    /**
     * Whether admission is currently closed by an emergency escalation.
     * Ships as a constant false: v1 raises an ALARM on a decode-error burst
     * and never closes admission, because a burst is precursor-grade
     * evidence - one preceded a hang, another killed a client with no hang -
     * and "not refusing means a hard hang" cannot be claimed for it. The
     * field exists so the F9 UI contract does not change when that decision
     * is revisited.
     */
    bool emergencyAdmissionClosed = false;
    bool emergencyAdmissionCloseAvailable = false;

    /** Milliseconds since the epoch of the last state transition, 0 if none. */
    int64_t lastTransitionMs = 0;
};

/**
 * Assemble the current state from every producer.
 *
 * Non-blocking and callable from the OSC or render thread. Fields tear across
 * producers by design; see the note above.
 */
MonitorState monitorSnapshot();

} // namespace videocomposer

#endif // VIDEOCOMPOSER_MONITOR_STATE_H
