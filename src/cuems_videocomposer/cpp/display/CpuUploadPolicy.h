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

#ifndef VIDEOCOMPOSER_CPUUPLOADPOLICY_H
#define VIDEOCOMPOSER_CPUUPLOADPOLICY_H

// The upload decision of OpenGLRenderer::renderLayer's CPU path (1amfwm7hg3u), without GL, so the unit
// target can test it. The renderer keeps only the GL calls.
//
// A layer's CPU frame is uploaded on the draw that first sees it (the frame chosen in this tick reaches the
// texture in this tick) and not again while its identity is unchanged: a second surface drawing the layer in
// the same tick, or a held frame on the next tick, reuses the texture. A failed map or unmap of the staging
// buffer uploads straight from the CPU buffer and is not recorded, so the next draw retries the PBO.

#include <cstdint>
#include <string>
#include <vector>

namespace videocomposer {
namespace cpuupload {

// What the texture would hold after an upload of this draw's frame. Every part is a generation: a counter
// that only moves forward, so a change in any of them is a different picture.
//  - sourceGeneration: LayerPlayback::setInputSource (a new load resets the frame counters).
//  - frameGeneration: LayerPlayback, every successful frame load (and, for a shared secondary, every frame
//    its driver caches): the media frame number is not used, because seek() moves it without loading pixels
//    and a shared secondary's buffer is a view of its driver's.
//  - preparedGeneration: LayerDisplay::prepareFrame, every write of its prepared (crop/panorama) buffer and
//    every switch from it back to the source buffer: a held frame whose panOffset changes is new pixels.
//  - textureGeneration: OpenGLRenderer, every (re)creation of the layer's texture.
struct Identity {
    uint64_t sourceGeneration = 0;
    uint64_t frameGeneration = 0;
    uint64_t preparedGeneration = 0;
    uint64_t textureGeneration = 0;

    bool operator==(const Identity& o) const {
        return sourceGeneration == o.sourceGeneration && frameGeneration == o.frameGeneration &&
               preparedGeneration == o.preparedGeneration && textureGeneration == o.textureGeneration;
    }
    bool operator!=(const Identity& o) const { return !(*this == o); }
};

// How the staging step of an upload went.
enum class Staging {
    NoPbo,        // the layer has no PBO (creation failed): the long-standing direct path
    Ok,           // orphaned, mapped, filled, unmapped: upload from the PBO
    MapFailed,    // glMapBufferRange returned NULL: nothing was copied
    UnmapFailed,  // glUnmapBuffer returned GL_FALSE: the store's contents are undefined
};

struct Decision {
    bool directFromCpu = false;  // glTexSubImage2D from the CPU buffer, GL_PIXEL_UNPACK_BUFFER unbound
    bool recorded = false;       // the identity is recorded: an equal draw skips the upload
    bool warn = false;           // emit the staging-failure WARNING now
    uint64_t suppressed = 0;     // failures not warned about since the previous WARNING (with warn)
};

// One staging-failure WARNING per layer per this interval.
constexpr int64_t kWarnIntervalNs = 10LL * 1000 * 1000 * 1000;

// One per layer.
class Policy {
public:
    // Does this draw upload? False only when the identity equals the one last recorded.
    bool needsUpload(const Identity& draw) const;

    // After the staging step of an upload of `draw`: which source glTexSubImage2D reads and whether the
    // identity is recorded. A failure (map/unmap) uploads directly, is not recorded and may warn (rate
    // limited by `nowNs`, a steady clock).
    Decision finish(Staging staging, const Identity& draw, int64_t nowNs);

    // Drop the recorded identity: the next draw uploads.
    void forget();

private:
    bool have_ = false;
    Identity last_;
    bool warned_ = false;
    int64_t lastWarnNs_ = 0;
    uint64_t suppressed_ = 0;
};

// VIDEOCOMPOSER_CADENCE_STATS, the same rule as CadenceStats::enabledFromEnv (set, not empty, not "0").
bool statsEnabledFromEnv();

// Upload cost per layer (AC3 (a)), only with VIDEOCOMPOSER_CADENCE_STATS=1: one line per layer every 60 s
// over the window since the previous line, and a final line when the layer's texture is dropped.
class Stats {
public:
    static constexpr int64_t kIntervalNs = 60LL * 1000 * 1000 * 1000;

    // One upload: `ms` from the PBO bind to the return of glTexSubImage2D; `direct` = from the CPU buffer.
    void add(double ms, bool direct, int width, int height, int64_t nowNs);
    // The periodic line when the window is >= kIntervalNs old; starts a new window.
    bool takeLine(int layerId, int64_t nowNs, std::string& line);
    // The line for what the current window holds (layer gone), if anything.
    bool takeFinalLine(int layerId, std::string& line);

private:
    std::string format(int layerId, bool isFinal) const;
    void reset();

    std::vector<double> ms_;
    uint64_t direct_ = 0;
    int width_ = 0;
    int height_ = 0;
    int64_t windowStartNs_ = 0;
    bool open_ = false;
};

}  // namespace cpuupload
}  // namespace videocomposer

#endif  // VIDEOCOMPOSER_CPUUPLOADPOLICY_H
