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

#ifndef VIDEOCOMPOSER_CONTRACTTYPES_H
#define VIDEOCOMPOSER_CONTRACTTYPES_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace videocomposer {

/**
 * Plain data the videocomposer reports on the hardware-configuration contract
 * (docs/contract-v1.md). Filled by the display backend on the render thread,
 * copied under a short lock, serialised to JSON on the contract's own threads.
 */
struct ContractModeTiming {
    uint32_t clockKhz = 0;
    uint32_t hdisplay = 0, hsyncStart = 0, hsyncEnd = 0, htotal = 0;
    uint32_t vdisplay = 0, vsyncStart = 0, vsyncEnd = 0, vtotal = 0;
    uint32_t flags = 0;   // DRM_MODE_FLAG_*
    uint32_t type = 0;    // DRM_MODE_TYPE_*
};

struct ContractMode {
    std::string modeId;       // unique per connector (assignModeIds)
    int width = 0;
    int height = 0;
    double refreshHz = 0.0;   // frame rate: clock / (htotal * vtotal); 1080i60 -> 30.00
    double fieldRateHz = 0.0; // field rate: 60.00 for 1080i60, = refreshHz when progressive
    bool interlaced = false;
    bool preferred = false;
    ContractModeTiming timing;

    /// Build from raw timing (the DRM mode fields).
    static ContractMode fromTiming(const ContractModeTiming& t);
};

struct ContractOutputHardware {
    std::string connector;            // "HDMI-A-1"
    std::string connection;           // "connected" | "disconnected" | "unknown"
    std::string make, model, serial;  // EDID, empty when unknown
    int physicalWidthMm = 0;
    int physicalHeightMm = 0;
    bool hasCurrent = false;          // false: the VC drives no surface on it
    ContractMode current;
    std::vector<ContractMode> modes;
};

/// Per-output color as the backend holds it (programme phase 3, plan 2.7): plain
/// copies of OutputColor's fields, so no GL object and no shared_ptr crosses into
/// the contract threads. Identity values when nothing is configured.
struct ContractOutputColor {
    float brightness = 0.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float hue = 0.0f;
    float gamma = 1.0f;
    std::string lut;                  // resolved path; empty = none configured (JSON null)
    std::string lutStatus = "none";   // "none" | "ok" | "missing" | "invalid"
    bool applied = false;             // color program linked and this output's color draw not failed
};

struct ContractOutputState {
    std::string connector;
    bool enabled = false;
    int x = 0, y = 0, width = 0, height = 0;   // canvas region
    float blendLeft = 0, blendRight = 0, blendTop = 0, blendBottom = 0, blendGamma = 2.2f;
    std::string modeId;               // current mode id, empty when not driven
    ContractOutputColor color;
};

struct ContractState {
    std::string pacing;               // "coupled" | "per_surface"
    int canvasWidth = 0;
    int canvasHeight = 0;
    std::string configSource;         // file the configuration came from
    std::vector<ContractOutputState> outputs;
};

struct ContractSnapshot {
    std::vector<ContractOutputHardware> hardware;
    ContractState state;
};

/// Give every mode of one connector a stable id "<w>x<h>@<hz 2 dec><p|i>",
/// suffixed "/<clock_khz>" when two modes would otherwise collide, and an
/// ordinal "#n" if even that collides. Also used for the current mode.
void assignModeIds(std::vector<ContractMode>& modes);
std::string baseModeId(const ContractMode& m);

/// The id the contract exposes by default: "<w>x<h>@<hz>p|i" with hz the rate
/// people and tools quote -- the FIELD rate for interlaced modes (1080i60 ->
/// "1920x1080@60.00i"), matching modetest and display.conf refresh=60i.
std::string contractModeId(const ContractMode& m);

/// Thread-safe holder: the backend publishes, the contract threads read copies.
class ContractSnapshotHolder {
public:
    void set(ContractSnapshot snapshot) {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = std::move(snapshot);
        ++generation_;
    }
    ContractSnapshot get(uint64_t* generation = nullptr) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation) {
            *generation = generation_;
        }
        return snapshot_;
    }
private:
    mutable std::mutex mutex_;
    ContractSnapshot snapshot_;
    uint64_t generation_ = 0;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_CONTRACTTYPES_H
