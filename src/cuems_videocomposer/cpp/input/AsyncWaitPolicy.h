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

#pragma once

namespace videocomposer {
namespace asyncwait {

/// How long the render thread may wait for an asynchronously decoded frame (VideoFileInput's VAAPI queue,
/// HAPVideoInput's async decoder). The wait runs ON the render thread, so it delays every output's next flip.
///
///  - No texture yet (cold start): `coldMs`, the decoder is spinning up and there is nothing to hold.
///  - First request of a frame with a texture to hold: 5 ms, a fraction of a vsync, as before.
///  - A retry of the frame that just missed (1amfwm7jme2): 0 ms. LayerPlayback re-requests a frame whose load held
///    the previous texture on every vsync until it is delivered; waiting on each retry would turn one lost frame into
///    a render stall on every output (the 1amfwm7gq3x symptom), so a retry only looks.
constexpr int kHeldWaitMs = 5;

constexpr int waitMs(bool textureValid, bool retryOfMissedFrame, int coldMs) {
    return !textureValid ? coldMs : (retryOfMissedFrame ? 0 : kHeldWaitMs);
}

} // namespace asyncwait
} // namespace videocomposer
