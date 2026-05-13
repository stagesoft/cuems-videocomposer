<!--
SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
SPDX-License-Identifier: GPL-3.0-or-later
SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
-->

# resources/

Image assets embedded into the videocomposer binary at build time via
`cmake/embed_splash.py` (xxd-style C array generator).

## Files

| File | Purpose |
|---|---|
| `splash.png` | Brand splash logo. Embedded by default. Shown for `SPLASH_DURATION_SECONDS` (10 s) on every output at startup via `StartupSplash`. |
| `probe-1080p.png` | Cold-boot mode-verification probe (1920×1080). Embedded **only** when `-DCUEMS_PROBE_SPLASH=ON`. Visual ground truth for whether a 1080p modeset actually landed at 1080p on the panel, or got pinned to the top-left quadrant of a 4K signal due to the i915 cold-boot HDMI PHY race. |

## Cold-boot probe image

`probe-1080p.png` is regenerated from a Python script (no ImageMagick / Pillow
dependency — uses only stdlib `zlib` + `struct` so it's reproducible from a
clean checkout):

```sh
python3 cmake/generate_probe_png.py resources/probe-1080p.png
```

The image is 1920×1080 with:

- 100 px solid red border on all four sides
- Four 200×200 colored corner squares **inside** the border:
  cyan (TL), magenta (TR), yellow (BL), green (BR)
- Centered black "1920 × 1080" label, ~270 px tall, drawn in a 7-segment-style
  font with a real diagonal `×` glyph
- White background between markers

### Why those features

The probe is designed so that **a single panel photograph** distinguishes a
correct 1080p modeset from a silent 4K-with-1080p-in-top-left modeset:

- **PASS (1080p)** — red border at the panel edges, all four colored corners
  at the panel corners, "1920 × 1080" centered on the full panel.
- **FAIL (cold-boot bug)** — red border at the 1/4 mark (far from the panel
  edge), all four colored corners clustered in the top-left quadrant, the
  rest of the panel showing fbcon black/garbage from the kernel's 4K
  framebuffer.

This is the runtime ground truth that `drmModeGetCrtc` cannot provide: the
kernel's own readback can report the requested mode while the HDMI link is
still scanning the previous one.

## Test-build workflow

Switch to the probe (test campaign):

```sh
cd build && cmake -DCUEMS_PROBE_SPLASH=ON .. && make -j$(nproc) && sudo make install
```

Run the cold-boot validation protocol — see `tests/COLD_BOOT_PROBE.md`.

Revert to the brand splash (production):

```sh
cd build && cmake -DCUEMS_PROBE_SPLASH=OFF .. && make -j$(nproc) && sudo make install
```

The same `splash_png[]` symbol is generated either way, so `StartupSplash`
needs no code change.
