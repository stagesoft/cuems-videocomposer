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

#include "ContractProtocol.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <system_error>

namespace videocomposer {

using json = nlohmann::json;

namespace {

double round2(double v) {
    return static_cast<double>(static_cast<long long>(v * 100.0 + (v >= 0 ? 0.5 : -0.5))) / 100.0;
}

json modeJson(const ContractMode& m) {
    const auto& t = m.timing;
    return json{
        {"mode_id", m.modeId},
        {"width", m.width},
        {"height", m.height},
        {"refresh_hz", round2(m.refreshHz)},
        {"field_rate_hz", round2(m.fieldRateHz)},
        {"interlaced", m.interlaced},
        {"preferred", m.preferred},
        {"timing", json{
            {"clock_khz", t.clockKhz},
            {"hdisplay", t.hdisplay}, {"hsync_start", t.hsyncStart},
            {"hsync_end", t.hsyncEnd}, {"htotal", t.htotal},
            {"vdisplay", t.vdisplay}, {"vsync_start", t.vsyncStart},
            {"vsync_end", t.vsyncEnd}, {"vtotal", t.vtotal},
            {"flags", t.flags}, {"type", t.type},
        }},
    };
}

json hardwareDoc(const ContractSnapshot& s) {
    json outputs = json::array();
    for (const auto& o : s.hardware) {
        json modes = json::array();
        for (const auto& m : o.modes) {
            modes.push_back(modeJson(m));
        }
        outputs.push_back(json{
            {"connector", o.connector},
            {"connection", o.connection},
            {"edid", json{{"make", o.make}, {"model", o.model}, {"serial", o.serial}}},
            {"physical_mm", json::array({o.physicalWidthMm, o.physicalHeightMm})},
            {"current", o.hasCurrent ? modeJson(o.current) : json(nullptr)},
            {"modes", modes},
        });
    }
    return json{{"outputs", outputs}};
}

json stateDoc(const ContractSnapshot& s, bool showLoaded) {
    json outputs = json::array();
    for (const auto& o : s.state.outputs) {
        outputs.push_back(json{
            {"connector", o.connector},
            {"enabled", o.enabled},
            {"region", json::array({o.x, o.y, o.width, o.height})},
            {"blend", json{{"left", o.blendLeft}, {"right", o.blendRight}, {"top", o.blendTop},
                           {"bottom", o.blendBottom}, {"gamma", o.blendGamma}}},
            {"mode_id", o.modeId.empty() ? json(nullptr) : json(o.modeId)},
        });
    }
    return json{
        {"mode", "show"},
        {"pacing", s.state.pacing},
        {"canvas", json{{"width", s.state.canvasWidth}, {"height", s.state.canvasHeight}}},
        {"config_source", s.state.configSource},
        {"show_loaded", showLoaded},
        {"outputs", outputs},
    };
}

std::string ok(const json& id, const json& result) {
    return json{{"v", ContractProtocol::kVersion}, {"id", id}, {"ok", true}, {"result", result}}.dump();
}

std::string err(const json& id, const std::string& code, const std::string& message,
                const json& extra = json::object()) {
    json e{{"code", code}, {"message", message}};
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        e[it.key()] = it.value();
    }
    return json{{"v", ContractProtocol::kVersion}, {"id", id}, {"ok", false}, {"error", e}}.dump();
}

bool lockPresent(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

}  // namespace

std::string ContractProtocol::handle(const std::string& request, const ContractSnapshot& snapshot,
                                     const Identity& identity, const std::string* cachedHardware) {
    json req = json::parse(request, nullptr, /*allow_exceptions=*/false);
    if (req.is_discarded() || !req.is_object()) {
        return err(nullptr, "bad_request", "request is not a JSON object");
    }
    const json id = req.contains("id") ? req["id"] : json(nullptr);
    if (!req.contains("op") || !req["op"].is_string()) {
        return err(id, "bad_request", "missing string field 'op'");
    }
    const std::string op = req["op"].get<std::string>();

    // hello ignores 'v': it is how a client discovers the versions.
    if (op == "hello") {
        return ok(id, json{
            {"contract_versions", json::array({kVersion})},
            {"vc_version", identity.vcVersion},
            {"pid", identity.pid},
            {"hostname", identity.hostname},
        });
    }
    if (!req.contains("v") || !req["v"].is_number_integer()) {
        return err(id, "bad_request", "missing integer field 'v'");
    }
    if (req["v"].get<int64_t>() > kVersion) {
        return err(id, "version_unsupported", "this videocomposer speaks contract v1",
                   json{{"max_v", kVersion}});
    }
    if (op == "get_caps") {
        return ok(id, json{{"caps", identity.caps}});
    }
    if (op == "get_hardware") {
        if (cachedHardware) {
            return "{\"v\":" + std::to_string(kVersion) + ",\"id\":" + id.dump() +
                   ",\"ok\":true,\"result\":" + *cachedHardware + "}";
        }
        return ok(id, hardwareDoc(snapshot));
    }
    if (op == "get_state") {
        return ok(id, stateDoc(snapshot, lockPresent(identity.showLockPath)));
    }
    return err(id, "unsupported_op", "unknown op '" + op + "'");
}

std::string ContractProtocol::hardwareJson(const ContractSnapshot& snapshot) {
    return hardwareDoc(snapshot).dump();
}

std::string ContractProtocol::stateJson(const ContractSnapshot& snapshot, bool showLoaded) {
    return stateDoc(snapshot, showLoaded).dump();
}

std::string ContractProtocol::eventStateChanged(uint64_t seq, int64_t pid) {
    return "state_changed " + json{{"v", kVersion}, {"seq", seq}, {"pid", pid},
                                    {"event", "state_changed"}}.dump();
}

std::string ContractProtocol::eventModeWarning(uint64_t seq, int64_t pid, const std::string& kind,
                                               const std::string& detail) {
    return "mode_warning " + json{{"v", kVersion}, {"seq", seq}, {"pid", pid}, {"event", "mode_warning"},
                                   {"kind", kind}, {"detail", detail}}.dump();
}

} // namespace videocomposer
