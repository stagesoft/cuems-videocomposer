# SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
"""Contract v1 conformance tests (docs/contract-v1.md).

Against the mock (default: started by the fixture on private sockets):
    /usr/lib/cuems/bin/python3 -m pytest -q tools/contract/test_contract.py
Against a running videocomposer on this node:
    VC_CONTRACT_REAL=1 /usr/lib/cuems/bin/python3 -m pytest -q tools/contract/test_contract.py
Never on casas (test suites run on the rigs).
"""
import json
import os
import re
import threading
import time

import pynng
import pytest

import mock_vc
from vc_contract_client import ContractError, VcContractClient, VcContractEvents

REAL = os.environ.get("VC_CONTRACT_REAL") == "1"
MODE_ID = re.compile(r"^\d+x\d+@\d+\.\d{2}[pi](/\d+)?(#\d+)?$")


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
        ids = [m["mode_id"] for m in o["modes"]]
        assert len(ids) == len(set(ids)), f"{o['connector']}: duplicate mode ids"
        for m in o["modes"] + ([o["current"]] if o["current"] else []):
            assert MODE_ID.match(m["mode_id"]), m["mode_id"]
            t = m["timing"]
            hz = t["clock_khz"] * 1000.0 / (t["htotal"] * t["vtotal"])
            assert abs(round(hz, 2) - m["refresh_hz"]) < 0.011
            assert m["field_rate_hz"] == pytest.approx(m["refresh_hz"] * (2 if m["interlaced"] else 1), abs=0.02)
        if o["current"]:
            assert o["current"]["mode_id"] in ids or not ids


def test_state_schema(client):
    s = client.state()
    assert s["mode"] == "show" and s["pacing"] in ("coupled", "per_surface")
    assert isinstance(s["show_loaded"], bool)
    hw = {o["connector"]: o for o in client.hardware()["outputs"]}
    for o in s["outputs"]:
        assert len(o["region"]) == 4
        if o["enabled"] and o["mode_id"]:
            assert hw[o["connector"]]["current"]["mode_id"] == o["mode_id"]


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
