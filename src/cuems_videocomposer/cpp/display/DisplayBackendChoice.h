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


/**
 * @file DisplayBackendChoice.h
 * @brief Which display backend the videocomposer starts on (1amfwm7hjze).
 *
 * cuems-RELATIONS Plans/2026-11-11-vc-drm-default-backend-1amfwm7hjze.md. DRM/KMS is the backend: it is the one with
 * per-output colour, the vblank cadence engine and the tests. X11 and Wayland are not maintained and not for
 * production, so they run only when --display-backend=x11|wayland asks for them. DISPLAY and WAYLAND_DISPLAY no
 * longer choose anything: a desktop session or an env file leaking either variable into the service used to switch a
 * show box onto an untested backend with one INFO line as the only trace.
 */

#ifndef VIDEOCOMPOSER_DISPLAYBACKENDCHOICE_H
#define VIDEOCOMPOSER_DISPLAYBACKENDCHOICE_H

#include <functional>
#include <string>

namespace videocomposer {
namespace displaybackend {

enum class Kind { Drm, X11, Wayland };

/** "drm", "x11", "wayland". */
const char* name(Kind kind);

/** What the parser and the errors list: "drm, x11, wayland". There is no "auto" (D1). */
const char* acceptedValues();

/** Case-insensitive exact match of drm/x11/wayland. Anything else (empty, "auto", "foo") is false. */
bool parse(const std::string& value, Kind& out);

/**
 * The backend to start on, from the stored option ("" = not given). Never reads the environment.
 * Not given -> Drm. False only for a non-empty value parse() rejects (the parser stops those first; a config file
 * line can still carry one).
 */
bool select(const std::string& option, Kind& out);

/**
 * An explicitly chosen desktop backend: one attempt, no fallback to the other desktop backend and none to DRM (D2:
 * the start fails, Restart=on-failure makes the unit loud). open(kind) builds the backend and its window; on false
 * this logs the one ERROR naming the backend and returns false.
 */
bool openExplicitDesktop(Kind kind, const std::function<bool(Kind)>& open);

} // namespace displaybackend
} // namespace videocomposer

#endif // VIDEOCOMPOSER_DISPLAYBACKENDCHOICE_H
