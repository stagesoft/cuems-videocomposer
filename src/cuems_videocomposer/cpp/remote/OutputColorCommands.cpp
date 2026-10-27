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

#include "OutputColorCommands.h"
#include "../display/DisplayBackend.h"
#include "../display/OutputColorConfig.h"
#include "../display/ShowLock.h"
#include "../input/NdiExtraIps.h"
#include "../utils/Logger.h"

#include <cmath>
#include <sstream>

namespace videocomposer {

namespace {

// Connector and parameter names are short; a LUT name keeps up to NAME_MAX and a
// resolved path up to PATH_MAX, so a paste-ready line is never cut short.
constexpr size_t kLutNameLogMax = 255;
constexpr size_t kLutPathLogMax = 4096;

std::string safe(const std::string& s) { return ndiSanitizeForLog(s); }
std::string safeLutName(const std::string& s) { return ndiSanitizeForLog(s, kLutNameLogMax); }
std::string safeLutPath(const std::string& s) { return ndiSanitizeForLog(s, kLutPathLogMax); }

// The color with its LUT strings made safe to log: an OSC LUT name is wire text
OutputColor loggable(const OutputColor& color) {
    OutputColor c = color;
    c.lutPath = safeLutPath(c.lutPath);
    c.lutValue = safeLutName(c.lutValue);
    return c;
}

std::string keysOrIdentity(const OutputColor& color) {
    const std::string keys = colorConfigLine(loggable(color));
    return keys.empty() ? std::string("identity, no color_* keys") : keys;
}

void logUsage() {
    LOG_WARNING << "display/color requires: <connector> brightness|contrast|saturation|hue|gamma"
                << " <value> | <connector> lut <name.cube>|none | <connector> reset"
                << " | <connector> dump | dump";
}

void logFailure(OutputColorStatus status, const std::string& connector,
                const std::string& detail = std::string()) {
    if (outputColorFailureIsError(status)) {
        LOG_ERROR << outputColorFailureLine(status, connector, detail);
    } else {
        LOG_WARNING << outputColorFailureLine(status, connector, detail);
    }
}

// "HDMI-A-1, HDMI-A-2" for the Unknown output line
std::string outputNames(const std::vector<OutputColorReport>& reports) {
    std::string names;
    for (const auto& r : reports) {
        if (!names.empty()) names += ", ";
        names += safe(r.name);
    }
    return names.empty() ? std::string("none") : names;
}

// The backend's current color of one output. On failure the line is already logged.
bool currentColor(DisplayBackend& backend, const std::string& connector, OutputColorReport& out) {
    std::vector<OutputColorReport> reports;
    const OutputColorStatus status = backend.getOutputColors(reports);
    if (status != OutputColorStatus::Applied) {
        logFailure(status, connector);
        return false;
    }
    for (const auto& r : reports) {
        if (r.name == connector) {
            out = r;
            return true;
        }
    }
    logFailure(OutputColorStatus::UnknownOutput, connector, "outputs: " + outputNames(reports));
    return false;
}

// After a backend call: Applied logs the paste-ready line of what the backend now
// holds; anything else logs its own line (with what was stored when the color
// program is unavailable, and the known outputs for an unknown one).
bool finish(DisplayBackend& backend, const std::string& connector, OutputColorStatus status) {
    if (status != OutputColorStatus::Applied) {
        std::string detail;
        if (status == OutputColorStatus::ProgramUnavailable ||
            status == OutputColorStatus::UnknownOutput) {
            std::vector<OutputColorReport> reports;
            backend.getOutputColors(reports);
            if (status == OutputColorStatus::UnknownOutput) {
                detail = "outputs: " + outputNames(reports);
            }
            for (const auto& r : reports) {
                if (r.name == connector) detail = keysOrIdentity(r.color);
            }
        }
        logFailure(status, connector, detail);
        return false;
    }
    std::vector<OutputColorReport> reports;
    backend.getOutputColors(reports);
    for (const auto& r : reports) {
        if (r.name == connector) {
            LOG_INFO << outputColorPasteReadyLine(r.name, r.color);
            return true;
        }
    }
    return true;
}

bool dumpColors(DisplayBackend* backend, const std::string* connector) {
    if (!backend) {
        LOG_WARNING << "display/color: No display backend available";
        return false;
    }
    std::vector<OutputColorReport> reports;
    const OutputColorStatus status = backend->getOutputColors(reports);
    if (status != OutputColorStatus::Applied) {
        logFailure(status, connector ? *connector : std::string());
        return false;
    }

    bool found = false;
    for (const auto& r : reports) {
        if (connector && r.name != *connector) {
            continue;
        }
        found = true;
        const std::string name = safe(r.name);
        LOG_INFO << "OutputColor: " << name << " " << keysOrIdentity(r.color)
                 << " (dump: lut_status=" << lutStatusName(r.color.lutStatus)
                 << " applied=" << (r.applied ? "yes" : "no") << ")";
        // A failure is said again on every dump (plan 2.9), not only at start
        if (r.color.lutStatus == LutStatus::Missing || r.color.lutStatus == LutStatus::Invalid) {
            LOG_ERROR << "OutputColor: " << name << " color_lut=" << loggable(r.color).lutValue
                      << " NOT APPLIED: lut_status=" << lutStatusName(r.color.lutStatus)
                      << " — output runs with trims only";
        }
        if (!r.applied && (r.color.trimsActive() || r.color.lutConfigured())) {
            LOG_ERROR << "OutputColor: " << name << " color configured but not on screen"
                      << " (the color program is unavailable or its first draw failed)"
                      << " — running without color";
        }
    }
    if (connector && !found) {
        logFailure(OutputColorStatus::UnknownOutput, *connector, "outputs: " + outputNames(reports));
        return false;
    }
    return true;
}

bool setLut(DisplayBackend& backend, const std::string& connector, const std::string& name,
            const std::string& lutDir) {
    if (name == "none") {
        return finish(backend, connector, backend.clearOutputLut(connector));
    }

    // Bare names only: the OSC port is unauthenticated UDP and never takes a path
    std::string resolved, error;
    if (!resolveOscLutName(name, resolved, error, lutDir)) {
        LOG_WARNING << "display/color: " << safe(connector) << " lut '" << safeLutName(name)
                    << "' REFUSED: " << error << " — nothing changed";
        return false;
    }

    OutputColorReport current;
    if (!currentColor(backend, connector, current)) {
        return false;
    }

    OutputLutLoadInfo info;
    const OutputColorStatus status = backend.configureOutputLut(connector, resolved, name, info);
    if (status == OutputColorStatus::LutFailed) {
        std::string kept = " — no previous LUT, the output keeps its trims";
        if (current.color.lutConfigured()) {
            kept = " — previous LUT " + loggable(current.color).lutValue + " kept";
        }
        logFailure(status, current.name,
                   "color_lut=" + safeLutName(name) + " NOT APPLIED: " + safeLutPath(info.error) + kept);
        return false;
    }
    if (status == OutputColorStatus::Applied) {
        LOG_INFO << "OutputColor: " << safe(current.name) << " color_lut=" << safeLutName(name)
                 << " loaded (ok, " << info.size << "^3, parsed in " << std::lround(info.parseMs)
                 << " ms)";
        if (info.clampedValues > 0) {
            LOG_WARNING << "OutputColor: " << safe(current.name) << " color_lut=" << safeLutName(name)
                        << ": " << info.clampedValues << " data value(s) outside [0, 1] clamped"
                        << " (an HDR-authored LUT? the outputs are SDR)";
        }
    }
    return finish(backend, connector, status);
}

bool setTrim(DisplayBackend& backend, const std::string& connector, const ColorTrimSpec& spec,
             const std::string& text) {
    float value = 0.0f;
    if (!parseFiniteFloat(text, value)) {
        logFailure(OutputColorStatus::BadValue, connector,
                   std::string(spec.name) + " '" + safe(text) + "' is not a finite number");
        return false;
    }

    OutputColorReport current;
    if (!currentColor(backend, connector, current)) {
        return false;
    }

    OutputColor next = current.color;
    if (!next.set(spec.trim, value)) {
        LOG_WARNING << "display/color: " << safe(connector) << " " << spec.name << "=" << safe(text)
                    << " out of range (" << formatColorNumber(spec.min) << " to "
                    << formatColorNumber(spec.max) << ") — using "
                    << formatColorNumber(next.get(spec.trim));
    }
    return finish(backend, connector, backend.configureOutputColor(connector, next));
}

}  // namespace

bool handleDisplayColorCommand(DisplayBackend* backend, const std::vector<std::string>& args,
                               const std::string& lutDir) {
    // Read-only forms first: dump answers under the show lock
    if (args.size() == 1 && args[0] == "dump") {
        return dumpColors(backend, nullptr);
    }
    if (args.size() == 2 && args[1] == "dump") {
        return dumpColors(backend, &args[0]);
    }

    // Every other form changes an output: refused while a project is loaded
    if (refuseUnderShowLock("display/color")) {
        return false;
    }

    if (args.size() < 2) {
        logUsage();
        return false;
    }
    if (!backend) {
        LOG_WARNING << "display/color: No display backend available";
        return false;
    }

    const std::string& connector = args[0];
    const std::string& param = args[1];

    if (param == "reset") {
        if (args.size() != 2) {
            logUsage();
            return false;
        }
        return finish(*backend, connector, backend->resetOutputColor(connector));
    }
    if (param == "lut") {
        if (args.size() != 3) {
            logUsage();
            return false;
        }
        return setLut(*backend, connector, args[2], lutDir);
    }
    const ColorTrimSpec* spec = findColorTrim(param);
    if (!spec) {
        LOG_WARNING << "display/color: unknown parameter '" << safe(param) << "' — use brightness,"
                    << " contrast, saturation, hue, gamma, lut, reset or dump";
        return false;
    }
    if (args.size() != 3) {
        logUsage();
        return false;
    }
    return setTrim(*backend, connector, *spec, args[2]);
}

std::vector<std::string> outputColorListLines(const DisplayBackend& backend) {
    std::vector<std::string> lines;
    std::vector<OutputColorReport> reports;
    if (backend.getOutputColors(reports) != OutputColorStatus::Applied) {
        return lines;   // no per-output color on this backend: nothing to list
    }
    for (const auto& r : reports) {
        lines.push_back(outputColorListLine(r));
    }
    return lines;
}

std::string outputColorListLine(const OutputColorReport& report) {
    std::string line = "output_color " + safe(report.name);
    const std::string keys = colorConfigLine(loggable(report.color));
    if (!keys.empty()) {
        line += ' ';
        line += keys;
    }
    line += " lut_status=";
    line += lutStatusName(report.color.lutStatus);
    line += report.applied ? " applied=yes" : " applied=no";
    return line;
}

std::string outputColorPasteReadyLine(const std::string& connector, const OutputColor& color) {
    const std::string keys = colorConfigLine(loggable(color));
    if (keys.empty()) {
        return "OutputColor: " + safe(connector) + " identity, no color_* keys (applied live —"
               " volatile: remove this output's color_* keys from /etc/cuems/display.conf to keep it)";
    }
    return "OutputColor: " + safe(connector) + " " + keys +
           " (applied live — volatile: paste into /etc/cuems/display.conf to keep it)";
}

std::string outputColorFailureLine(OutputColorStatus status, const std::string& connector,
                                   const std::string& detail) {
    const std::string name = safe(connector);
    switch (status) {
        case OutputColorStatus::Applied:
            return std::string();
        case OutputColorStatus::UnknownOutput:
            return "display/color: Unknown output '" + name + "'" +
                   (detail.empty() ? std::string() : " (" + detail + ")");
        case OutputColorStatus::Unsupported:
            return "display/color: per-output color is not supported by this display backend";
        case OutputColorStatus::BadValue:
            return "display/color: " + name + " " + (detail.empty() ? std::string("bad value") : detail) +
                   " — nothing changed";
        case OutputColorStatus::LutFailed:
            return "OutputColor: " + name + " " +
                   (detail.empty() ? std::string("color_lut NOT APPLIED") : detail);
        case OutputColorStatus::ProgramUnavailable:
            return "OutputColor: " + name + " color configured but the color program is unavailable"
                   " — running without color" + (detail.empty() ? std::string() : " (stored: " + detail + ")");
    }
    return std::string();
}

bool outputColorFailureIsError(OutputColorStatus status) {
    switch (status) {
        case OutputColorStatus::Unsupported:
        case OutputColorStatus::LutFailed:
        case OutputColorStatus::ProgramUnavailable:
            return true;
        case OutputColorStatus::Applied:
        case OutputColorStatus::UnknownOutput:
        case OutputColorStatus::BadValue:
            return false;
    }
    return true;
}

}  // namespace videocomposer
