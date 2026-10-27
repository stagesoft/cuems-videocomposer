#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
"""Mock videocomposer for the hardware-configuration contract v1
(docs/contract-v1.md): same sockets, same schema, canned hardware (a node with
two 4K30 panels and one free connector). For the manager team to build
against without a GPU, and for tools/contract/test_contract.py.

    mock_vc.py [--rep URL] [--pub URL] [--event-every SECONDS] [--no-color-program]

HDMI-A-1 carries a configured per-output color (trims + a LUT), HDMI-A-2 the
identity. --no-color-program mocks a VC whose color program did not link: no
"output_color" cap and applied=false on every output.
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
    field = hz * (2 if interlaced else 1)
    return {
        "id": f"{w}x{h}@{field:.2f}{'i' if interlaced else 'p'}",
        "mode_id": f"{w}x{h}@{hz:.2f}{'i' if interlaced else 'p'}{suffix}",
        "width": w, "height": h,
        "refresh_hz": round(hz, 2), "field_rate_hz": round(hz * (2 if interlaced else 1), 2),
        "interlaced": interlaced, "preferred": preferred,
        "timing": {"clock_khz": clock, "hdisplay": w, "hsync_start": w + 88, "hsync_end": w + 132,
                   "htotal": htotal, "vdisplay": h, "vsync_start": h + 4, "vsync_end": h + 9,
                   "vtotal": vtotal, "flags": 5 | (16 if interlaced else 0),
                   "type": 64 | (8 if preferred else 0)},
    }


def simple(hw):
    """Default view: typed fields + exact id; duplicates collapse; no timings."""
    outs = []
    for o in hw["outputs"]:
        seen, modes, preferred = set(), [], None
        for m in o["modes"]:
            if m["preferred"] and preferred is None:
                preferred = m["id"]
            if m["id"] in seen:
                continue
            seen.add(m["id"])
            modes.append({"id": m["id"], "width": m["width"], "height": m["height"],
                          "hz": m["field_rate_hz"], "interlaced": m["interlaced"]})
        outs.append({"connector": o["connector"], "connection": o["connection"], "edid": o["edid"],
                     "current": o["current"]["id"] if o["current"] else None,
                     "preferred": preferred, "modes": modes})
    return {"outputs": outs}


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


BASE_CAPS = ["report_v1", "mode_timing", "interlace_flag"]
IDENTITY_COLOR = {"brightness": 0.0, "contrast": 1.0, "saturation": 1.0, "hue": 0.0, "gamma": 1.0,
                  "lut": None, "lut_status": "none"}


def canned_caps(color_program=True):
    return BASE_CAPS + (["output_color"] if color_program else [])


def canned_color(i, color_program=True):
    if i == 0:
        color = {"brightness": 0.02, "contrast": 1.08, "saturation": 1.0, "hue": -3.0, "gamma": 1.05,
                 "lut": "/etc/cuems/luts/p2-christie.cube", "lut_status": "ok" if color_program else "none"}
    else:
        color = dict(IDENTITY_COLOR)
    color["applied"] = color_program
    return color


def canned_state(show_lock="/tmp/cuems.show.lock", color_program=True):
    return {"mode": "show", "pacing": "coupled", "canvas": {"width": 7680, "height": 2160},
            "config_source": "mock", "show_loaded": os.path.exists(show_lock),
            "outputs": [{"connector": f"HDMI-A-{i + 1}", "enabled": True, "region": [3840 * i, 0, 3840, 2160],
                         "blend": {"left": 0, "right": 0, "top": 0, "bottom": 0, "gamma": 2.2},
                         "current": "3840x2160@30.00p",
                         "color": canned_color(i, color_program)} for i in range(2)]}


def handle(raw, ident, color_program=True):
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
        return ok({"caps": canned_caps(color_program)})
    if op == "get_hardware":
        full = canned_hardware()
        return ok(full if req.get("detail") == "full" else simple(full))
    if op == "get_state":
        return ok(canned_state(color_program=color_program))
    return err("unsupported_op", f"unknown op '{op}'")


def serve(rep_url, pub_url, event_every, stop, color_program=True):
    ident = {"vc_version": "mock", "pid": os.getpid(), "hostname": os.uname().nodename}
    with pynng.Rep0(listen=rep_url, recv_timeout=200) as rep, pynng.Pub0(listen=pub_url) as pub:
        seq = 0
        next_event = time.monotonic() + event_every if event_every else None
        while not stop.is_set():
            try:
                raw = rep.recv()
                rep.send(json.dumps(handle(raw.decode(errors="replace"), ident, color_program)).encode())
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
    ap.add_argument("--no-color-program", action="store_true",
                    help="mock a VC whose color program did not link (no output_color cap, applied=false)")
    args = ap.parse_args()
    stop = threading.Event()
    try:
        serve(args.rep, args.pub, args.event_every, stop, color_program=not args.no_color_program)
    except KeyboardInterrupt:
        stop.set()


if __name__ == "__main__":
    main()
