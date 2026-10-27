<!--
SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
SPDX-License-Identifier: GPL-3.0-or-later
SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
-->

# API Reference

Auto-generated from C++ source documentation.

## Core

::: cuems-videocomposer.VideoComposerApplication

## Layers

::: cuems-videocomposer.VideoLayer
::: cuems-videocomposer.LayerManager
::: cuems-videocomposer.LayerProperties

## Display

::: cuems-videocomposer.DisplayBackend
::: cuems-videocomposer.MultiOutputRenderer
::: cuems-videocomposer.VirtualCanvas

## Input

::: cuems-videocomposer.InputSource
::: cuems-videocomposer.VideoFileInput
::: cuems-videocomposer.NDIVideoInput
::: cuems-videocomposer.HAPVideoInput

## Hardware Decoding

::: cuems-videocomposer.HardwareDecoder
::: cuems-videocomposer.VaapiInterop

## Sync

::: cuems-videocomposer.SyncSource
::: cuems-videocomposer.FramerateConverterSyncSource

## Remote Control

::: cuems-videocomposer.OSCRemoteControl

### `/videocomposer/display/color` — per-output color

```
/videocomposer/display/color <connector> brightness|contrast|saturation|hue|gamma <value>
/videocomposer/display/color <connector> lut <name.cube>
/videocomposer/display/color <connector> lut none
/videocomposer/display/color <connector> reset
/videocomposer/display/color <connector> dump
/videocomposer/display/color dump
```

| Parameter | Range | Identity | Notes |
|---|---|---|---|
| `brightness` | −1 … 1 | 0 | added to R, G, B |
| `contrast` | 0 … 2 | 1 | around 0.5 |
| `saturation` | 0 … 2 | 1 | 0 = grey |
| `hue` | degrees | 0 | folded into −180 … 180 |
| `gamma` | 0.1 … 3 | 1 | `pow(x, 1/g)`: g > 1 brightens |
| `lut` | bare name | none | a 3D `.cube` file (size 2–65) in `/etc/cuems/luts/`. No `/` and no `..` are allowed, and the name must end in `.cube` |

- This is the color of one **output** of the virtual canvas (DRM backend), applied in the output blit in this order: trims → LUT → edge-blend ramp. It is separate from the per-layer and master color.
- An out-of-range value is clamped, with a WARNING naming the value used. A non-finite or unparsable value changes nothing (WARNING).
- `lut none` clears only the LUT. `reset` sets the five trims to identity and clears the LUT.
- If a LUT fails to load, the previous LUT stays (ERROR: `… NOT APPLIED: <reason> — previous LUT <name> kept`).
- **Volatile.** Each change logs the output's paste-ready `display.conf` line (`OutputColor: <conn> color_contrast=1.08 … (applied live — volatile: paste into /etc/cuems/display.conf to keep it)`). The persistent form is the `color_*` keys in `/etc/cuems/display.conf`, applied at start; see `CLAUDE.md`, "Per-output color (phase 3)".
- `dump` logs `OutputColor: <conn> <keys> (dump: lut_status=none|ok|missing|invalid applied=yes|no)` for one output or for every output.
- Per-output color **survives `/videocomposer/reset`**, which clears only layers and master properties. It also survives `display/blend`, `display/region` and `display/mode`.
- On a backend without the virtual canvas: ERROR `display/color: per-output color is not supported by this display backend`.
- There is no reply on the wire: every outcome is a journal line.

### Display commands refused under the show lock

While `/tmp/cuems.show.lock` exists (a project is loaded), these commands are **refused**: `display/color` (except `dump`), `display/blend`, `display/region`, `display/mode`, `display/resolution_mode` and `display/load`. Nothing changes, and a WARNING is logged:

```
<command> REFUSED: /tmp/cuems.show.lock exists — a project is loaded, or the lock was left by an engine older than cuems-engine 0.1.0rc6-6 or by a crashed engine; unload the project (engine ≥ rc6-6) or stop the engines, then retry; color is also applied from display.conf at the next start
```

- Not gated: `display/color … dump`, `output/list`, `display/list`, `display/modes`, `display/save` and `display/assign`.
- This is a behaviour change: `display/blend`, `display/region`, `display/mode`, `display/resolution_mode` and `display/load` were not gated before. It needs **cuems-engine ≥ 0.1.0rc6-6** on every box: older engines remove the lock only when they stop, not on unload.

`output/list` logs one `output_color <conn> <keys> lut_status=… applied=yes|no` line per output, after the `region` lines. Those `region` lines are unchanged.

## Video

::: cuems-videocomposer.FrameBuffer
