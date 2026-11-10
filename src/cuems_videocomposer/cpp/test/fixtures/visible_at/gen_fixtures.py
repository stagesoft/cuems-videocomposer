#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
"""The cadence fixtures of the visible_at tests (869feynrj step 1b) that the 1amfwm7gpz7 model does not export:
outputs at 25 and 30 Hz. Same model, same format and the same self-consistent export as its run_scenarios.py
--export (integer period, frames from that run). Usage:

    python3 gen_fixtures.py <cuems-RELATIONS>/baselines/test3-saltitos-20261104/model

writes va_*.txt next to this file. The other rows of TestVisibleAt replay the model's own fixtures (../cadence/)."""
import os
import sys

model = os.path.abspath(sys.argv[1])
sys.path.insert(0, model)
from engine import Engine          # noqa: E402
from world import run_ideal        # noqa: E402

ROWS = (
    # name, fps, hz, seconds, t0 (s, a phase off the vblank grid), sigma_ms
    ("va_25at25", 25, 25, 12, 10.37, 0.8),    # exact, r = 1
    ("va_25at30", 25, 30, 12, 10.37, 0.8),    # Bresenham, r = 1.2
    ("va_30at25", 30, 25, 12, 10.37, 0.8),    # drop regime, r = 0.83
)

here = os.path.dirname(os.path.abspath(__file__))


class Rec(Engine):
    def __init__(self, *a, **k):
        super().__init__(*a, **k)
        self.trace = []

    def poll(self, pos, msc, valid=True):
        f = super().poll(pos, msc, valid)
        self.trace.append((pos, msc, valid, f))
        return f


for name, fps, hz, secs, t0, sigma in ROWS:
    e = Rec(fps, 1e9 / hz, H=0.6, lowpass=16)
    run_ideal(e, fps, hz, secs, sigma_ms=sigma, t0=t0)
    per = int(1e9 / hz)
    rp = Engine(fps, per, H=0.6, lowpass=16)
    frames = [rp.poll((p * 1000.0 / fps) * fps / 1000.0, m, valid=v) for p, m, v, _ in e.trace]
    with open(os.path.join(here, name + ".txt"), "w", encoding="utf-8") as f:
        f.write("%r %d %r %d %d\n" % (float(fps), per, 0.6, 16, 30))
        for (pos, msc, valid, _), fr in zip(e.trace, frames):
            f.write("P %r %d %d %d\n" % (pos, msc, 1 if valid else 0, fr))
        for msc, kind in rp.events:
            f.write("E %d %s\n" % (msc, kind))
        f.write("C %d %d %d %d %d %d\n" % (rp.corr[0], rp.corr[1], rp.skips, rp.drops, rp.stalls, rp.reanchors))
    print(name, len(frames), "polls; corr", rp.corr, "drops", rp.drops, "reanchors", rp.reanchors)
