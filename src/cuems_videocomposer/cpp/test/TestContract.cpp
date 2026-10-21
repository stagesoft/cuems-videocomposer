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

#ifdef HAVE_CONTRACT_TESTS

#include "TestFramework.h"
#include "../remote/ContractProtocol.h"
#include "../remote/ContractServer.h"
#include "../remote/ContractTypes.h"

#include <nlohmann/json.hpp>
#include <nng/nng.h>
#include <nng/protocol/pubsub0/sub.h>
#include <nng/protocol/reqrep0/req.h>

#include <chrono>
#include <iostream>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>

using namespace videocomposer;
using namespace videocomposer::test;
using json = nlohmann::json;

namespace {

ContractModeTiming timing(uint32_t clock, uint32_t hd, uint32_t ht, uint32_t vd, uint32_t vt,
                          uint32_t flags = 0, uint32_t type = 0) {
    ContractModeTiming t;
    t.clockKhz = clock;
    t.hdisplay = hd; t.hsyncStart = hd + 88; t.hsyncEnd = hd + 132; t.htotal = ht;
    t.vdisplay = vd; t.vsyncStart = vd + 4; t.vsyncEnd = vd + 9; t.vtotal = vt;
    t.flags = flags;
    t.type = type;
    return t;
}

ContractSnapshot sampleSnapshot() {
    ContractSnapshot s;
    ContractOutputHardware a;
    a.connector = "HDMI-A-1";
    a.connection = "connected";
    a.make = "GSM"; a.model = "LG UltraFine"; a.serial = "506NTMXDV771";
    a.physicalWidthMm = 600; a.physicalHeightMm = 340;
    a.modes.push_back(ContractMode::fromTiming(timing(148500, 1920, 2200, 1080, 1125, 0, 1u << 3)));
    a.modes.push_back(ContractMode::fromTiming(timing(74250, 1920, 2200, 1080, 1125, 1u << 4)));
    assignModeIds(a.modes);
    a.hasCurrent = true;
    a.current = a.modes[0];
    ContractOutputHardware b;
    b.connector = "HDMI-A-2";
    b.connection = "disconnected";
    s.hardware = {a, b};
    s.state.pacing = "coupled";
    s.state.canvasWidth = 1920; s.state.canvasHeight = 1080;
    s.state.configSource = "/run/cuems/display.conf";
    ContractOutputState o;
    o.connector = "HDMI-A-1"; o.enabled = true; o.width = 1920; o.height = 1080;
    o.modeId = contractModeId(a.modes[0]);
    s.state.outputs = {o};
    return s;
}

ContractProtocol::Identity identity(const std::string& lockPath = "/nonexistent/cuems.show.lock") {
    ContractProtocol::Identity id;
    id.vcVersion = "test";
    id.hostname = "node01";
    id.pid = 4242;
    id.caps = {"report_v1"};
    id.showLockPath = lockPath;
    return id;
}

json call(const std::string& req, const std::string& lock = "/nonexistent/cuems.show.lock") {
    return json::parse(ContractProtocol::handle(req, sampleSnapshot(), identity(lock)));
}

std::string tmpUrl(const char* tag) {
    return "ipc:///tmp/vc-contract-test-" + std::to_string(getpid()) + "-" + tag + ".ipc";
}

bool requestOnce(const std::string& url, const std::string& body, std::string& reply, int timeoutMs = 2000) {
    nng_socket req;
    if (nng_req0_open(&req) != 0) return false;
    nng_socket_set_ms(req, NNG_OPT_RECVTIMEO, timeoutMs);
    nng_socket_set_ms(req, NNG_OPT_SENDTIMEO, timeoutMs);
    bool ok = nng_dial(req, url.c_str(), nullptr, 0) == 0 &&
              nng_send(req, const_cast<char*>(body.data()), body.size(), 0) == 0;
    if (ok) {
        char* buf = nullptr;
        size_t sz = 0;
        ok = nng_recv(req, &buf, &sz, NNG_FLAG_ALLOC) == 0;
        if (ok) {
            reply.assign(buf, sz);
            nng_free(buf, sz);
        }
    }
    nng_close(req);
    return ok;
}

}  // namespace

bool test_Contract_ModeFromTimingAndIds() {
    auto p = ContractMode::fromTiming(timing(148500, 1920, 2200, 1080, 1125, 0, 1u << 3));
    auto i = ContractMode::fromTiming(timing(74250, 1920, 2200, 1080, 1125, 1u << 4));
    if (p.interlaced || !p.preferred || std::abs(p.refreshHz - 60.0) > 1e-6) return false;
    // 1080i60: the VC's formula gives the frame rate; the field rate is double.
    if (!i.interlaced || std::abs(i.refreshHz - 30.0) > 1e-6 || std::abs(i.fieldRateHz - 60.0) > 1e-6) return false;
    std::vector<ContractMode> modes = {
        p, i,
        ContractMode::fromTiming(timing(297000, 1920, 4400, 1080, 1125)),  // also 60.00p, other clock
        p,                                                                  // exact duplicate
    };
    assignModeIds(modes);
    return modes[0].modeId == "1920x1080@60.00p/148500#1" && modes[3].modeId == "1920x1080@60.00p/148500#2" &&
           modes[1].modeId == "1920x1080@30.00i" && modes[2].modeId == "1920x1080@60.00p/297000";
}

bool test_Contract_HelloIgnoresVersion() {
    auto r = call(R"({"v":2,"id":"a","op":"hello"})");
    return r["ok"] == true && r["id"] == "a" && r["result"]["contract_versions"] == json::array({1}) &&
           r["result"]["pid"] == 4242 && r["result"]["hostname"] == "node01";
}

bool test_Contract_Errors() {
    auto bad = call("not json");
    auto noV = call(R"({"id":1,"op":"get_state"})");
    auto newer = call(R"({"v":2,"id":2,"op":"get_hardware"})");
    auto unknown = call(R"({"v":1,"id":3,"op":"apply"})");
    return bad["ok"] == false && bad["error"]["code"] == "bad_request" && bad["id"].is_null() &&
           noV["error"]["code"] == "bad_request" && noV["id"] == 1 &&
           newer["error"]["code"] == "version_unsupported" && newer["error"]["max_v"] == 1 &&
           unknown["error"]["code"] == "unsupported_op";
}

bool test_Contract_HardwareShape() {
    // Default view: typed fields + exact id, interlaced at its field rate, no timings.
    auto r = call(R"({"v":1,"id":"h","op":"get_hardware"})");
    if (r["ok"] != true) return false;
    const auto& outs = r["result"]["outputs"];
    if (outs.size() != 2) return false;
    const auto& a = outs[0];
    const auto& b = outs[1];
    const bool simple = a["connector"] == "HDMI-A-1" && a["connection"] == "connected" &&
        a["edid"]["serial"] == "506NTMXDV771" && a["current"] == "1920x1080@60.00p" &&
        a["preferred"] == "1920x1080@60.00p" && a["modes"].size() == 2 &&
        a["modes"][1]["id"] == "1920x1080@60.00i" && a["modes"][1]["hz"] == 60.0 &&
        a["modes"][1]["interlaced"] == true && !a["modes"][0].contains("timing") &&
        b["connection"] == "disconnected" && b["current"].is_null() && b["preferred"].is_null() &&
        b["modes"].empty();
    // Full view on request: timings and the unique per-connector mode_id.
    auto f = call(R"({"v":1,"id":"f","op":"get_hardware","detail":"full"})");
    const auto& fa = f["result"]["outputs"][0];
    const bool full = fa["modes"][1]["timing"]["htotal"] == 2200 && fa["modes"][1]["field_rate_hz"] == 60.0 &&
        fa["modes"][1]["id"] == "1920x1080@60.00i" && fa["current"]["mode_id"] == "1920x1080@60.00p";
    return simple && full;
}

bool test_Contract_DuplicateModesCollapse() {
    ContractSnapshot s = sampleSnapshot();
    s.hardware[0].modes.push_back(s.hardware[0].modes[0]);  // EDID duplicate
    s.hardware[0].modes.push_back(ContractMode::fromTiming(timing(297000, 1920, 4400, 1080, 1125)));  // other timing, same id
    assignModeIds(s.hardware[0].modes);
    auto r = json::parse(ContractProtocol::handle(R"({"v":1,"id":1,"op":"get_hardware"})", s, identity()));
    return r["result"]["outputs"][0]["modes"].size() == 2;
}

bool test_Contract_CachedHardwareMatches() {
    const auto snap = sampleSnapshot();
    const std::string cached = ContractProtocol::hardwareJson(snap);
    const std::string req = R"({"v":1,"id":{"k":[1,2]},"op":"get_hardware"})";
    auto a = json::parse(ContractProtocol::handle(req, snap, identity()));
    auto b = json::parse(ContractProtocol::handle(req, snap, identity(), &cached));
    return a == b && b["id"] == json{{"k", {1, 2}}};
}

bool test_Contract_StateShowLock() {
    const std::string lock = "/tmp/vc-contract-test-lock-" + std::to_string(getpid());
    std::remove(lock.c_str());
    auto absent = call(R"({"v":1,"id":"s","op":"get_state"})", lock);
    if (FILE* f = std::fopen(lock.c_str(), "w")) {
        std::fputs("1", f);
        std::fclose(f);
    }
    auto present = call(R"({"v":1,"id":"s","op":"get_state"})", lock);
    std::remove(lock.c_str());
    return absent["result"]["show_loaded"] == false && present["result"]["show_loaded"] == true &&
           present["result"]["mode"] == "show" && present["result"]["pacing"] == "coupled" &&
           present["result"]["outputs"][0]["region"] == json::array({0, 0, 1920, 1080}) &&
           present["result"]["outputs"][0]["current"] == "1920x1080@60.00p";
}

bool test_Contract_ServerRoundTripAndBindFailure() {
    ContractSnapshotHolder holder;
    holder.set(sampleSnapshot());
    ContractServer::Config cfg;
    cfg.repUrl = tmpUrl("rep");
    cfg.pubUrl = tmpUrl("pub");
    ContractServer first(holder, identity(), cfg);
    if (!first.start()) { std::cerr << "[first.start failed] "; return false; }
    std::string reply;
    if (!requestOnce(cfg.repUrl, R"({"v":1,"id":"x","op":"get_hardware"})", reply)) { std::cerr << "[req1 failed] "; return false; }
    if (json::parse(reply)["result"]["outputs"][0]["connector"] != "HDMI-A-1") { std::cerr << "[req1 content] "; return false; }
    // A second instance on the same socket must fail softly; the first keeps serving.
    ContractServer second(holder, identity(), cfg);
    if (second.start()) { std::cerr << "[second.start succeeded] "; return false; }
    reply.clear();
    if (!requestOnce(cfg.repUrl, R"({"v":1,"id":"y","op":"hello"})", reply)) { std::cerr << "[req2 failed] "; return false; }
    first.stop();
    first.stop();  // idempotent
    return json::parse(reply)["id"] == "y" && !first.running();
}

bool test_Contract_ServerLifecycleStress() {
    ContractSnapshotHolder holder;
    holder.set(sampleSnapshot());
    ContractServer::Config cfg;
    cfg.repUrl = tmpUrl("life");
    cfg.pubUrl = tmpUrl("lifepub");
    for (int i = 0; i < 100; ++i) {
        ContractServer s(holder, identity(), cfg);
        if (!s.start()) return false;
        if (i % 10 == 0) {
            // Close with a request in flight: a client sends and the server stops at once.
            std::thread client([&] {
                std::string r;
                requestOnce(cfg.repUrl, R"({"v":1,"id":"z","op":"get_hardware"})", r, 300);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            s.stop();
            client.join();
        }
    }
    return true;
}

bool test_Contract_OversizedRequestDoesNotKillServer() {
    ContractSnapshotHolder holder;
    holder.set(sampleSnapshot());
    ContractServer::Config cfg;
    cfg.repUrl = tmpUrl("big");
    cfg.pubUrl = tmpUrl("bigpub");
    ContractServer s(holder, identity(), cfg);
    if (!s.start()) return false;
    // The abusive client runs detached: with nng 1.5.2 its own nng_close() can
    // block while the oversized message is still in flight. That is the
    // client's problem; what matters is that the server keeps serving others.
    const std::string url = cfg.repUrl;
    std::thread([url] {
        std::string ignored;
        const std::string huge(10 * 1024 * 1024, '{');
        requestOnce(url, huge, ignored, 500);  // dropped by RECVMAXSZ: no reply expected
    }).detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::string reply;
    const bool ok = requestOnce(cfg.repUrl, R"({"v":1,"id":"after","op":"hello"})", reply);
    return ok && json::parse(reply)["id"] == "after";
}

bool test_Contract_PubEventsReachSubscriber() {
    ContractSnapshotHolder holder;
    holder.set(sampleSnapshot());
    ContractServer::Config cfg;
    cfg.repUrl = tmpUrl("evrep");
    cfg.pubUrl = tmpUrl("ev");
    ContractServer s(holder, identity(), cfg);
    if (!s.start()) return false;
    nng_socket sub;
    if (nng_sub0_open(&sub) != 0) return false;
    nng_socket_set(sub, NNG_OPT_SUB_SUBSCRIBE, "", 0);
    nng_socket_set_ms(sub, NNG_OPT_RECVTIMEO, 300);
    nng_dial(sub, cfg.pubUrl.c_str(), nullptr, 0);
    bool got = false;
    for (int attempt = 0; attempt < 10 && !got; ++attempt) {  // pub/sub: the subscription needs a moment
        s.publish(ContractProtocol::eventStateChanged(s.nextSeq(), s.pid()));
        char* buf = nullptr;
        size_t sz = 0;
        if (nng_recv(sub, &buf, &sz, NNG_FLAG_ALLOC) == 0) {
            const std::string ev(buf, sz);
            nng_free(buf, sz);
            got = ev.rfind("state_changed ", 0) == 0 &&
                  json::parse(ev.substr(14))["event"] == "state_changed";
        }
    }
    nng_close(sub);
    return got;
}

#endif  // HAVE_CONTRACT_TESTS
