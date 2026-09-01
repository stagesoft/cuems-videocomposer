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

// The FP530 8 GB box reports ~7.6 GiB of MemTotal once the framebuffer carve-out
// is taken; the 16 GB variant reports ~15.5. Anything below this line is read as
// the 8 GB machine - the one profile with a measured cap.
constexpr long RAM_8GB_CEILING_MB = 12 * 1024;

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
        const bool is8gb = (p.ramTotalMb > 0 && p.ramTotalMb < RAM_8GB_CEILING_MB);
        if (is8gb) {
            p.name = "fp530-8gb";
            p.cap = 4;
            p.armed = true;
            p.detail = "measured boundary: 4 concurrent 4K-class decode sessions "
                       "clean, 5 and beyond in the hang region";
        } else {
            p.name = "fp530-16gb";
            p.cap = 0;
            p.armed = false;
            p.detail = "same GPU as fp530-8gb but the boundary has not been "
                       "measured with this much RAM - monitor only";
        }
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

    if (name == "fp530-8gb") {
        p.cap = 4;
        p.armed = true;
        p.detail = "forced by --vc-profile; measured boundary 4 concurrent "
                   "4K-class decode sessions";
    } else if (name == "fp530-16gb") {
        p.detail = "forced by --vc-profile; no measured boundary - monitor only";
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
    return "fp530-8gb, fp530-16gb, 4ktop-780m, intel-legacy, unknown";
}

} // namespace videocomposer
