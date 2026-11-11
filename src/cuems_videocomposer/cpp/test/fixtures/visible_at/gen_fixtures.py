#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
"""The cadence fixtures of the visible_at tests (869feynrj step 1b) that the 1amfwm7gpz7 model does not export:
outputs at 25 and 30 Hz. Same model, same format and the same self-consistent export as its run_scenarios.py
--export (integer period, frames from that run). Usage:

    python3 gen_fixtures.py <cuems-RELATIONS>/baselines/test3-saltitos-20261104/model

writes va_*.txt next to this file. The other rows of TestVisibleAt replay the model's own fixtures (../cadence/).

va_res_*: the five residues of a 30 fps start on the 25 fps MTC grid (1amfwm7h924's offset split: whole content
frames + a sub-frame phase the converter adds to its clock). The positions are those of a model fixture in ../cadence/;
the frames are the model's on the clock shifted by the phase exactly as the C++ computes it (splitEngineOffset, then
(posMs + phaseMs) * fps / 1000), so the expectations are phase-adjusted. Extra line: O <offset> <whole> <phaseMs>.
The run is cut at 240 polls (the onsets are within the first 120)."""
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


# ---- the 30 fps offset residues (869feynrj review X4) ----------------------------------------------------------------
import math   # noqa: E402


def split(offset, mtc_fps, content_fps):
    """sync/EngineOffsetSplit.h, the same double operations in the same order."""
    x = float(offset) * content_fps / mtc_fps
    whole = math.floor(x + 1e-9)
    frac = x - whole
    if frac < 0.0:
        frac = 0.0
    return int(whole), frac * 1000.0 / content_fps


def read_cadence(name):
    with open(os.path.join(here, "..", "cadence", name + ".txt"), encoding="utf-8") as f:
        head = f.readline().split()
        rows = []
        for line in f:
            t = line.split()
            if t and t[0] == "P":
                rows.append((float(t[1]), int(t[2]), t[3] != "0"))
    return float(head[0]), int(head[1]), float(head[2]), int(head[3]), int(head[4]), rows


RES = (("a_Exact", "30at60"), ("e_Bresenham", "30at50"))
for base, tag in RES:
    fps, per, H, lowpass, spacing, rows = read_cadence(base)
    rows = rows[:240]
    first = math.floor(rows[0][0])
    for k in range(10):                          # starts 40..49 MTC frames after the first position: all 5 residues twice
        offset = -(int(first * 25 // 30) + 40 + k)
        whole, phase = split(offset, 25.0, fps)
        eng = Engine(fps, per, H=H, lowpass=lowpass, spacing=spacing)
        frames = [eng.poll(((p * 1000.0 / fps) + phase) * fps / 1000.0, m, valid=v) for p, m, v in rows]
        name = "va_res_%s_%d" % (tag, -offset)
        with open(os.path.join(here, name + ".txt"), "w", encoding="utf-8") as f:
            f.write("%r %d %r %d %d\n" % (fps, per, H, lowpass, spacing))
            f.write("O %d %d %r\n" % (offset, whole, phase))
            for (p, m, v), fr in zip(rows, frames):
                f.write("P %r %d %d %d\n" % (p, m, 1 if v else 0, fr))
        onset = next((i for i, fr in enumerate(frames) if fr + whole >= 0), -1)
        print(name, "residue", (-offset) % 5, "whole", whole, "phase %.3f ms" % phase, "onset poll", onset,
              "shown", frames[onset] + whole if onset >= 0 else None)

