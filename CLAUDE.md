# cuems-videocomposer

Part of the **CUEMS** ecosystem — see the [`cuems-RELATIONS`](https://github.com/stagesoft/cuems-RELATIONS) repo for the system index, architecture diagram, and protocol/port map.

## Role

Multi-layer video compositor for live production: hardware-decoded video rendering via DRM/KMS (also X11/Wayland), OSC-controlled, MTC-synced. C++17. systemd service `cuems-videocomposer.service` (node-side). Features: multi-layer composition/blend, MTC/LTC sync, NDI input, hardware decode (VAAPI/CUDA/QSV), HAP direct GPU texture upload, startup splash, on-screen timecode/OSD.

Vendored git submodules: `mtcreceiver`, `oscreceiver` (+ others). Run `git submodule update --init` after cloning.

## Build

```bash
mkdir -p build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)
```

Deploy binaries with the **stop → cp → start** discipline (the player is a systemd service; `cp` over a running executable fails with `Text file busy`, and `mv`+`cp` without a restart keeps the old inode running). Verify via `readlink /proc/$(systemctl show -p MainPID --value cuems-videocomposer)/exe`.

## Video cue lifecycle (VideoComposer side)

The engine→VC protocol is documented once in the cuems-RELATIONS CLAUDE.md ("Video Cue Lifecycle"). VC's half: receive `/videocomposer/layer/load <file> <cueId>` on arm; on run, honor `/offset`, `/visible`, `/loop`, `/mtcfollow`; lock playback to MTC; auto-unload the layer on end (if enabled); `/videocomposer/reset` clears all state between projects. Default OSC port **7000**.

`LayerPlayback` clamps an overshooting media frame to `totalFrames-1` while `wraparound_` is false, and wraps (`% totalFrames`) once it's true. `wraparound_` is enabled by the engine's `/loop=1` — for infinite loops the engine now sends `/loop` **before** `/mtcfollow` (see field notes).

## Resolution control

Precedence (after the DRMBackend patch on `main`/`rc_1`): **`--resolution` CLI flag > `resolution_policy` in `/run/cuems/display.conf` (general) > built-in 1080p default**, with **per-output `resolution=WxH` lines always overriding on top**. So this works:

```
resolution_policy=native
canvas_layout=custom
[output:HDMI-A-1]
  canvas_region=0,0,1920,1080
  resolution=1920x1080     # per-output exception (pin a 4K panel to 1080p)
```

The patch lives in `cpp/display/drm/DRMBackend.cpp` (`openWindow`/`initializeVirtualCanvas`): load display.conf before the global modeset (unless `-r` was explicit); the per-output override block applies unconditionally. **Deploy gotcha:** `cuems-generate-display-conf` does NOT emit `resolution_policy=`, so a fresh node with no operator override falls back to the 1080p default.

## Branch layout (after 2026-07-02 cleanup)

- **`rc_1`** = production truth (the deployed 24h binary). Carries async HAP decoder, shared-decoder, forward-jump safety-net + post-seek catch-up, gputexture sub-upload, videoindexer AUTO hwdec, 24h `mtcreceiver`. `main` trails rc_1.
- **`rc_1-instrumented`** = rc_1 + render-side instrumentation cherry-picks (`OFFSET-APPLIED` log, LayerPlayback wrap/sync tracing, DRMBackend/RemoteCommandRouter tracing → `/tmp/.claude/debug.log`). Build this when an investigation needs render-side visibility.
- **`fix/hap-loop-wrap-race-instrumented`** (commit `5eb4238`, "do NOT merge") adds per-second `[RENDER]`/`[RENDER-FINE]` timers (avg render/composite/atomic-commit µs, gated by CMake `CUEMS_FINE_TIMERS`) — the tool for diagnosing render-bound frame drops.
- `archive/debug-2026-07` (tag) = the old `debug` branch (experimental `fix/mtc-bias-compensation` anti-drift smoother, intentionally not adopted).

**Methodology note:** to decide if a commit is really absent from rc_1, compare file **CONTENT** (`diff <(git show A:path) <(git show B:path)`), NOT commit-hash ancestry (cherry-picks get new hashes) and not a path-assumed grep.

## Field notes / gotchas

- **Looping-video first-loop freeze (FIXED engine-side).** A `loop=-1` cue played one loop, froze ~one loop on the last frame, then looped forever. Root cause was engine OSC ordering: `/mtcfollow=1` was sent at GO but `/loop=1` only after the postwait, so during that gap the layer followed MTC with `wraparound_` OFF → overshoot → clamp to last frame. Fixed in cuems-engine `rc_1` `41ab6bc` (`run_videoCue` sends `/loop` before `/mtcfollow` for infinite loops). It was **NOT** the HAP decoder's per-wrap buffer clears (benign, ~25ms recovery) and **NOT** a 30fps-MTC bug (MTC is 25fps; VC correctly translates 25fps MTC *time* to 30fps media *frame* numbers). Latent since engine `4efc1e7` (2026-03); only visible when postwait > clip length.
- **2s synchronized skip on ALL outputs (FIXED in mtcreceiver).** All outputs across both nodes + audio skipped in lockstep every ~2s. Cause: `libmtcmaster` (jitter_improvement build) emits a full-frame MTC SysEx resync every 50 frames (2s @25fps); mtcreceiver's QF-assembled `mtcHead` lags wire-MTC by a constant ~3 frames (Phase-2 commit `021b689` removed the `+2` compensation), so storing the FF into `mtcHead` unconditionally snapped +80–120ms every 2s → VC MIDISyncSource anti-drift hard-snapped. Fixed in mtcreceiver `rc_1` `aa44894` (hold the QF timebase on RESYNC, only reposition on genuine SEEK). VC side shipped on `main` `118e06d` (submodule bump + MIDISyncSource snap log + `resetWrapOffset()` in `resetAll()` + classifier tolerance 2→5); **VC deb 0.1.2-2** (also fixed `Depends: librtmidi1`→`librtmidi6`). See the mtcreceiver CLAUDE.md.
- **Engine/VC canvas mismatch is a KNOWN accepted limitation.** When `display.conf` declares more outputs than are physically connected, the engine canvas = bbox of ALL declared `[output:*]` regions while VC self-limits to *connected* DRM connectors. Cues targeting declared-but-disconnected outputs land outside the VC canvas — expected on partially-cabled rigs; do NOT trim display.conf ad hoc. Also: `canvas_region` (logical) ≠ scanout mode; e.g. an Acer V193HQV via HDMI→VGA runs native 1366×768 while its adapter-mangled EDID falsely advertises 1080p — don't force 1080p through such adapters.
- **N97 display ceiling** (source/measurement, 2026-06): sustains 2×4K@30 or 1×4K@60 with 0 drops but **hard-fails 3×4K@30** (render/EU-saturation, perf-limit reason `OTHER`, ~69°C — NOT thermal, NOT pl4). i3-1215U tops out at the same 2×4K@30 but for a thermal reason. HAP shifts every verdict down. Real lever: distribute outputs across nodes (1×4K@60/node proven, MTC-synced, 0 drops). `PresentationTiming` counts drops from DRM page-flip MSC deltas → `journalctl -u cuems-videocomposer | grep Dropped` is the accurate signal.
