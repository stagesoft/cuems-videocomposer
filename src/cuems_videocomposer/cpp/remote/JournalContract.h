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

#ifndef VIDEOCOMPOSER_JOURNALCONTRACT_H
#define VIDEOCOMPOSER_JOURNALCONTRACT_H

// Log lines that another process parses out of the journal.
//
// The videocomposer has no OSC reply channel: cuems-power-bridge's NDI
// preview tool (cuems-ndi-preview, /ndi/*) learns what happened to a request
// by reading these lines back from the journal. Their wording is therefore a
// cross-process contract with no version number. Every log site that emits
// one of them builds it here, and TestJournalContract pins the exact text.
// Changing a string below breaks the bridge: change both sides together.
//
// Header-only, no NDI SDK, no display: it must build in CI.

#include <sstream>
#include <string>
#include <vector>

namespace videocomposer {
namespace journal {

// --- startup ---------------------------------------------------------------

inline std::string starting(const std::string& debVersion) {
    return "cuems-videocomposer " + debVersion + " starting";
}

inline const char* workerThreadRunning() {
    return "AsyncVideoLoader: Worker thread running";
}

// --- async load outcome (terminal lines for one load request) -----------

inline std::string asyncLoadComplete(const std::string& filepath, const std::string& cueId) {
    return "Async load complete: " + filepath + " (cue ID: " + cueId + ")";
}

inline std::string asyncLoadFailed(const std::string& filepath, const std::string& cueId) {
    return "Async load failed for: " + filepath + " (cue ID: " + cueId + ")";
}

inline std::string layerNoLongerExists(const std::string& cueId) {
    return "Layer no longer exists for cue ID: " + cueId;
}

inline std::string skippingCancelledLoad(const std::string& cueId) {
    return "AsyncVideoLoader: Skipping cancelled load for cue: " + cueId;
}

inline std::string discardingCancelledResult(const std::string& cueId) {
    return "AsyncVideoLoader: Discarding result for cancelled cue: " + cueId;
}

inline const char* resetAll() {
    return "Reset: removing all layers, cancelling loads, resetting master";
}

// --- NDI input -------------------------------------------------------------

inline std::string ndiConnected(const std::string& sourceName) {
    return "NDI: Connected to source: " + sourceName;
}

inline std::string ndiSourceNotFound(const std::string& sourceName) {
    return "NDI: Source not found: " + sourceName;
}

inline std::string ndiSourceFormat(int width, int height, double fps) {
    std::ostringstream os;
    os << "NDI: Source format: " << width << "x" << height << " @ " << fps << " fps (BGRA)";
    return os.str();
}

inline const char* ndiNoVideoFrame() {
    return "NDI: No video frame received, using defaults (1920x1080 @25fps)";
}

// The first real frame after ndiNoVideoFrame(): the format the layer was
// placed with was invented, the source's real one is known from now on.
inline std::string ndiSourceFormatUpdated(int width, int height, double fps) {
    std::ostringstream os;
    os << "NDI: Source format updated " << width << "x" << height << " @ " << fps
       << " fps (was invented)";
    return os.str();
}

// A source that changes its format mid-stream (resolution switch in OBS).
inline std::string ndiSourceFormatChanged(int width, int height, double fps) {
    std::ostringstream os;
    os << "NDI: Source format changed " << width << "x" << height << " @ " << fps << " fps";
    return os.str();
}

// --- NDI discovery (/videocomposer/ndi/discover) --------------------------
// Parsers anchor on the "NDI discover: " prefix, never on a bare "NDI:":
// other lines (output/list's "NDI: Not configured") also start with "NDI:".

inline const char* ndiDiscoverPrefix() { return "NDI discover: "; }

inline std::string ndiDiscoverStarted(int seconds) {
    return "NDI discover: started (" + std::to_string(seconds) + " s)";
}

// One line per source. Names may contain " @ "; addresses never do, so a
// parser splits on the LAST " @ ".
inline std::string ndiDiscoverSource(const std::string& name, const std::string& address) {
    return "NDI discover: " + name + " @ " + (address.empty() ? std::string("-") : address);
}

inline std::string ndiDiscoverDone(size_t count) {
    return "NDI discover: done (" + std::to_string(count) + ")";
}

inline const char* ndiDiscoverBusy() {
    return "NDI discover: busy";
}

inline const char* ndiDiscoverUnavailable() {
    return "NDI discover: unavailable (built without the NDI SDK)";
}

// --- geometry (output/list, layer/<id>/fit_output) ------------------------

inline std::string region(const std::string& name, int x, int y, int w, int h) {
    return "region " + name + " " + std::to_string(x) + "," + std::to_string(y) + "," +
           std::to_string(w) + "," + std::to_string(h);
}

// basis: "dims WxH", "fallback" (no input yet) or "invented" (source format
// not known yet, e.g. an NDI source that has sent no frame).
inline std::string fitOutput(const std::string& layerId, const std::string& regionName,
                             const std::string& mode, int x, int y, double sx, double sy,
                             const std::string& basis) {
    std::ostringstream os;
    os << "fit_output: " << layerId << " -> " << regionName << " " << mode << " pos " << x << ","
       << y << " scale " << sx << "," << sy << " (" << basis << ")";
    return os.str();
}

inline std::string fitOutputUnknown(const std::string& name, const std::vector<std::string>& have) {
    std::string list;
    for (size_t i = 0; i < have.size(); ++i) {
        if (i) list += ", ";
        list += have[i];
    }
    return "fit_output: unknown output '" + name + "' (have: " + list + ")";
}

}  // namespace journal
}  // namespace videocomposer

#endif  // VIDEOCOMPOSER_JOURNALCONTRACT_H
