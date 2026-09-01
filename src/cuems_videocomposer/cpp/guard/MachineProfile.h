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

#ifndef VIDEOCOMPOSER_MACHINE_PROFILE_H
#define VIDEOCOMPOSER_MACHINE_PROFILE_H

#include <string>

namespace videocomposer {

/**
 * MachineProfile - which box this is, and therefore what the hang guard knows.
 *
 * The vcn_dec ring hang (869en65tm) is a property of one GPU generation under
 * one memory configuration, not of the compositor. So the cap that protects
 * against it is only meaningful on hardware where the boundary was actually
 * measured, and claiming a number anywhere else would be inventing evidence.
 * This is the lookup that decides which of those two situations we are in.
 *
 * **Only fp530-8gb ships armed.** Every other profile is monitor-only: the
 * warnings still run, nothing is ever refused, and the startup log says so in
 * as many words. A profile earns a cap by having a measurement behind it, and
 * the only such measurement today is the FP530 8 GB one - 4 concurrent
 * 4K-class decode sessions clean, 5 and beyond in the hang region.
 *
 * Detection is PCI vendor:device from sysfs plus total RAM, both read once at
 * startup. The resolved DRM card path is kept here too, because the saturation
 * monitor's VRAM/GTT readers need the same answer and one wrong card is better
 * found in one place than in three.
 */
struct MachineProfile {
    /** Stable profile identifier, as it appears in the startup log. */
    std::string name = "unknown";

    /**
     * Concurrent 4K-class decode sessions allowed. 0 means "no cap" -
     * monitor-only, nothing is ever refused. Only a measured boundary may
     * put a non-zero number here.
     */
    int cap = 0;

    /**
     * Whether the guard may refuse. False on every profile without a measured
     * boundary, and forced false by --hang-guard=off.
     */
    bool armed = false;

    /** Human-readable reason the profile resolved this way (for the log). */
    std::string detail;

    /** Where the profile came from: "compiled" (autodetected) or "flag". */
    std::string source = "compiled";

    /** PCI ids as read, for the log. -1 when unreadable. */
    long pciVendor = -1;
    long pciDevice = -1;

    /** Total system RAM in MiB as read from /proc/meminfo, -1 when unknown. */
    long ramTotalMb = -1;

    /**
     * Resolved DRM device sysfs directory, e.g. "/sys/class/drm/card0/device".
     * Empty when no card could be resolved. Fixed at detection time: the
     * saturation monitor's sysfs readers run on the fatal-signal path in
     * ExitReporter's neighbourhood, where opendir() and allocation are both
     * forbidden, so the scan happens exactly once, here, at startup.
     */
    std::string drmDevicePath;

    /** True when the profile has a measured cap it could arm. */
    bool hasCap() const { return cap > 0; }

    /**
     * Autodetect from the running hardware. Never fails: an unrecognised box
     * resolves to the "unknown" profile, which is monitor-only and says so.
     */
    static MachineProfile detect();

    /**
     * Look up a profile by name, for --vc-profile=<name>.
     * Returns false when the name is not in the table - the caller must treat
     * that as a loud startup error, never as a silent fallback.
     */
    static bool byName(const std::string& name, MachineProfile& out);

    /** Comma-separated list of valid profile names, for error messages. */
    static std::string knownNames();
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_MACHINE_PROFILE_H
