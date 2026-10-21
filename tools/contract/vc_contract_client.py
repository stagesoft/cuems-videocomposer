#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
"""Reference client for the videocomposer hardware-configuration contract v1
(docs/contract-v1.md). Usable as a library or from the command line:

    vc_contract_client.py hello|caps|hardware|state      one request, JSON out
    vc_contract_client.py events [SECONDS]               print events as they arrive
    vc_contract_client.py poll HZ SECONDS [OP]           load generator (rig tests)

Runs with /usr/lib/cuems/bin/python3 (pynng is in the CUEMS venv).
"""
import itertools
import json
import sys
import time

import pynng

REQ_URL = "ipc:///tmp/cuems-videocomposer.ipc"
PUB_URL = "ipc:///tmp/cuems-videocomposer-events.ipc"
VERSION = 1


class ContractError(RuntimeError):
    def __init__(self, code, message, reply):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.reply = reply


class VcContractClient:
    """One Req socket; every call sends a request and waits for its reply."""

    def __init__(self, url=REQ_URL, timeout_ms=2000):
        self._ids = itertools.count(1)
        self._sock = pynng.Req0(recv_timeout=timeout_ms, send_timeout=timeout_ms)
        self._sock.dial(url, block=True)

    def close(self):
        self._sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def request(self, op, v=VERSION):
        req = {"v": v, "id": next(self._ids), "op": op}
        self._sock.send(json.dumps(req).encode())
        reply = json.loads(self._sock.recv().decode())
        if reply.get("id") != req["id"]:
            raise RuntimeError(f"reply id {reply.get('id')!r} does not match request {req['id']!r}")
        if not reply.get("ok"):
            err = reply.get("error", {})
            raise ContractError(err.get("code"), err.get("message"), reply)
        return reply["result"]

    def hello(self):
        return self.request("hello")

    def caps(self):
        return self.request("get_caps")["caps"]

    def hardware(self):
        return self.request("get_hardware")

    def state(self):
        return self.request("get_state")


class VcContractEvents:
    """Subscriber. Per the contract: subscribe first, then read state, then
    watch seq/pid and re-read on a gap or a new pid."""

    def __init__(self, url=PUB_URL, timeout_ms=1000):
        self._sock = pynng.Sub0(recv_timeout=timeout_ms)
        self._sock.subscribe(b"")
        self._sock.dial(url, block=False)
        self.last = {}  # pid -> last seq

    def next(self):
        """Return (topic, event dict, gap) or raise pynng.Timeout."""
        raw = self._sock.recv().decode()
        topic, _, body = raw.partition(" ")
        ev = json.loads(body)
        pid, seq = ev.get("pid"), ev.get("seq")
        prev = self.last.get(pid)
        gap = prev is None or seq != prev + 1
        self.last[pid] = seq
        return topic, ev, gap

    def close(self):
        self._sock.close()


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd in ("hello", "caps", "hardware", "state"):
        with VcContractClient() as c:
            result = {"hello": c.hello, "caps": c.caps, "hardware": c.hardware, "state": c.state}[cmd]()
        print(json.dumps(result, indent=2))
        return 0
    if cmd == "events":
        seconds = float(argv[2]) if len(argv) > 2 else 60.0
        ev = VcContractEvents()
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                topic, body, gap = ev.next()
                print(topic, json.dumps(body), "(gap: re-read state)" if gap else "", flush=True)
            except pynng.Timeout:
                pass
        return 0
    if cmd == "poll":
        hz, seconds = float(argv[2]), float(argv[3])
        op = argv[4] if len(argv) > 4 else "get_state"
        n = errors = 0
        worst = 0.0
        with VcContractClient() as c:
            end = time.monotonic() + seconds
            period = 1.0 / hz
            while time.monotonic() < end:
                t0 = time.monotonic()
                try:
                    c.request(op)
                    n += 1
                except Exception:  # noqa: BLE001 — counted, the run goes on
                    errors += 1
                dt = time.monotonic() - t0
                worst = max(worst, dt)
                time.sleep(max(0.0, period - dt))
        print(json.dumps({"op": op, "requests": n, "errors": errors, "worst_ms": round(worst * 1000, 1)}))
        return 0 if errors == 0 else 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
