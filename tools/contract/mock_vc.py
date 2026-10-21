#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
"""Mock videocomposer for the hardware-configuration contract v1
(docs/contract-v1.md): same sockets, same schema, canned hardware (a node with
two 4K30 panels and one free connector). For the manager team to build
against without a GPU, and for tools/contract/test_contract.py.

    mock_vc.py [--rep URL] [--pub URL] [--event-every SECONDS]
"""
import argparse
import json
import os
import threading
import time

import pynng

VERSION = 1


def mode(w, h, clock, htotal, vtotal, interlaced=False, preferred=False, suffix=""):
    hz = clock * 1000.0 / (htotal * vtotal)
    return {
        "mode_id": f"{w}x{h}@{hz:.2f}{'i' if interlaced else 'p'}{suffix}",
        "width": w, "height": h,
        "refresh_hz": round(hz, 2), "field_rate_hz": round(hz * (2 if interlaced else 1), 2),
        "interlaced": interlaced, "preferred": preferred,
        "timing": {"clock_khz": clock, "hdisplay": w, "hsync_start": w + 88, "hsync_end": w + 132,
                   "htotal": htotal, "vdisplay": h, "vsync_start": h + 4, "vsync_end": h + 9,
                   "vtotal": vtotal, "flags": 5 | (16 if interlaced else 0),
                   "type": 64 | (8 if preferred else 0)},
    }


def canned_hardware():
    modes = [mode(3840, 2160, 297000, 4400, 2250, preferred=True),
             mode(1920, 1080, 148500, 2200, 1125),
             mode(1920, 1080, 74250, 2200, 1125, interlaced=True)]
    out = []
    for name, serial in (("HDMI-A-1", "MOCK0001"), ("HDMI-A-2", "MOCK0002")):
        out.append({"connector": name, "connection": "connected",
                    "edid": {"make": "MCK", "model": "Mock 4K", "serial": serial},
                    "physical_mm": [600, 340], "current": modes[0], "modes": modes})
    out.append({"connector": "HDMI-A-3", "connection": "disconnected",
                "edid": {"make": "", "model": "", "serial": ""}, "physical_mm": [0, 0],
                "current": None, "modes": []})
    return {"outputs": out}


def canned_state(show_lock="/tmp/cuems.show.lock"):
    return {"mode": "show", "pacing": "coupled", "canvas": {"width": 7680, "height": 2160},
            "config_source": "mock", "show_loaded": os.path.exists(show_lock),
            "outputs": [{"connector": f"HDMI-A-{i + 1}", "enabled": True, "region": [3840 * i, 0, 3840, 2160],
                         "blend": {"left": 0, "right": 0, "top": 0, "bottom": 0, "gamma": 2.2},
                         "mode_id": "3840x2160@30.00p"} for i in range(2)]}


def handle(raw, ident):
    try:
        req = json.loads(raw)
    except (ValueError, UnicodeDecodeError):
        req = None
    if not isinstance(req, dict):
        return {"v": VERSION, "id": None, "ok": False,
                "error": {"code": "bad_request", "message": "request is not a JSON object"}}
    rid = req.get("id")
    op = req.get("op")

    def err(code, msg, **extra):
        return {"v": VERSION, "id": rid, "ok": False, "error": {"code": code, "message": msg, **extra}}

    def ok(result):
        return {"v": VERSION, "id": rid, "ok": True, "result": result}

    if not isinstance(op, str):
        return err("bad_request", "missing string field 'op'")
    if op == "hello":
        return ok({"contract_versions": [VERSION], **ident})
    if not isinstance(req.get("v"), int) or isinstance(req.get("v"), bool):
        return err("bad_request", "missing integer field 'v'")
    if req["v"] > VERSION:
        return err("version_unsupported", "this videocomposer speaks contract v1", max_v=VERSION)
    if op == "get_caps":
        return ok({"caps": ["report_v1", "mode_timing", "interlace_flag"]})
    if op == "get_hardware":
        return ok(canned_hardware())
    if op == "get_state":
        return ok(canned_state())
    return err("unsupported_op", f"unknown op '{op}'")


def serve(rep_url, pub_url, event_every, stop):
    ident = {"vc_version": "mock", "pid": os.getpid(), "hostname": os.uname().nodename}
    with pynng.Rep0(listen=rep_url, recv_timeout=200) as rep, pynng.Pub0(listen=pub_url) as pub:
        seq = 0
        next_event = time.monotonic() + event_every if event_every else None
        while not stop.is_set():
            try:
                raw = rep.recv()
                rep.send(json.dumps(handle(raw.decode(errors="replace"), ident)).encode())
            except pynng.Timeout:
                pass
            if next_event and time.monotonic() >= next_event:
                seq += 1
                ev = {"v": VERSION, "seq": seq, "pid": ident["pid"], "event": "state_changed"}
                pub.send(("state_changed " + json.dumps(ev)).encode())
                next_event += event_every


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rep", default="ipc:///tmp/cuems-videocomposer.ipc")
    ap.add_argument("--pub", default="ipc:///tmp/cuems-videocomposer-events.ipc")
    ap.add_argument("--event-every", type=float, default=0.0)
    args = ap.parse_args()
    stop = threading.Event()
    try:
        serve(args.rep, args.pub, args.event_every, stop)
    except KeyboardInterrupt:
        stop.set()


if __name__ == "__main__":
    main()
