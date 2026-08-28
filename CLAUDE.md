# cuems-videocomposer

Part of the **CUEMS** ecosystem — see the [`cuems-RELATIONS`](https://github.com/stagesoft/cuems-RELATIONS) repo for the system index, architecture diagram, and protocol/port map.

## Role

Multi-layer video compositor for live production: hardware-decoded video rendering via **DRM/KMS**, OSC-controlled, MTC-synced. C++17. systemd service `cuems-videocomposer.service` (node-side). Features: multi-layer composition/blend, MTC/LTC sync, NDI input, hardware decode (VAAPI/CUDA/QSV), HAP direct GPU texture upload, startup splash, on-screen timecode/OSD.

**DRM/KMS is THE path** — it is what every deployed node runs and the only backend kept current. `X11Display` and `WaylandDisplay` are **legacy, left in the tree but outdated**: they still compile and can come up on a dev desktop, but they are not maintained and must not be treated as reference behaviour. When the backends disagree, DRM is right and the other two are stale — never "fix" DRM to match them, and never rely on something X11/Wayland happen to do (see the hardware-decoder probe gotcha below for a bug that hid exactly there). `HeadlessDisplay` is a last-resort fallback when DRM fails, not a selectable mode.

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

- **A layer can claim hardware decode and run on the CPU — defect 6(b) (FIXED, `feat/g8-truth-in-reporting` `5e3b753`).** `AsyncDecodeQueue::isHardwareDecoding()` answers from `useHardware_`, which `open()` sets from *"a VAAPI device attached and the codec is whitelisted"* — a statement of **intent**, decided before any frame exists. A **4:2:2 8-bit** clip satisfies every one of those conditions and FFmpeg then decodes it on the CPU regardless, so the layer reported `Health::ok` **and hardware** while the GPU's decode engine sat at zero. Pre-existing, not an F2 regression.
  The truthful signal was already being computed and thrown away: the decode thread compares `decodeFrame_->format == AV_PIX_FMT_VAAPI` at two sites and simply took the other branch when they disagreed. `noteDecodedFormat()` now latches that on the **first decoded frame of each `open()`** (so a recovery reopen re-arms it — a reopen can land on a different path), logs it once at **ERROR** naming the real pixel format, and `VideoFileInput::getHealth()` **derives** `sw_fallback` from it.
  Three things to know before touching this:
  1. **`getHealth()`/`getHealthReason()` `try_lock` `queueAccessMutex_` and must NEVER block on it.** That mutex is the recovery gate — `close()` and the tier-3 path `reset()` the queue's `unique_ptr`, so reading it unguarded is a use-after-free waiting for a second thread; but `recoveryWorkerFunc` **holds the same mutex across its whole `{1000,2000,4000}` ms backoff**, so a blocking lock would stall the caller for seconds. Failing the try_lock returns the un-refined `health_`, which is correct: a busy gate means recovery owns the queue, i.e. the layer is already in a fault `health_` reports.
  2. **`sw_fallback` is not a failure state.** The layer plays. It now has two routes in: refused at `open()` (ladder tier 3) or accepted and gone soft (derived). Only `declared_failed`/`load_failed` mean broken, and the derivation is guarded on `h == Health::ok` so it can never mask them.
  3. **It is journal-only today.** Nothing outside `VideoFileInput` calls `getHealth()`, and the OSC surface has no reply channel, so the ERROR line is the entire observable. Making it queryable is **F9** (the post-project-load health ping), deferred to its own phase. Scope limit worth knowing: the first-frame latch does not cover a mid-stream hwaccel renegotiation inside one `open()` — accepted, since 6(b) is wrong from frame 1 and CUEMS media is single-file constant-format.
  Drill: `cuems-RELATIONS/baselines/fp530-capacity/g8-truth-drill.py`. Plan: `Plans/2026-08-28-g8-truth-in-reporting-and-f9-ping.md`. ClickUp 869en65tm.
- **Looping-video first-loop freeze (FIXED engine-side).** A `loop=-1` cue played one loop, froze ~one loop on the last frame, then looped forever. Root cause was engine OSC ordering: `/mtcfollow=1` was sent at GO but `/loop=1` only after the postwait, so during that gap the layer followed MTC with `wraparound_` OFF → overshoot → clamp to last frame. Fixed in cuems-engine `rc_1` `41ab6bc` (`run_videoCue` sends `/loop` before `/mtcfollow` for infinite loops). It was **NOT** the HAP decoder's per-wrap buffer clears (benign, ~25ms recovery) and **NOT** a 30fps-MTC bug (MTC is 25fps; VC correctly translates 25fps MTC *time* to 30fps media *frame* numbers). Latent since engine `4efc1e7` (2026-03); only visible when postwait > clip length.
- **2s synchronized skip on ALL outputs (FIXED in mtcreceiver).** All outputs across both nodes + audio skipped in lockstep every ~2s. Cause: `libmtcmaster` (jitter_improvement build) emits a full-frame MTC SysEx resync every 50 frames (2s @25fps); mtcreceiver's QF-assembled `mtcHead` lags wire-MTC by a constant ~3 frames (Phase-2 commit `021b689` removed the `+2` compensation), so storing the FF into `mtcHead` unconditionally snapped +80–120ms every 2s → VC MIDISyncSource anti-drift hard-snapped. Fixed in mtcreceiver `rc_1` `aa44894` (hold the QF timebase on RESYNC, only reposition on genuine SEEK). VC side shipped on `main` `118e06d` (submodule bump + MIDISyncSource snap log + `resetWrapOffset()` in `resetAll()` + classifier tolerance 2→5); **VC deb 0.1.2-2** (also fixed `Depends: librtmidi1`→`librtmidi6`). See the mtcreceiver CLAUDE.md.
- **Engine/VC canvas mismatch is a KNOWN accepted limitation.** When `display.conf` declares more outputs than are physically connected, the engine canvas = bbox of ALL declared `[output:*]` regions while VC self-limits to *connected* DRM connectors. Cues targeting declared-but-disconnected outputs land outside the VC canvas — expected on partially-cabled rigs; do NOT trim display.conf ad hoc. Also: `canvas_region` (logical) ≠ scanout mode; e.g. an Acer V193HQV via HDMI→VGA runs native 1366×768 while its adapter-mangled EDID falsely advertises 1080p — don't force 1080p through such adapters.
- **Heap corruption when several video layers load at once (FIXED, `rc_1` `d5f08ff`).** Arming ≥3 video cues aborted the process (`double free or corruption`, alternating SIGABRT/SIGSEGV; 5× in 40 min on the FP530). **Two independent defects** — the obvious one is not the one that kills:
  1. `HardwareDecoder::detectAvailable()` gated on a plain `bool`, so all 4 `AsyncVideoLoader` workers passed the cold-cache gate together and `clear()`/`push_back()`ed the same `std::vector`.
  2. **`av_log_set_callback()` installs the callback PROCESS-WIDE.** Once any thread armed it, a thread merely running `avformat_open_input()` read *another* thread's `is_probing_hardware` and appended to the same shared vector. **Fatal on its own** — an instrumented run aborted with a single prober and zero concurrent probes, purely from non-probing threads writing the capture buffer. This is the reported backtrace (`avformat_open_input → av_log → callback → __libc_free`), and **serialising the probe does NOT fix it**. General rule: *never back an FFmpeg log callback with non-`thread_local` state.*
  Fix = `thread_local` capture state + `std::atomic` gate with double-checked `probe_mutex` + warm the cache in `AsyncVideoLoader::initialize()` before spawning workers. **Why it only ever bit in production:** `X11Display`/`WaylandDisplay` happen to probe during display init, but there was **no `detectAvailable` anywhere in `display/drm/`** — so DRM, the only path that matters, was the only one reaching the workers cold. That incidental X11 warm-up is exactly the kind of legacy-backend behaviour you must not rely on.
- **Reproducing that class of race: test2 CANNOT prove it — use the AMD FP530.** The reliable recipe is loading the **same file 4×** on a freshly started VC (with *different* files the workers reach the probe seconds apart and never collide; the "load 3-4 layers" wording in the ticket is unreliable). On Intel (test2/N100) the race fires ~9/10 but almost never aborts — its probe window is only ~4.4 ms — so a green run there means nothing. Measured A/B on the FP530, 4 concurrent OSC loads of one 4K clip per trial: **before 6/10 raced, 2/10 aborted; after 0/20, 0/20**. A second unprivileged VC instance is enough to test: its modeset is denied under libseat, so it never disturbs the running service.
- **Per-flip DRM framebuffer churn (FIXED, `rc_1` `83b1489` + `c2b74e8`) — and what its commit message overclaims.** `createFramebuffer()` filled `modifiers[]` unconditionally from `gbm_bo_get_modifier()`; when the BO carries no explicit modifier that returns `DRM_FORMAT_MOD_INVALID` (`0x00ffffffffffffff` — **not zero**) while `DRM_MODE_FB_MODIFIERS` is deliberately left unset, and the kernel rejects ADDFB2 with `-EINVAL` whenever a `modifiers[]` entry is non-zero and that flag is clear. Separately, the "rebuild if the BO changed" test missed on ~100% of flips because GBM rotates a small BO pool, so every frame of every output also paid a `drmModeRmFB` + `drmModeAddFB2` pair. Fix = fill `modifiers[]` only when the flag will actually be set (and skip the call the kernel is guaranteed to refuse), plus attach the fb id to the BO via `gbm_bo_set_user_data()` so `destroyFramebuffer()` must **no longer** `RmFB` — ownership moved to the BO, released by the destroy callback at `gbm_surface_destroy()`, which runs while the DRM fd is still open.
  **Measured effect (twin-binary ABBA A/B, 2026-08-14 — 6 s of `strace` per arm):**

  | box | arm | ADDFB2 | RMFB | atomic commits |
  |---|---|---|---|---|
  | AMD FP530 | before | **2154** | 1077 | 359 |
  | AMD FP530 | after | **0** | **0** | 360 |
  | Intel N97 | before | **1074** | 1074 | 358 |
  | Intel N97 | after | **0** | **0** | 360 |

  AMD is **2:1** — the rejected `AddFB2WithModifiers` *plus* the working fallback — so **`83b1489` is AMD-only**. Intel is **1:1**: the `WithModifiers` call succeeds first time on i915, so **`83b1489` is inert there** (byte-identical ioctl before and after). **`c2b74e8` helps both**, zeroing the per-flip pair: ~539 ioctls/s on AMD, ~716/s on Intel at 3×1080p60. Cost: CPU −9% (1×4K60) / −11% (2×4K60) on AMD, −14% (3×1080p60) on Intel, at identical `gfx` and 0 dropped frames across 12 rungs. Live fb count is flat (higher after — one per BO — but bounded by the pool, **not a leak**).
  ⚠️ **`83b1489`'s commit message says "stock FAIL 58 drops → fixed PASS 0". That does NOT reproduce and should not be cited.** It compared the packaged Debian binary against a local build — an A/B of two *builds*, not of the patch. With both arms built from one tree (BASE rebuild = only `DRMSurface.cpp` + link), **the pre-fix binary also passes 2×4K60 with 0 drops**. The CPU/ioctl win is real; **there is no capacity rescue**. General rule: *an A/B is only valid if both binaries are built the same way* — and stamp the binary's md5 into the result, or an arm label is an assertion rather than a measurement. Raw data: `cuems-RELATIONS/baselines/fp530-capacity/` (`AB-light-*`, `AB-heavy-*`, `AB-intel-*`), doc page 69maa-11632, ClickUp 869efh2f5.
- **N97 display ceiling** (source/measurement, 2026-06): sustains 2×4K@30 or 1×4K@60 with 0 drops but **hard-fails 3×4K@30** (render/EU-saturation, perf-limit reason `OTHER`, ~69°C — NOT thermal, NOT pl4). i3-1215U tops out at the same 2×4K@30 but for a thermal reason. HAP shifts every verdict down. Real lever: distribute outputs across nodes (1×4K@60/node proven, MTC-synced, 0 drops). `PresentationTiming` counts drops from DRM page-flip MSC deltas → `journalctl -u cuems-videocomposer | grep Dropped` is the accurate signal.
