# SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
"""Contract v1 conformance tests (docs/contract-v1.md).

Against the mock (default: started by the fixture on private sockets):
    /usr/lib/cuems/bin/python3 -m pytest -q tools/contract/test_contract.py
Against a running videocomposer on this node:
    VC_CONTRACT_REAL=1 /usr/lib/cuems/bin/python3 -m pytest -q tools/contract/test_contract.py
Plus the per-output color round trip over OSC (changes the picture for a few
seconds and restores it; needs no project loaded, i.e. no /tmp/cuems.show.lock):
    VC_CONTRACT_REAL=1 VC_CONTRACT_OSC=1 /usr/lib/cuems/bin/python3 -m pytest -q tools/contract/test_contract.py
Never on casas (test suites run on the rigs).
"""
import json
import math
import os
import re
import socket
import struct
import threading
import time

import pynng
import pytest

import mock_vc
from vc_contract_client import ContractError, VcContractClient, VcContractEvents

REAL = os.environ.get("VC_CONTRACT_REAL") == "1"
OSC = os.environ.get("VC_CONTRACT_OSC") == "1"
OSC_PORT = int(os.environ.get("VC_OSC_PORT", "7000"))
MODE_ID = re.compile(r"^(\d+)x(\d+)@(\d+\.\d{2})([pi])$")
COLOR_KEYS = {"brightness", "contrast", "saturation", "hue", "gamma", "lut", "lut_status", "applied"}
TRIM_RANGES = {"brightness": (-1.0, 1.0), "contrast": (0.0, 2.0), "saturation": (0.0, 2.0),
               "hue": (-180.0, 180.0), "gamma": (0.1, 3.0)}
IDENTITY_TRIMS = {"brightness": 0.0, "contrast": 1.0, "saturation": 1.0, "hue": 0.0, "gamma": 1.0}


@pytest.fixture(scope="module")
def urls():
    if REAL:
        yield "ipc:///tmp/cuems-videocomposer.ipc", "ipc:///tmp/cuems-videocomposer-events.ipc"
        return
    tag = f"{os.getpid()}"
    rep, pub = f"ipc:///tmp/vc-mock-{tag}.ipc", f"ipc:///tmp/vc-mock-{tag}-ev.ipc"
    stop = threading.Event()
    t = threading.Thread(target=mock_vc.serve, args=(rep, pub, 0.2, stop), daemon=True)
    t.start()
    time.sleep(0.3)
    yield rep, pub
    stop.set()
    t.join(2)


@pytest.fixture(scope="module")
def urls_no_color():
    """A mock VC whose color program did not link (mock only)."""
    tag = f"{os.getpid()}-nocolor"
    rep, pub = f"ipc:///tmp/vc-mock-{tag}.ipc", f"ipc:///tmp/vc-mock-{tag}-ev.ipc"
    stop = threading.Event()
    t = threading.Thread(target=mock_vc.serve, args=(rep, pub, 0.0, stop),
                         kwargs={"color_program": False}, daemon=True)
    t.start()
    time.sleep(0.3)
    yield rep, pub
    stop.set()
    t.join(2)


@pytest.fixture
def client(urls):
    with VcContractClient(urls[0]) as c:
        yield c


def test_hello(client):
    h = client.hello()
    assert 1 in h["contract_versions"] and isinstance(h["pid"], int) and h["hostname"]


def test_hello_answers_any_version(client):
    assert client.request("hello", v=7)["contract_versions"] == [1]


def test_caps(client):
    caps = client.caps()
    assert "report_v1" in caps and "hotplug" not in caps


def test_errors(client):
    with pytest.raises(ContractError) as e:
        client.request("get_hardware", v=2)
    assert e.value.code == "version_unsupported" and e.value.reply["error"]["max_v"] == 1
    with pytest.raises(ContractError) as e:
        client.request("apply")
    assert e.value.code == "unsupported_op"


def test_hardware_schema(client):
    hw = client.hardware()["outputs"]
    assert hw, "no connector reported"
    for o in hw:
        assert o["connection"] in ("connected", "disconnected", "unknown")
        ids = [m["id"] for m in o["modes"]]
        assert len(ids) == len(set(ids)), f"{o['connector']}: duplicate ids"
        for m in o["modes"]:
            w, h, hz, kind = MODE_ID.match(m["id"]).groups()
            assert (int(w), int(h)) == (m["width"], m["height"])
            assert float(hz) == pytest.approx(m["hz"], abs=0.006)
            assert (kind == "i") == m["interlaced"]
            assert "timing" not in m
        for ref in (o["current"], o["preferred"]):
            assert ref is None or ref in ids


def test_hardware_full_detail(client):
    full = client.request_raw({"op": "get_hardware", "detail": "full"})["outputs"]
    for o in full:
        for m in o["modes"]:
            t = m["timing"]
            frame = t["clock_khz"] * 1000.0 / (t["htotal"] * t["vtotal"])
            assert abs(round(frame, 2) - m["refresh_hz"]) < 0.011
            assert m["field_rate_hz"] == pytest.approx(m["refresh_hz"] * (2 if m["interlaced"] else 1), abs=0.02)


def test_state_schema(client):
    s = client.state()
    assert s["mode"] == "show" and s["pacing"] in ("coupled", "per_surface")
    assert isinstance(s["show_loaded"], bool)
    hw = {o["connector"]: o for o in client.hardware()["outputs"]}
    for o in s["outputs"]:
        assert len(o["region"]) == 4
        if o["enabled"] and o["current"]:
            assert hw[o["connector"]]["current"] == o["current"]


@pytest.mark.skipif(REAL, reason="the real VC publishes only on configuration changes")
def test_events_seq(urls):
    ev = VcContractEvents(urls[1])
    try:
        seen = []
        end = time.monotonic() + 3
        while time.monotonic() < end and len(seen) < 3:
            try:
                seen.append(ev.next())
            except pynng.Timeout:
                pass
        assert len(seen) >= 2
        assert seen[0][0] == "state_changed"
        assert all(not gap for _, _, gap in seen[1:])  # consecutive seq after the first
    finally:
        ev.close()


def check_color_schema(color):
    assert set(color) == COLOR_KEYS
    for key, (lo, hi) in TRIM_RANGES.items():
        assert isinstance(color[key], (int, float)) and not isinstance(color[key], bool)
        assert lo <= color[key] <= hi, f"{key}={color[key]} outside [{lo}, {hi}]"
    assert color["lut_status"] in ("none", "ok", "missing", "invalid")
    assert color["lut"] is None or (isinstance(color["lut"], str) and color["lut"].startswith("/"))
    if color["lut"] is None:
        assert color["lut_status"] == "none"
    assert isinstance(color["applied"], bool)


def test_state_color(client):
    """Per-output color (plan 2.7): with the cap, every output carries `color`;
    without it, any `color` present is not applied."""
    caps = client.caps()
    outputs = client.state()["outputs"]
    if "output_color" in caps:
        assert outputs
        for o in outputs:
            check_color_schema(o["color"])
    else:
        for o in outputs:
            if "color" in o:
                check_color_schema(o["color"])
                assert o["color"]["applied"] is False
    if not REAL:
        assert "output_color" in caps
        a, b = outputs[0]["color"], outputs[1]["color"]
        assert a == {"brightness": 0.02, "contrast": 1.08, "saturation": 1.0, "hue": -3.0, "gamma": 1.05,
                     "lut": "/etc/cuems/luts/p2-christie.cube", "lut_status": "ok", "applied": True}
        assert {k: b[k] for k in IDENTITY_TRIMS} == IDENTITY_TRIMS and b["lut"] is None
        assert b["lut_status"] == "none" and b["applied"] is True


@pytest.mark.skipif(REAL, reason="mock fixture: a VC whose color program did not link")
def test_no_color_program_mock(urls_no_color):
    with VcContractClient(urls_no_color[0]) as c:
        caps = c.caps()
        outputs = c.state()["outputs"]
    assert "output_color" not in caps and "report_v1" in caps
    assert outputs and all(o["color"]["applied"] is False for o in outputs)
    for o in outputs:
        check_color_schema(o["color"])


def osc_message(address, *args):
    """A minimal OSC message, every argument as a string (the VC parses them)."""
    def pad(b):
        b += b"\0"
        return b + b"\0" * (-len(b) % 4)
    tags = "," + "s" * len(args)
    return pad(address.encode()) + pad(tags.encode()) + b"".join(pad(str(a).encode()) for a in args)


def send_osc(address, *args):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.sendto(osc_message(address, *args), ("127.0.0.1", OSC_PORT))


def wait_state(client, predicate, timeout=3.0):
    """Poll get_state until predicate(state) holds (the VC applies OSC between frames)."""
    end = time.monotonic() + timeout
    state = client.state()
    while not predicate(state) and time.monotonic() < end:
        time.sleep(0.05)
        state = client.state()
    return state


def output_of(state, connector):
    return next(o for o in state["outputs"] if o["connector"] == connector)


@pytest.mark.skipif(not (REAL and OSC), reason="real VC + VC_CONTRACT_OSC=1 only: changes the picture")
def test_color_round_trip_and_blend_keeps_color(urls):
    """S5 rig row: an OSC color change shows in get_state and publishes state_changed;
    a later display/blend leaves `color` unchanged (M1); everything restored."""
    with VcContractClient(urls[0]) as c:
        if "output_color" not in c.caps():
            pytest.skip("no output_color cap on this VC")
        state = c.state()
        if state["show_loaded"]:
            pytest.skip("a project is loaded (show lock present): display commands are refused")
        target = next((o for o in state["outputs"] if o["enabled"]
                       and {k: o["color"][k] for k in IDENTITY_TRIMS} == IDENTITY_TRIMS
                       and o["color"]["lut"] is None), None)
        if target is None:
            pytest.skip("every output has color configured; not touching a calibration")
        name, blend = target["connector"], target["blend"]
        ev = VcContractEvents(urls[1])
        try:
            time.sleep(0.2)  # let the subscription land before the change
            send_osc("/videocomposer/display/color", name, "brightness", "0.1")
            s1 = wait_state(c, lambda s: math.isclose(output_of(s, name)["color"]["brightness"], 0.1,
                                                      abs_tol=1e-6))
            color = output_of(s1, name)["color"]
            assert math.isclose(color["brightness"], 0.1, abs_tol=1e-6) and color["applied"] is True
            topics = []
            end = time.monotonic() + 2
            while time.monotonic() < end and "state_changed" not in topics:
                try:
                    topics.append(ev.next()[0])
                except pynng.Timeout:
                    pass
            assert "state_changed" in topics

            send_osc("/videocomposer/display/blend", name, "64", blend["right"], blend["top"],
                     blend["bottom"], blend["gamma"])
            s2 = wait_state(c, lambda s: output_of(s, name)["blend"]["left"] == 64)
            assert output_of(s2, name)["blend"]["left"] == 64
            assert output_of(s2, name)["color"] == color  # M1: the blend rebuild keeps the color
        finally:
            ev.close()
            send_osc("/videocomposer/display/blend", name, blend["left"], blend["right"], blend["top"],
                     blend["bottom"], blend["gamma"])
            send_osc("/videocomposer/display/color", name, "reset")
        s3 = wait_state(c, lambda s: output_of(s, name)["color"]["brightness"] == 0.0
                        and output_of(s, name)["blend"] == blend)
        restored = output_of(s3, name)
        assert {k: restored["color"][k] for k in IDENTITY_TRIMS} == IDENTITY_TRIMS
        assert restored["blend"] == blend
