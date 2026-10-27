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

#ifndef VIDEOCOMPOSER_OUTPUTCOLORCOMMANDS_H
#define VIDEOCOMPOSER_OUTPUTCOLORCOMMANDS_H

// /videocomposer/display/color (programme phase 3, plan 2.6) and the output_color
// lines of output/list (plan 2.8).
//
//   display/color <connector> brightness|contrast|saturation|hue|gamma <value>
//   display/color <connector> lut <name.cube> | none
//   display/color <connector> reset
//   display/color <connector> dump
//   display/color dump
//
// Kept out of RemoteCommandRouter: it needs only a DisplayBackend, so the tests
// drive it with a fake backend and a temporary lock path, without GL. Every
// mutating form is refused while the show lock exists (display/ShowLock.h); dump
// is read-only and always answers. Strings from the wire are logged only through
// ndiSanitizeForLog. Each change logs the output's paste-ready display.conf line:
// an OSC change is volatile (the VC cannot write /etc/cuems).

#include "../display/CubeLut.h"
#include "../display/OutputColor.h"

#include <string>
#include <vector>

namespace videocomposer {

class DisplayBackend;

/**
 * The display/color handler. `lutDir` is where bare LUT names resolve (tests pass
 * a temporary directory). Returns true when the change was applied (or the dump
 * answered), false otherwise; every outcome has its own journal line.
 */
bool handleDisplayColorCommand(DisplayBackend* backend, const std::vector<std::string>& args,
                               const std::string& lutDir = kLutDirectory);

/**
 * One line per output for output/list, after its region lines:
 *   output_color HDMI-A-2 color_contrast=1.08 color_lut=p2.cube lut_status=ok applied=yes
 * Never begins with "region" (the NDI preview tool parses ^region ...$). Empty on a
 * backend without per-output color.
 */
std::vector<std::string> outputColorListLines(const DisplayBackend& backend);

/** The output/list line of one output (sanitised). */
std::string outputColorListLine(const OutputColorReport& report);

/**
 * The line logged after every successful change:
 *   OutputColor: HDMI-A-2 color_contrast=1.08 color_lut=p2.cube (applied live — volatile:
 *   paste into /etc/cuems/display.conf to keep it)
 */
std::string outputColorPasteReadyLine(const std::string& connector, const OutputColor& color);

/** The journal line for a backend status other than Applied (sanitised). */
std::string outputColorFailureLine(OutputColorStatus status, const std::string& connector,
                                   const std::string& detail = std::string());

/** Whether outputColorFailureLine(status) is logged as an ERROR (else a WARNING). */
bool outputColorFailureIsError(OutputColorStatus status);

}  // namespace videocomposer

#endif  // VIDEOCOMPOSER_OUTPUTCOLORCOMMANDS_H
