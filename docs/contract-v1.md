<!--
SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
SPDX-License-Identifier: GPL-3.0-or-later
SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
-->

# Videocomposer ↔ hardware-configuration manager — contract v1 (report-only)

This document is for the team building the CUEMS hardware-configuration manager. It describes what the
videocomposer (VC) offers on each node in **version 1**: a **read-only** view of the video hardware it
drives and of the configuration it runs. Writing configuration (outputs, modes, regions, blends, colour,
warp) is version 2 and comes later; nothing in v1 changes what is on screen.

`docs/api.md` (the whole VC surface) treats this as its management-plane chapter. Cue-time commands
(load, play, opacity, position, volume…) stay on the engine's OSC port and are not part of this contract.

## 1. Where and how

| | |
|---|---|
| Requests | NNG **Req/Rep**, `ipc:///tmp/cuems-videocomposer.ipc` — the VC listens, the manager dials |
| Events | NNG **Pub/Sub**, `ipc:///tmp/cuems-videocomposer-events.ipc` — the VC publishes, the manager subscribes |
| Payload | UTF-8 JSON, one document per message (events: a topic word, one space, then the JSON) |
| Scope | Local to the node. One manager per node; the controller's manager aggregates the others itself |
| Permissions | Socket files are created `0660`, owner `cuems`, group `cuems`. The manager must run as `root` or as a member of group `cuems` |
| Limits | Requests larger than 64 KiB are dropped. A reply not read within 1 s is dropped |
| Python | `pynng` (0.8.1 is already in `/usr/lib/cuems`). A reference client and a mock VC are in `tools/contract/` |

The VC serves the contract from the moment its display is up until it stops. If the socket cannot be
created (for example a second VC instance on the same node), the VC logs one WARNING and runs without the
contract; it never fails because of it.

**When there is no socket** (VC stopped, VC older than this contract, or contract disabled in its build) the
manager must fall back to what it knows from files (`/run/cuems/display.conf`) and retry later.

## 2. Requests and replies

Every request is a JSON object:

```json
{"v": 1, "id": "any JSON value you choose", "op": "get_hardware"}
```

Every reply echoes `id`:

```json
{"v": 1, "id": "…", "ok": true,  "result": { … }}
{"v": 1, "id": "…", "ok": false, "error": {"code": "…", "message": "…"}}
```

| `error.code` | When |
|---|---|
| `bad_request` | Not a JSON object, no string `op`, or no integer `v` (every op except `hello`) |
| `version_unsupported` | `v` is higher than the VC speaks; `error.max_v` says what it does speak |
| `unsupported_op` | Unknown `op` (for example a v2 op sent to a v1 VC) |

Versioning: `hello` ignores `v` and always answers in the v1 shape, so a client can always discover what the
VC speaks. New optional fields may appear in later VCs without a version change; clients must ignore
fields they do not know. Anything that changes meaning bumps `v`. Capabilities are listed by `get_caps`.

### 2.1 `hello`

```json
{"v": 1, "id": 1, "op": "hello"}
→ {"v":1,"id":1,"ok":true,"result":{"contract_versions":[1],"vc_version":"0.1.2-11","pid":20829,"hostname":"node01"}}
```

`pid` changes when the VC restarts: compare it with the `pid` in events to detect a restart.

### 2.2 `get_caps`

```json
→ {"result": {"caps": ["report_v1", "mode_timing", "interlace_flag"]}}
```

`hotplug` is **not** in v1's list yet: cable plug/unplug events will be announced by that token when they
exist. Until then, re-read `get_hardware` when you need fresh connector state.

### 2.3 `get_hardware`

Ask for it **when you connect, after a `state_changed` event and after a `pid` change** — not in a polling
loop. The reply is large (every mode of every connector with its timing, tens of kB on a three-output node)
and the hardware only changes with the configuration. **Keep any work on a video node light during
playback**: on test2 node01 (N97, 2×4K@50 + 1080p50, GPU at its PL4 power limit) a client parsing this reply
at 10 Hz — and the same parsing done by a plain script with no contract traffic at all — each produced one
~0.25–0.4 s presentation hitch per 10–20 minutes, while the idle node and `get_state` at 10 Hz produced none
(2026-10-21). The VC serves the reply from a cache; the cost is on the client's side of the same CPU. If you
need to poll, poll `get_state`.

One entry per connector of the GPU, connected or not (example values):

```json
{"outputs": [
  {"connector": "HDMI-A-1",
   "connection": "connected",
   "edid": {"make": "GSM", "model": "LG UltraFine", "serial": "506NTMXDV771"},
   "physical_mm": [600, 340],
   "current": {"mode_id": "3840x2160@30.00p", "width": 3840, "height": 2160,
               "refresh_hz": 30.0, "field_rate_hz": 30.0, "interlaced": false, "preferred": false,
               "timing": {"clock_khz": 297000, "hdisplay": 3840, "hsync_start": 4016, "hsync_end": 4104,
                          "htotal": 4400, "vdisplay": 2160, "vsync_start": 2168, "vsync_end": 2178,
                          "vtotal": 2250, "flags": 5, "type": 64}},
   "modes": [ { …same shape as current… }, … ]},
  {"connector": "HDMI-A-3", "connection": "disconnected", "edid": {"make":"","model":"","serial":""},
   "physical_mm": [0, 0], "current": null, "modes": []}
]}
```

- `connection`: `connected`, `disconnected` or `unknown` (the kernel could not tell; do not treat it as
  disconnected).
- `current` is the mode the VC **drives** on that connector, or `null` when the VC has no output there
  (disconnected, `enabled=false` in `display.conf`, or plugged after the VC started — the VC only builds its
  outputs at start).
- `refresh_hz` is the **frame** rate, `clock / (htotal × vtotal)`, two decimals as computed (59.94, not 60).
  For an interlaced mode it is half the field rate: 1080i60 reports `refresh_hz: 30.0, field_rate_hz: 60.0,
  interlaced: true`. Tools such as `modetest` print the field rate for those modes.
- `timing` is the raw DRM mode (`flags`, `type` are the `DRM_MODE_FLAG_*` / `DRM_MODE_TYPE_*` bits); v2 will
  address modes by it.
- `mode_id` is `"<w>x<h>@<refresh_hz>p"` or `…i`, unique **per connector**. When two modes would share it
  (CEA and CVT timings of the same size and rate) it gets `"/<clock_khz>"`, and `"#n"` if even that
  collides. Stable across VC restarts for the same monitor.
- EDID serial numbers are personal-ish data; the socket permissions keep them to `cuems`.

### 2.4 `get_state`

```json
{"mode": "show",
 "pacing": "coupled",
 "canvas": {"width": 7680, "height": 2160},
 "config_source": "/run/cuems/display.conf",
 "show_loaded": true,
 "outputs": [
   {"connector": "HDMI-A-1", "enabled": true, "region": [0, 0, 3840, 2160],
    "blend": {"left": 0, "right": 0, "top": 0, "bottom": 0, "gamma": 2.2},
    "mode_id": "3840x2160@30.00p"}
 ]}
```

- `mode` is always `"show"` in v1. A future preview/edit mode (switched by the engine when the editor
  enters a preview) will show here.
- `pacing`: `coupled` (all outputs present together, paced by the slowest; the default) or `per_surface`.
- `config_source`: the file the VC started from, or `drm_defaults` when there was none.
- `show_loaded`: the engine's lock file `/tmp/cuems.show.lock` exists. The engines create it when a
  project loads; after an engine crash it can be stale. In v2 configuration writes
  will be refused while it is present.
- `outputs` lists the outputs of the VC's canvas; `region` is `[x, y, width, height]` in canvas pixels.

## 3. Events

Subscribe to all topics (empty prefix) or to the topic words you want. Each message is
`"<topic> <json>"`:

```
state_changed {"v":1,"seq":12,"pid":20829,"event":"state_changed"}
mode_warning  {"v":1,"seq":13,"pid":20829,"event":"mode_warning","kind":"mixed_refresh",
               "detail":"setResolutionMode: HDMI-A-1=60.00Hz, HDMI-A-2=59.94Hz"}
```

| Topic | Meaning |
|---|---|
| `state_changed` | The configuration changed (an operator tool or OSC command changed a region, blend or mode). Call `get_state` (and `get_hardware` if modes matter). |
| `mode_warning` | `kind: "mixed_refresh"` — the outputs run at different rates (see the VC's guarantee: stutter-free per output, no frame lock across a seam of unequal rates). More kinds may appear. |

Pub/Sub delivers nothing to a subscriber that is not connected yet and may drop messages for a slow one.
So the client must:
1. subscribe, then call `get_hardware` and `get_state` (never rely on events for the initial state);
2. watch `seq` (per VC process, from 1, no gaps unless messages were lost) and `pid` (a new `pid` means the VC
   restarted: re-read everything);
3. on any gap, re-read.

## 4. What is not in v1

Writing anything; edit mode; cable hotplug events (`hotplug` cap); node identity in `hello` (the manager
knows its own node). Each arrives behind a `caps` token or a `v` bump.
