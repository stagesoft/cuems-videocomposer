# Log evidence bundle for ticket 869dd1c4d

3-monitor 4K playback render-budget overrun on slave_dev (N97).
Captured 2026-05-21. Author: investigation session with Ion Reguera.

## Pre-fix vs post-fix coverage

This bundle contains **both pre-fix and post-fix data** in the same files —
they are interleaved chronologically. The "fix" in question is
`fix(decode): suppress forward-jump during post-seek catch-up climb`
(now `origin/main 248c790`). Use these markers to slice the data:

| Time on 2026-05-21 | What was running on slave | Binary sha | Status |
|---|---|---|---|
| Before 12:09:07 | **PRE-fix instrumented** v0.1.1-3 | `90656a5e` | Trap active. Forward-jump flood 199 438 events. |
| 12:09:07 → 12:12:58 | Post-fix instrumented (forward-jump fix) | `4e4fd37e` | Fix installed BUT rtpmidid is broken; **MTC not flowing**, so no usable playback evidence in this window. |
| 12:12:58 → 13:21:01 | Post-fix instrumented | `4e4fd37e` | **Post-fix Tests A, B, C all live here.** rtpmidid recovered, normal playback. |
| 13:21:01 onwards | **Post-fix CLEAN** (no instrumentation) | `dbb9512c` | Same Test C config as 12:57+ window. Used to compare instrumentation overhead. |

**Pre-fix evidence** (the bug that ticket 869dd12wg's forward-jump fix
addressed) is concentrated in slave-vc-journal-trimmed.txt.gz — search
for `Forward jump detected`.

**Post-fix evidence** (the data this ticket 869dd1c4d is actually about
— 4K render-budget overrun that REMAINS after the forward-jump fix) is
in slave-debug.log between approximately 12:13:32 and 13:18, in the
`[RENDER] RATE` and `[VIDEO-DECQ] RATE` lines. The clean-binary post-fix
window (13:21+) contains no `[RENDER]` lines (instrumentation removed)
but is covered in the journal + the GPU measurement table below.

## Files in this bundle

| File | Origin | Size | Purpose |
|------|--------|------|---------|
| `slave-debug.log` | `slave_dev:/tmp/.claude/debug.log` | 1.5 MB | Per-second `[RENDER]`, `[VIDEO-DECQ]`, `[ENGINE]`, `[VIDEO-OSC]`, `[VIDEO-VFI]`, `[VIDEO-WRAP]` instrumentation. Written by the **instrumented** videocomposer binary that was running during Tests A/B/C; `[ENGINE]` continues to be written by `cuems-controller-engine` even after the clean binary was installed at 13:21. |
| `slave-vc-journal-trimmed.txt.gz` | `slave_dev: journalctl -u cuems-videocomposer --since today` | 464 KB | Full videocomposer journal for the day, with the pre-fix FORWARD-JUMP flood deduplicated (first 50 + last 50 of each burst preserved; counts in elided-block summaries). 199 438 forward-jump events total before the fix; 0 after. |
| `test-configs.txt` | manually compiled | <10 KB | Snapshots of `display.conf` + `videocomposer-flags.env` for each test phase. |

## Timeline (2026-05-21, all times local CEST)

| Window | Phase | Binary | What was running | Key signature |
|--------|-------|--------|------------------|---------------|
| 11:19 – 11:28 | Baseline pre-FORWARD-JUMP | instrumented v0.1.1-3 (pre-fix) | Single video, MTC running normally | DRIFT_MONITOR clean, MTC frames 442→17242 at 30 fps |
| 11:28:23 – ~11:55 | FORWARD-JUMP trap active | same | 1 video paused after SYSEX seek-to-267 | `Forward jump detected (target=267, lastDecoded=240)` flood at ~70/sec |
| 11:28:53 → 12:12:58 | rtpmidid split-brain | n/a | MTC not arriving to slave | Slave: `Timeout connecting to control port` every 30s. Master: split-brain warnings, ghost ports accumulating. See sibling ticket 869dd12wg. |
| 11:55:48 | Fix install on master | instrumented + forward-jump fix (sha 4e4fd37e) | — | Both `restart cuems-videocomposer`. Master smooth post-restart. |
| 12:09:07 | Fix install on slave | same | — | Reveals the rtpmidid breakage (`syncFrame=-1 rolling=0`). |
| 12:12:58 | rtpmidid restart both ends | — | — | Workaround. MTC starts flowing again. |
| ~12:14 – 12:23 | Test A | instrumented + fix | 1 video decode → 1× 1080p + 2× 4K outputs | Render dropped to **48 fps**. avg render ~17.5 ms. |
| 12:23:49 | Project reload | — | — | "Reset: removing all layers" — operator triggered reload to add more decodes |
| 12:24:01 – 12:28+ | Test B | instrumented + fix | 3 video decodes → 1× 1080p + 2× 4K outputs | **45 fps, 98% GPU render busy, 67% CPU (11% iowait, 11% softirq)**. avg render 17.5 ms. Decode steady at 30 fps × 3 streams. |
| ~12:54:55 | Test C setup | — | — | `display.conf` swapped to 3× 1920x1080 canvas + `OPERATOR_FLAGS --resolution 1080p` added. DRM modes confirmed at 1080p on all 3 panels. |
| 12:57:12 – 13:18 | Test C | instrumented + fix | 3 video decodes → 3× 1080p outputs (4K panels downscaled) | **60 fps locked, 45% GPU render busy, 100% CPU idle.** |
| 13:21+ | Clean binary deploy | clean main + fix (sha dbb9512c) | Same Test C config | 42-45% render busy, 21-22% video busy, 0.42-0.45 W. PresentationTiming dropped 2 frames once at GO; zero after. Forward-jump events: 0. |

## GPU + CPU measurements (intel_gpu_top + top)

Test A and the baseline (single output 1080p) were captured before we started saving raw `intel_gpu_top -J` output — only the per-second engine % numbers are below. Test B and Test C have rich timeseries embedded in the `slave-debug.log` `[RENDER] RATE` / `[VIDEO-DECQ] RATE` lines.

| Test | Decodes | Outputs | Render busy | Video | GPU MHz | Power | CPU | iowait | fps | avg render |
|------|---------|---------|-------------|-------|---------|-------|-----|--------|-----|------------|
| Baseline | 1 | 1× 1080p | 75-93 % | 0 % | 1042 | 0.66 W | 25 % | 0 % | 60 | 16.6 ms |
| A | 1 | 1× 1080p + 2× 4K | (no GPU sample) | 0 % | — | — | ~50 % | 0 % | **48** | ~17.5 ms |
| B | 3 | 1× 1080p + 2× 4K | **98 %** | 24 % | 1137 (max boost) | 1.02 W | 67 % | **11 %** | **45** | 17.5 ms |
| **C** | 3 | **3× 1080p** | **45 %** | 23 % | ~420 (idle) | 0.45 W | 0 % | 0 % | **60** | 16.5 ms |
| C (clean binary) | 3 | 3× 1080p | 42-45 % | 21-22 % | 450-483 | 0.42-0.45 W | 0 % (sample varies, 33% iowait at moments) | varies | 60 | n/a (instrumentation removed) |

## How to re-analyse this bundle

- **fps timeseries**: `grep '\[RENDER\] \[DEBUG' slave-debug.log` → `effective_fps` and `avg_render_us` columns per second.
- **per-stream decode rate**: `grep '\[VIDEO-DECQ\].*RATE' slave-debug.log` → `effective_fps` per stream per second.
- **forward-jump trap activations** (pre-fix): `grep 'Forward jump detected' slave-vc-journal-trimmed.txt` — 199 438 total. After fix: 0-3 per session (genuine stalls only).
- **engine-side cue offset broadcasts**: `grep '\[ENGINE\]' slave-debug.log` — 3 different cue UUIDs visible in Test B / C (`9543c3fb`, `7e30aa74`, `21c17910`).
- **time window selection**: use the timeline above to slice by timestamp. E.g. Test C clean-binary window starts at 13:21:01 (`PresentationTiming: Initialized for 60Hz`).

## Caveats

1. The instrumented binary running during Tests A/B/C was branch `fix/forward-jump-post-seek-trap` (commit `d5d6270`, equivalent of `248c790` on main). The clean binary is built from `main` at `248c790` with no instrumentation — `[RENDER]` / `[VIDEO-DECQ]` tags stop appearing in `debug.log` after 13:21:01. `[ENGINE]` tag continues to be written by `cuems-controller-engine` (Python, separate instrumentation).
2. **rtpmidid breakage** between 11:28:53 and 12:12:58 contaminates the window. During that period MTC was NOT flowing to slave — any frame numbers / render rates from that window are not meaningful playback data. See sibling ticket 869dd12wg for the rtpmidid investigation.
3. `slave-debug.log` is what was captured at 13:27:39. The file is append-only; if the `[ENGINE]` writer continues, later snapshots will have more lines. This snapshot is sufficient to cover Tests A through C plus the clean-binary verification window.
