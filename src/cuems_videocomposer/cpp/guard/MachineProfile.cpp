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

#include "MachineProfile.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace videocomposer {

namespace {

// PCI ids the measurement campaign actually ran on.
constexpr long PCI_VENDOR_AMD   = 0x1002;
constexpr long PCI_VENDOR_INTEL = 0x8086;
constexpr long PCI_DEV_PICASSO  = 0x15d8;  // FP530 (Ryzen Picasso, VCN 1.0)
constexpr long PCI_DEV_PHOENIX  = 0x15bf;  // 780M / 8845HS (VCN 4.0, unified ring)

// RAM is NOT a discriminator, and stopped being one on 2026-09-04. It was: a
// MemTotal ceiling of 12 GiB split Picasso into `fp530-8gb` (armed, cap 4) and
// `fp530-16gb` (monitor-only). Two things were wrong with that.
//
//   1. Fitting more RAM SILENTLY DISARMED the guard. The roomier box shipped
//      LESS protected, with nothing failing and only a startup log line to say
//      so - on a rig with no remote power that ends in a trip to site.
//   2. It was not even a stable reading of the hardware. MemTotal is installed
//      RAM minus the UMA carve-out, which is a BIOS setting, so a 16 GB board
//      with a >=4 GiB carve-out dropped back under the ceiling and re-armed
//      under a name that then described neither its RAM nor its carve-out.
//
// The hang does not come from system memory. It is the VCN 1.0 single decode
// ring: the signature reproduced identically across a GPU firmware upgrade, and
// G12 retired VRAM as the anchor outright - a 12-session run held 4936 MB of
// 5120 addressable (96 %) WITHOUT hanging, while a run that hung held less.
// So the GPU is the discriminator, and RAM is a detail for the log.

/**
 * Read a small text file into buf. Returns the byte count, or -1.
 * Deliberately plain read()/close(): this runs at startup, but it reads the
 * same nodes the signal-path readers use and staying in the same idiom keeps
 * the two honest about each other.
 */
ssize_t readSmallFile(const char* path, char* buf, size_t bufSize) {
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = ::read(fd, buf, bufSize - 1);
    ::close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return n;
}

/** Parse a sysfs "0x1002\n" style hex id. Returns -1 when unreadable. */
long readHexId(const std::string& path) {
    char buf[32];
    if (readSmallFile(path.c_str(), buf, sizeof(buf)) < 0) return -1;
    long v = -1;
    if (std::sscanf(buf, "%lx", &v) != 1) return -1;
    return v;
}

/** Total RAM in MiB from /proc/meminfo, or -1. */
long readRamTotalMb() {
    char buf[256];
    if (readSmallFile("/proc/meminfo", buf, sizeof(buf)) < 0) return -1;
    // MemTotal is the first line on every kernel we run.
    const char* p = std::strstr(buf, "MemTotal:");
    if (!p) return -1;
    long kb = 0;
    if (std::sscanf(p, "MemTotal: %ld kB", &kb) != 1) return -1;
    return kb / 1024;
}

/**
 * Find the render GPU's sysfs device directory.
 *
 * Prefers a card that exposes amdgpu's memory nodes, because that is the card
 * whose numbers the monitor wants; otherwise takes the lowest-numbered card
 * with a readable vendor id. Returns an empty string when there is no DRM at
 * all (a headless test host, or a container without /sys/class/drm).
 */
std::string resolveDrmDevicePath() {
    DIR* dir = ::opendir("/sys/class/drm");
    if (!dir) return std::string();

    std::vector<std::string> candidates;
    while (dirent* e = ::readdir(dir)) {
        const char* n = e->d_name;
        if (std::strncmp(n, "card", 4) != 0) continue;
        // "card0" yes, "card0-HDMI-A-1" no - connectors are not devices.
        if (std::strchr(n + 4, '-') != nullptr) continue;
        candidates.push_back(std::string("/sys/class/drm/") + n + "/device");
    }
    ::closedir(dir);

    if (candidates.empty()) return std::string();
    std::sort(candidates.begin(), candidates.end());

    for (const std::string& c : candidates) {
        char probe[64];
        if (readSmallFile((c + "/mem_info_vram_total").c_str(), probe, sizeof(probe)) > 0) {
            return c;  // amdgpu: the card with the numbers we actually read
        }
    }
    for (const std::string& c : candidates) {
        if (readHexId(c + "/vendor") >= 0) return c;
    }
    return candidates.front();
}

MachineProfile makeUnknown() {
    MachineProfile p;
    p.name = "unknown";
    p.cap = 0;
    p.armed = false;
    p.detail = "no measured hang boundary for this hardware - monitor only, "
               "nothing will be refused";
    return p;
}

} // namespace

MachineProfile MachineProfile::detect() {
    MachineProfile p = makeUnknown();
    p.drmDevicePath = resolveDrmDevicePath();
    p.ramTotalMb = readRamTotalMb();

    if (p.drmDevicePath.empty()) {
        p.detail = "no DRM device found - monitor only, nothing will be refused";
        return p;
    }

    p.pciVendor = readHexId(p.drmDevicePath + "/vendor");
    p.pciDevice = readHexId(p.drmDevicePath + "/device");

    if (p.pciVendor == PCI_VENDOR_AMD && p.pciDevice == PCI_DEV_PICASSO) {
        p.name = "fp530-picasso";
        p.cap = 4;
        p.armed = true;
        // The boundary was measured on an 8 GB board and is applied to every
        // Picasso, because the ring - not the RAM - is what saturates. That is
        // deliberately the conservative direction: if a roomier board turns out
        // to tolerate more, relaxing the cap is a change backed by a new
        // measurement, whereas leaving it unarmed until someone measures ships
        // an unprotected box today. ramTotalMb is still read and logged, so the
        // reading can be re-attributed later without guessing.
        p.detail = "measured boundary: 4 concurrent 4K-class decode sessions "
                   "clean, 5 and beyond in the hang region (measured on 8 GB; "
                   "the boundary is the VCN 1.0 decode ring, not system RAM)";
        return p;
    }

    if (p.pciVendor == PCI_VENDOR_AMD && p.pciDevice == PCI_DEV_PHOENIX) {
        p.name = "4ktop-780m";
        p.detail = "VCN 4.0 with a unified ring - the FP530 number does not "
                   "transfer - monitor only";
        return p;
    }

    if (p.pciVendor == PCI_VENDOR_INTEL) {
        p.name = "intel-legacy";
        p.detail = "i915 does not expose drm-engine-dec; warnings come from the "
                   "internal miss/stall counters - monitor only";
        return p;
    }

    return p;  // unknown, and it says so
}

bool MachineProfile::byName(const std::string& name, MachineProfile& out) {
    // An explicit --vc-profile still autodetects first, so the log can report
    // the real hardware next to the profile the operator forced onto it.
    MachineProfile detected = detect();

    MachineProfile p;
    p.drmDevicePath = detected.drmDevicePath;
    p.pciVendor = detected.pciVendor;
    p.pciDevice = detected.pciDevice;
    p.ramTotalMb = detected.ramTotalMb;
    p.source = "flag";
    p.name = name;

    if (name == "fp530-picasso") {
        p.cap = 4;
        p.armed = true;
        p.detail = "forced by --vc-profile; measured boundary 4 concurrent "
                   "4K-class decode sessions";
    } else if (name == "fp530-8gb" || name == "fp530-16gb") {
        // Both accepted, and both mean the same thing now. They are the
        // pre-2026-09-04 names from when RAM was the discriminator: a host, a
        // runbook or a systemd drop-in still passing either must keep working,
        // and must not get a DIFFERENT cap depending on which one it happens to
        // say. Note this makes `--vc-profile fp530-16gb` ARM where it used to be
        // monitor-only. That was never a considered choice - it fell out of the
        // RAM split - and the supported way to disarm is --hang-guard=off.
        p.name = "fp530-picasso";
        p.cap = 4;
        p.armed = true;
        p.detail = std::string("forced by --vc-profile as '") + name +
                   "', a deprecated alias of fp530-picasso (the profile no "
                   "longer keys on RAM); measured boundary 4 concurrent "
                   "4K-class decode sessions";
    } else if (name == "4ktop-780m") {
        p.detail = "forced by --vc-profile; VCN 4.0 unified ring, no measured "
                   "boundary - monitor only";
    } else if (name == "intel-legacy") {
        p.detail = "forced by --vc-profile; monitor only";
    } else if (name == "unknown") {
        p.detail = "forced by --vc-profile; monitor only";
    } else {
        return false;
    }

    out = p;
    return true;
}

std::string MachineProfile::knownNames() {
    return "fp530-picasso, 4ktop-780m, intel-legacy, unknown "
           "(fp530-8gb and fp530-16gb accepted as deprecated aliases)";
}

} // namespace videocomposer
