<!--
SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
SPDX-License-Identifier: GPL-3.0-or-later
SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
-->

# Cold-boot mode-verification campaign — runbook

ClickUp task: [869d47uvr](https://app.clickup.com/t/869d47uvr)

## What this tests

The i915 cold-boot 4K-vs-1080p race: `drmModeSetCrtc(... 1920x1080 ...)`
returns 0 and `drmModeGetCrtc` reports the requested mode, but the HDMI link
is still in its pre-init 4K state and the panel keeps scanning 4K. The 1080p
framebuffer ends up in the upper-left quadrant of a 4K signal.

The branch `fix/cold-boot-modeset-verification` adds:

1. Explicit `drmSetMaster` in the libseat path (hygiene).
2. A `verifyCrtcMode` helper applied at every `drmModeSetCrtc` call site.
3. A first-frame in-process retry (CRTC disable → 250 ms settle → re-enable)
   when the verifier mismatches.
4. Fatal-error propagation through the run loop so a still-mismatched
   modeset exits the process and `Restart=on-failure` recovers with a fresh
   one.

`drmModeGetCrtc` cannot tell us whether the panel is actually scanning the
requested mode — the kernel's own readback can lie. The probe image is the
runtime ground truth: a single panel photograph during the splash window
unambiguously distinguishes correct 1080p from the cold-boot bug.

## Prerequisites

- The affected node (e.g. `000000000002` locally, or `10.16.10.2` for the
  reference production node).
- `cuems-videocomposer.service` enabled at boot.
- A camera (phone is fine) to photograph the panel during the splash window.
- A way to **fully power-cycle** the node (not warm reboot — different
  failure surface).

## Procedure

### 1. Build with the probe image

```sh
cd /home/stagelab/src/cuems-videocomposer/build
cmake -DCUEMS_PROBE_SPLASH=ON ..
make -j$(nproc)
sudo make install
```

The CMake STATUS line `CUEMS_PROBE_SPLASH=ON — embedding cold-boot probe
image (resources/probe-1080p.png)` confirms the swap took.

Verify the binary actually embedded the probe:

```sh
strings build/cuems-videocomposer | grep -c "splash_png_len"  # sanity
```

(There's no in-binary marker that distinguishes splash vs probe directly —
the easiest verification is the next cold boot.)

### 2. Run 20 cold boots

Per cold boot:

1. Power-cycle the node (not `reboot` — full power off).
2. Wait for it to come up (LEDs / display signal).
3. **Within the first ~10 seconds after the panel lights up, photograph it.**
   This is the splash window — it shows the probe image instead of the
   brand splash.
4. SSH in once the system is reachable.
5. Run the per-boot capture script:

   ```sh
   cd /home/stagelab/src/cuems-videocomposer/tests
   ./cold-boot-probe-run.sh
   ```

   The script prompts you for `PASS / FAIL / SKIP` based on the photograph,
   captures the journal and `i915_display_info` debugfs dump, and appends
   to `tests/cold-boot-results.csv`.

   debugfs is root-only, so the script uses `sudo cat ...` if not run as
   root. If you don't want a sudo prompt per boot, add this sudoers entry:

   ```
   cuems ALL=(ALL) NOPASSWD: /bin/cat /sys/kernel/debug/dri/0/i915_display_info
   ```

### 3. Photograph classification

| Verdict | What you see on the panel |
|---|---|
| **PASS** | Red border at the panel EDGES. All four colored corner squares (cyan top-left, magenta top-right, yellow bottom-left, green bottom-right) at the panel corners. "1920 × 1080" centered on the FULL panel. |
| **FAIL** | Red border at the 1/4 mark — far from the panel edges. All four colored corners clustered in the top-left QUADRANT of the panel. The rest of the panel is mostly black (kernel fbcon at 4K). |
| **SKIP** | Couldn't get a usable photo this boot (forgot, panel off, etc.). Re-do this boot. |

A FAIL means the cold-boot race fired and was not recovered. Cross-reference
with the journal capture:

- `verifyCrtcMode MISMATCH` → the in-kernel readback also disagreed, the
  retry was attempted.
- `cold-boot retry succeeded` → the in-process retry caught it. Should be
  PASS in the photo. If it's FAIL, the readback lies — see "What if the
  retry log says success but the photo says FAIL" below.
- `cold-boot retry still mismatched … marking fatal` + "Display backend
  reports fatal error — exiting for systemd restart" + a *second* service
  start in the journal → `Restart=on-failure` fired. Should be PASS in the
  photo from the second start. If it's FAIL even after the restart,
  the i915 PHY is fully wedged — the rebind workaround is required.

### 4. Decision matrix (after 20 boots)

Based on the PASS/FAIL tally and the journal classification:

- **0% FAIL AND in-process retry recovery rate ≥ 80%** → keep the retry.
  File a follow-up task to retire `cuems-i915-rebind.sh`.
- **0% FAIL AND in-process retry recovery rate < 80%** → drop the retry in
  a follow-up; the `Restart=on-failure` path is doing all the work. File a
  follow-up to retire the rebind.
- **Any FAIL cases** → wire `cuems-i915-rebind.sh` into the unit (per-node
  flag-gated via `/etc/cuems/videocomposer-flags.env REBIND_I915_ON_START=true`)
  as a separate task. Do not retire the rebind.

### 5. Revert to production splash

```sh
cd /home/stagelab/src/cuems-videocomposer/build
cmake -DCUEMS_PROBE_SPLASH=OFF ..
make -j$(nproc)
sudo make install
```

Confirm with one more boot that the brand splash returns to its normal look
before leaving the node.

## What if the retry log says "succeeded" but the photo says FAIL?

That's the failure mode the design review warned about: `drmModeGetCrtc`
can return the requested mode while the panel is still scanning the
previous one. In that case:

- The in-process retry's verifier passes (false negative on the bug).
- The fatal flag is never set.
- `Restart=on-failure` doesn't fire.
- The user sees a wrong-mode panel until the next manual restart.

If this shows up in the data (PASS in journal, FAIL in photo), the
verifier alone is insufficient and the rebind workaround stays — gate it
on the per-node config flag and treat the verifier as in-band telemetry,
not a self-heal mechanism.

## Plumbing test (no real cold-boot needed)

Before the 20-boot campaign, you can verify the verify → retry → fatal →
systemd-restart chain end-to-end without waiting for the actual race to
fire. The binary honours an env var **`CUEMS_VC_FORCE_VERIFY_FAIL=N`** that
makes the next *N* calls to the cold-boot verifier return `false`. The env
var is consumed once per process — restart re-arms.

For the typical 3-output config:

| `N` | Expected result | What you should see in `journalctl -u cuems-videocomposer` |
|---|---|---|
| **3** | All three surfaces' first verify forced false → all three trigger retry → retry uses real verify which passes. | Three `CUEMS_VC_FORCE_VERIFY_FAIL — forcing first-frame verify failure` lines, three `cold-boot retry succeeded` lines. Service stays up. |
| **6** | First AND retry verify forced false on every surface → `fatalModeset_` set on all → run loop exits → `Restart=on-failure` brings up a fresh process (where the env var fires again under systemd → infinite loop unless you remove it). | `marking fatal for systemd restart`, `Display backend reports fatal error — exiting for systemd restart`, then a *second* unit start in the journal. |

Because `=6` causes a restart loop under systemd if the env var stays set,
**do the plumbing tests with the service stopped and run the binary directly
as the `cuems` user** (a wrapper is included):

```sh
sudo systemctl stop cuems-videocomposer.service
tests/cold-boot-probe-plumbing-test.sh 3      # retry-success path
tests/cold-boot-probe-plumbing-test.sh 6      # fatal-exit path (process exits non-zero)
sudo systemctl start cuems-videocomposer.service
```

The script runs the binary inline (no daemonisation, foreground stdout/stderr),
captures the output to `tests/cold-boot-captures/<ts>_plumbing_N<n>.txt`, and
reports the exit code. With `N=6` the binary should exit non-zero within a
second; with `N=3` you'll need to Ctrl-C after you see the retry log lines.

## Files this campaign produces

- `tests/cold-boot-results.csv` — one row per boot, with verdict and
  paths to the journal/debugfs captures.
- `tests/cold-boot-captures/<timestamp>_journal.txt` — per-boot journal.
- `tests/cold-boot-captures/<timestamp>_i915_display_info.txt` — per-boot
  pipe-state dump.
- Photographs (kept on operator device — annotate with timestamp and
  PASS/FAIL to correlate with the CSV).
