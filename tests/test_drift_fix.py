#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Test suite for Option 6 anti-drift fix in MIDISyncSource::getTimeMs().
# Requires: cuems-videocomposer running via systemd, libmtcmaster, python-osc.
# Reads DRIFT_MONITOR logs from journalctl (LOG_INFO level required).
#
# Usage:
#   python3 tests/test_drift_fix.py
#   python3 tests/test_drift_fix.py --osc-port 7000

import sys
import time
import subprocess
import re
import argparse
from pathlib import Path

try:
    from pythonosc.udp_client import SimpleUDPClient
except ImportError:
    print("ERROR: python-osc not installed. Install with: pip install python-osc")
    sys.exit(1)

# Import MTC helper
sys.path.insert(0, str(Path(__file__).parent))
from mtc_helper import MTCHelper, MTC_AVAILABLE

# --- Media files ---
MEDIA_DIR = Path("/opt/cuems_library/media")
VIDEO_60FPS = MEDIA_DIR / "center_with_music.mov"      # 60fps, 15:46
VIDEO_25FPS = MEDIA_DIR / "01_sync_1080_l_25.mov"       # 25fps, 19.96s
VIDEO_60FPS_SHORT = MEDIA_DIR / "center_30s.mov"        # 60fps, 30s

MTC_FPS = 25.0
THRESHOLD_MS = 20  # accounts for MTC sender jitter in test environments


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def get_drift_logs(since_seconds=10):
    """Read DRIFT_MONITOR lines from journalctl."""
    result = subprocess.run(
        ["journalctl", "-u", "cuems-videocomposer",
         f"--since={since_seconds} sec ago", "--no-pager"],
        capture_output=True, text=True
    )
    lines = []
    for line in result.stdout.splitlines():
        if "DRIFT_MONITOR" in line:
            m = re.search(r'errorMs=(-?\d+).*rate=([\d.]+).*baseMtcMs=(\d+)', line)
            if m:
                lines.append({
                    "errorMs": int(m.group(1)),
                    "rate": float(m.group(2)),
                    "baseMtcMs": int(m.group(3)),
                    "raw": line,
                })
    return lines


def wait_for_logs(min_samples=3, timeout=15):
    """Wait until we have at least min_samples DRIFT_MONITOR entries."""
    start = time.time()
    while time.time() - start < timeout:
        logs = get_drift_logs(since_seconds=8)
        if len(logs) >= min_samples:
            return logs
        time.sleep(1)
    return get_drift_logs(since_seconds=timeout)


def stats(logs):
    """Compute error stats from log entries."""
    if not logs:
        return None
    errors = [entry["errorMs"] for entry in logs]
    abs_errors = sorted(abs(e) for e in errors)
    n = len(abs_errors)
    return {
        "n": n,
        "min": min(errors),
        "max": max(errors),
        "mean": sum(errors) / n,
        "abs_max": abs_errors[-1],
        "abs_p95": abs_errors[int(n * 0.95)] if n > 20 else abs_errors[-1],
    }


def check_bounded(logs, threshold_ms, label=""):
    """Assert all errors are within threshold."""
    s = stats(logs)
    if not s:
        print(f"  FAIL {label}: no logs collected")
        return False
    ok = s["abs_max"] <= threshold_ms
    status = "PASS" if ok else "FAIL"
    print(f"  {status} {label}: n={s['n']} min={s['min']}ms max={s['max']}ms "
          f"mean={s['mean']:.1f}ms abs_max={s['abs_max']}ms abs_p95={s['abs_p95']}ms")
    return ok


class LayerManager:
    """Load/unload layers via OSC on the running videocomposer."""

    def __init__(self, osc_port=7000):
        self.osc = SimpleUDPClient("127.0.0.1", osc_port)
        self.loaded_cues = []

    def load(self, video_path, cue_id):
        """Load a layer, set mtcfollow, make visible."""
        print(f"  Loading layer {cue_id}: {video_path.name}")
        self.osc.send_message("/videocomposer/layer/load", [str(video_path), cue_id])
        time.sleep(1.5)
        self.osc.send_message(f"/videocomposer/layer/{cue_id}/mtcfollow", [1])
        self.osc.send_message(f"/videocomposer/layer/{cue_id}/visible", [1])
        time.sleep(0.5)
        self.loaded_cues.append(cue_id)

    def unload_all(self):
        """Remove all loaded layers."""
        for cue_id in self.loaded_cues:
            self.osc.send_message(f"/videocomposer/layer/{cue_id}/remove", [])
        time.sleep(1)
        self.loaded_cues.clear()

    def reset(self):
        """Full compositor reset."""
        self.osc.send_message("/videocomposer/reset", [])
        time.sleep(1)
        self.loaded_cues.clear()


# ---------------------------------------------------------------------------
# Test cases
# ---------------------------------------------------------------------------

def test_60fps_steady_state(mtc, layers):
    """60fps video @ 25fps MTC — primary cross-fps drift path, 30s."""
    print("\n=== Test 1: 60fps steady-state (30s) ===")
    layers.reset()
    layers.load(VIDEO_60FPS, "drift-60fps")
    mtc.start(start_frame=0)
    time.sleep(3)  # EMA warm-up

    logs_before = get_drift_logs(since_seconds=2)
    if not logs_before:
        print("  Waiting for DRIFT_MONITOR logs...")
        wait_for_logs(min_samples=2, timeout=10)

    time.sleep(30)
    logs = get_drift_logs(since_seconds=32)
    return check_bounded(logs, THRESHOLD_MS, label="60fps steady-state 30s")


def test_25fps_no_drift(mtc, layers):
    """25fps video @ 25fps MTC — same-fps, getTimeMs() not called, no drift."""
    print("\n=== Test 2: 25fps same-fps baseline (15s) ===")
    layers.reset()
    layers.load(VIDEO_25FPS, "drift-25fps")
    mtc.start(start_frame=0)
    time.sleep(3)

    time.sleep(15)
    logs = get_drift_logs(since_seconds=17)
    if not logs:
        print("  PASS 25fps no-drift: no DRIFT_MONITOR logs (getTimeMs() not called, as expected)")
        return True
    # If logs appear, they should still be bounded
    return check_bounded(logs, THRESHOLD_MS, label="25fps same-fps")


def test_multilayer_60_and_25(mtc, layers):
    """Two layers: 60fps + 25fps simultaneously, 20s."""
    print("\n=== Test 3: Multi-layer 60fps + 25fps (20s) ===")
    layers.reset()
    layers.load(VIDEO_60FPS, "drift-multi-60")
    layers.load(VIDEO_25FPS, "drift-multi-25")
    mtc.start(start_frame=0)
    time.sleep(3)

    time.sleep(20)
    logs = get_drift_logs(since_seconds=22)
    return check_bounded(logs, THRESHOLD_MS, label="multi-layer 60+25fps")


def test_stop_resume(mtc, layers):
    """Stop MTC for 3s, resume, verify clean recovery."""
    print("\n=== Test 4: Stop / Resume ===")
    layers.reset()
    layers.load(VIDEO_60FPS_SHORT, "drift-stopresume")
    mtc.start(start_frame=0)
    time.sleep(5)

    print("  Stopping MTC...")
    mtc.stop()
    time.sleep(3)

    print("  Resuming MTC...")
    mtc.start(start_frame=0)
    time.sleep(5)

    logs = get_drift_logs(since_seconds=7)
    return check_bounded(logs, THRESHOLD_MS, label="post-resume")


def test_seek_backward(mtc, layers):
    """Seek backward — triggers mtcStepMs < -10, tests justSnapped guard."""
    print("\n=== Test 5: Seek backward ===")
    layers.reset()
    layers.load(VIDEO_60FPS, "drift-seekback")
    mtc.seek(minutes=5)
    mtc.start(start_frame=int(5 * 60 * MTC_FPS))
    time.sleep(5)

    print("  Seeking backward to 1:00...")
    mtc.seek(minutes=1)
    time.sleep(0.5)
    mtc.start(start_frame=int(1 * 60 * MTC_FPS))
    time.sleep(5)

    logs = get_drift_logs(since_seconds=7)
    return check_bounded(logs, THRESHOLD_MS, label="post-backward-seek")


def test_seek_forward(mtc, layers):
    """Seek forward — large positive mtcStepMs, tests snap + recovery."""
    print("\n=== Test 6: Seek forward ===")
    print("  Seeking forward to 10:00...")
    mtc.seek(minutes=10)
    time.sleep(0.5)
    mtc.start(start_frame=int(10 * 60 * MTC_FPS))
    time.sleep(5)

    logs = get_drift_logs(since_seconds=7)
    return check_bounded(logs, THRESHOLD_MS, label="post-forward-seek")


def test_rapid_seeks(mtc, layers):
    """Multiple rapid seeks — stress test snap/correction interaction."""
    print("\n=== Test 7: Rapid seeks ===")
    positions = [
        (0, 30),   # 0:30
        (3, 0),    # 3:00
        (0, 5),    # 0:05
        (8, 0),    # 8:00
        (1, 0),    # 1:00
    ]
    for mins, secs in positions:
        total_frames = int((mins * 60 + secs) * MTC_FPS)
        mtc.seek(minutes=mins, seconds=secs)
        time.sleep(0.3)
        mtc.start(start_frame=total_frames)
        time.sleep(2)

    time.sleep(3)
    logs = get_drift_logs(since_seconds=5)
    return check_bounded(logs, THRESHOLD_MS, label="post-rapid-seeks")


def test_multilayer_two_60fps(mtc, layers):
    """Two 60fps layers simultaneously, 20s."""
    print("\n=== Test 8: Multi-layer 2x 60fps (20s) ===")
    layers.reset()
    time.sleep(2)  # let reset settle
    layers.load(VIDEO_60FPS, "drift-dual60-a")
    layers.load(VIDEO_60FPS_SHORT, "drift-dual60-b")
    time.sleep(3)  # let async loads complete
    mtc.start(start_frame=0)
    time.sleep(5)  # EMA warm-up

    # Confirm logs are flowing before collecting
    initial = wait_for_logs(min_samples=2, timeout=15)
    if not initial:
        print("  WARNING: still no logs, extending wait...")
        time.sleep(10)
    time.sleep(20)
    logs = get_drift_logs(since_seconds=30)
    return check_bounded(logs, THRESHOLD_MS, label="multi-layer 2x60fps")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Drift fix verification test suite")
    parser.add_argument("--osc-port", type=int, default=7000,
                        help="OSC port of running videocomposer (default: 7000)")
    args = parser.parse_args()

    if not MTC_AVAILABLE:
        print("ERROR: libmtcmaster not available")
        sys.exit(1)

    # Check media files exist
    for f in [VIDEO_60FPS, VIDEO_25FPS, VIDEO_60FPS_SHORT]:
        if not f.exists():
            print(f"ERROR: media file not found: {f}")
            sys.exit(1)

    # Verify cuems-videocomposer is running
    result = subprocess.run(
        ["systemctl", "is-active", "cuems-videocomposer"],
        capture_output=True, text=True
    )
    if result.stdout.strip() != "active":
        print("ERROR: cuems-videocomposer service is not running")
        sys.exit(1)

    print("=" * 60)
    print("DRIFT FIX VERIFICATION TEST SUITE")
    print(f"MTC: {MTC_FPS}fps  |  OSC port: {args.osc_port}")
    print(f"60fps: {VIDEO_60FPS.name}")
    print(f"25fps: {VIDEO_25FPS.name}")
    print(f"Threshold: {THRESHOLD_MS}ms")
    print("=" * 60)

    mtc = MTCHelper(fps=MTC_FPS, portname="DriftTest")
    mtc.setup()
    layers = LayerManager(osc_port=args.osc_port)

    results = {}
    try:
        results["60fps_steady_state"] = test_60fps_steady_state(mtc, layers)
        results["25fps_no_drift"] = test_25fps_no_drift(mtc, layers)
        results["multilayer_60_25"] = test_multilayer_60_and_25(mtc, layers)
        results["stop_resume"] = test_stop_resume(mtc, layers)
        results["seek_backward"] = test_seek_backward(mtc, layers)
        results["seek_forward"] = test_seek_forward(mtc, layers)
        results["rapid_seeks"] = test_rapid_seeks(mtc, layers)
        results["multilayer_2x60"] = test_multilayer_two_60fps(mtc, layers)
    except KeyboardInterrupt:
        print("\nInterrupted.")
    finally:
        layers.reset()
        mtc.cleanup()

    # Summary
    print("\n" + "=" * 60)
    print("DRIFT FIX VERIFICATION RESULTS")
    print("=" * 60)
    all_pass = True
    for name, passed in results.items():
        status = "PASS" if passed else "FAIL"
        print(f"  {status}  {name}")
        if not passed:
            all_pass = False

    print()
    if all_pass:
        print("ALL TESTS PASSED")
        sys.exit(0)
    else:
        print("SOME TESTS FAILED")
        sys.exit(1)


if __name__ == "__main__":
    main()
