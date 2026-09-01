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

#ifndef VIDEOCOMPOSER_EXITREPORTER_H
#define VIDEOCOMPOSER_EXITREPORTER_H

#include <string>

namespace videocomposer {

/**
 * ExitReporter - say why the process is gone, on the paths where nobody else will.
 *
 * The compositor can vanish without a word. Under 869en65tm the sequence is a
 * kernel `ring vcn_dec timeout` -> `GPU reset begin!`, after which Mesa's amdgpu
 * winsys finds its command stream rejected on a non-robust context and calls
 * `exit(1)` from inside a driver call. No exception, no error return, no line of
 * ours: the journal shows the kernel's complaint, then systemd reporting status
 * 1/FAILURE, with nothing from the program in between. From the engine's side the
 * layer simply stops.
 *
 * `exit()` runs `atexit` handlers, which is the whole opening this uses. What the
 * handler cannot do is *fix* anything -- the F0 investigation established that
 * in-process recovery from a GPU reset is unreachable here (the rejected context
 * is Mesa's own VA-API one, created with flags 0; even an
 * EGL_EXT_create_context_robustness EGL context does not change the outcome). So
 * the goal is strictly to leave a record with enough state to tell this death
 * apart from the others.
 *
 * Reporting policy, deliberately narrow:
 *
 *   - Nothing at all before the main loop starts. `--help`, `--version` and
 *     `--discover-ndi` are ordinary CLI runs and stay silent.
 *   - Orderly stop (main loop exited and teardown finished, exit code 0) --
 *     one INFO line.
 *   - Anything else after the loop started -- one ERROR line carrying the decoder
 *     census and the GPU's own memory accounting, because that is the pair that
 *     separates the eviction-storm hang from every other way to die.
 *
 * SIGTERM is deliberately NOT handled. Today it kills the process outright, which
 * is how `systemctl stop|restart` works during a show turnaround; installing a
 * graceful handler would make a stop wait on the render loop and, if that loop is
 * wedged, hold systemd for TimeoutStopSec. That trade belongs to whoever changes
 * shutdown semantics, not to a reporter. The consequence is intentional and
 * harmless: a systemd stop produces no record, exactly as it does today.
 *
 * Fatal signals (SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT) do NOT run atexit handlers,
 * so they get their own handler, written under async-signal-safety rules: `write`
 * to stderr only, no syslog, no iostreams, no allocation. It re-raises with the
 * default disposition afterwards so the exit status and any core dump are what
 * they would have been.
 */
namespace exitreport {

/**
 * Arm the reporter. Idempotent; safe to call before anything spawns.
 *
 * Call it after the logging singletons exist. `atexit` handlers and static
 * destructors run in reverse registration order, so a logger constructed before
 * this call is guaranteed to outlive the handler that wants to use it.
 */
void install();

/**
 * The main loop is about to start. Before this, a death is a CLI exit and says
 * nothing; after it, a death is worth a record.
 */
void markRunning();

/**
 * The main loop exited on its own terms and teardown completed. Turns the
 * closing record from ERROR into INFO.
 */
void markCleanShutdown();

/**
 * Live hardware-decoder census.
 *
 * `surfaces` is the pool size the decoder asked for in frames, not an estimate in
 * megabytes: F0 published `est_pool_mb` figures that assumed a 17-frame DPB and
 * overstated the real allocation, and the correction was to trust amdgpu's own
 * accounting instead. The record therefore carries the surface *count* we
 * requested next to the VRAM the driver actually reports.
 */
void decoderOpened(int surfaces);
void decoderClosed(int surfaces);

/** A decode error was observed anywhere in the process (see AsyncDecodeQueue). */
void decodeErrorObserved(int averr);

/**
 * The census and GPU accounting as one line, for callers that want it outside an
 * exit path (tests, and anything that wants to state the same facts).
 */
std::string censusLine();

}  // namespace exitreport
}  // namespace videocomposer

#endif  // VIDEOCOMPOSER_EXITREPORTER_H
