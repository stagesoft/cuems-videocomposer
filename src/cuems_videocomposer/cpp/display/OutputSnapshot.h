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

#ifndef VIDEOCOMPOSER_OUTPUTSNAPSHOT_H
#define VIDEOCOMPOSER_OUTPUTSNAPSHOT_H

#include <cstdint>
#include <string>
#include <vector>

namespace videocomposer {

/**
 * OutputSnapshot - test aid: write what each physical output actually shows,
 * read back AFTER the blit (edge blend, warp, per-output colour applied) and
 * before the swap, as a binary PPM per output.
 *
 * Off unless VIDEOCOMPOSER_SNAPSHOT_DIR names a directory. Then SIGUSR2 asks
 * for one round: every enabled output writes <dir>/<output>-<round>.ppm at its
 * next blit, and one INFO line names each file. The readback and the write are
 * synchronous on the render thread: measured on test2 node01 (N97, 2x4K + 1080p)
 * a round stalls the loop ~50-90 ms and the MTC smooth clock snaps once
 * ("anti-drift SNAP errorMs=51..91"). Rigs and test builds only, never shows;
 * an async PBO readback with the write on another thread would remove it.
 *
 * Exists for the keep-working matrix (K5/K6) of cuems-RELATIONS
 * Plans/2026-10-21-vc-multi-output-programme.md, phase 0.5: output/capture
 * reads the canvas before the blit and cannot see blend, warp or colour.
 */
class OutputSnapshot {
public:
    /// Read the env, install the SIGUSR2 handler. Idempotent.
    static void installFromEnv();
    static bool enabled();

    /// True while a requested round still has outputs to capture.
    static bool wanted(const std::string& outputName);

    /// Read back the bound draw framebuffer (w x h) and write the PPM.
    static void capture(const std::string& outputName, int width, int height);

    /// Called after a blit pass: closes the round once 'enabledOutputs'
    /// outputs have been captured.
    static void endPass(size_t enabledOutputs);

    /// Pure helper (tested): RGBA rows bottom-up (GL) -> PPM bytes top-down.
    static std::vector<uint8_t> encodePpm(const std::vector<uint8_t>& rgba, int width, int height);

    /// Test hook: request a round without a signal.
    static void requestForTest();
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_OUTPUTSNAPSHOT_H
