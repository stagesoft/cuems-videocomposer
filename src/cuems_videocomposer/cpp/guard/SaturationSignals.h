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

#ifndef VIDEOCOMPOSER_SATURATION_SIGNALS_H
#define VIDEOCOMPOSER_SATURATION_SIGNALS_H

#include <cstdint>

namespace videocomposer {

/**
 * SaturationSignals - what the render and decode paths tell the monitor.
 *
 * ## The rule this exists to obey
 *
 * The saturation monitor must never reach *into* a layer, an input source or a
 * decode queue. The recovery worker holds the queue-access gate across its
 * whole {1000, 2000, 4000} ms attempt ladder, so a monitor that took that lock
 * would stall for up to ~7 s behind exactly the event it is trying to
 * describe, and one that used try_lock would go blind during precisely that
 * burst. So every signal is published *outward* by its producer into a plain
 * atomic here, and the monitor only ever reads these.
 *
 * That also removes the need for a per-frame layer walk: an event increments a
 * counter on the thread that observed it, which costs one relaxed atomic add
 * on a path that was already doing work, and nothing at all on the quiet path.
 *
 * ## Why the miss counter is not the existing pacing warning
 *
 * The pacing warning in LayerPlayback sits in the branch where a frame *was*
 * loaded, so a wedged layer - the case that matters - never reaches it and
 * would emit zero events. The counters here are incremented on the branches
 * where a frame was wanted and did not arrive, which is the signal an operator
 * actually suffers, and it works identically on Intel, where the GPU reports
 * no decode occupancy at all.
 */
namespace signals {

/** A layer wanted a frame this vsync and the decoder had none. */
void frameMissed();

/** A displayed frame was held well past its due time (pacing slip). */
void frameHeldLong();

/**
 * A decode error was observed on one layer.
 *
 * `layerKey` identifies the source so the monitor can tell one sick file from
 * a platform-wide event: a burst on two or more layers at once is a property
 * of the GPU, not of any clip. Errors on a single layer stay the existing
 * per-layer recovery story and never escalate here.
 */
void decodeErrorOnLayer(uint64_t layerKey);

/** Totals since start. */
long framesMissed();
long framesHeldLong();

/** Distinct layers that reported a decode error within the last `windowMs`. */
int layersErroringWithin(long windowMs);

/** Test seam: forget every recorded event. */
void resetForTest();

} // namespace signals

} // namespace videocomposer

#endif // VIDEOCOMPOSER_SATURATION_SIGNALS_H
