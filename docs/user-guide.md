# CUEMS Video Composer – User guide

## Starting the application

By default the application shows a startup logo (splash) on all outputs for a few seconds, then enters the main display. To skip the splash (e.g. for scripting or headless use), run with:

```bash
cuems-videocomposer --no-splash
```

Use `--fullscreen` to start in fullscreen and `--ontop` to keep the window above others.

## Loading videos and layers

- **Single file at startup:** pass the path as a positional argument, e.g. `cuems-videocomposer video.mp4`.
- **Via OSC:** send `/videocomposer/load` with a file path (string) to load a video, replacing the current one.
- **Multiple layers:** use OSC layer commands to add, load, and manage layers. Add a layer with `/videocomposer/layer/add` (path), load a file into a layer with `/videocomposer/layer/load` (filepath, cueId) or `/videocomposer/layer/<id>/file` (path). Reorder with `/videocomposer/layer/reorder`, set opacity and visibility per layer.

For the full list of layer and application OSC paths, see the [OSC reference](../OSC_CONTROLS_SUMMARY.md).

## Timecode sync

- **MTC (MIDI Time Code):** The app can lock playback to MTC. By default it tries to connect to the “Midi Through” ALSA port. Use `--midi PORT` to set a specific ALSA sequencer port (or `-1` for autodetect). Use `aconnect -l` to list ports. OSC: `/videocomposer/midi/connect` (string = port name) and `/videocomposer/midi/disconnect`.
- **LTC:** LTC sync is supported when built with libltc; connect the LTC source to the configured input.

## Remote control

Enable OSC with `--osc PORT` (default 7000). Main use cases:

- Load/seek: `/videocomposer/load`, `/videocomposer/seek`
- Layer management: `/videocomposer/layer/add`, `/videocomposer/layer/load`, `/videocomposer/layer/<id>/offset`, `/videocomposer/layer/<id>/opacity`, etc.
- OSD: `/videocomposer/osd/timecode`, `/videocomposer/osd/text`
- Quit: `/videocomposer/quit`

See the [OSC reference](../OSC_CONTROLS_SUMMARY.md) for the complete API.

## Display and resolution

- **Resolution mode:** `--resolution MODE` with `native`, `maximum`, `1080p`, `720p`, or `4k`. Default is `1080p`.
- **Backends:** On Linux the app can use DRM/KMS (direct, no X/Wayland), X11, or Wayland. It picks an available backend; DRM is typically used for dedicated display outputs.
- **Fullscreen:** `--fullscreen` starts fullscreen; ontop with `--ontop`.

## Hardware decoding

Use `--hw-decode MODE` to choose the decoder:

- `auto` (default) – use GPU when available (VAAPI, CUDA, QSV).
- `vaapi` – Intel/AMD GPUs on Linux.
- `cuda` – NVIDIA GPUs.
- `software` – CPU only (slower, maximum compatibility).

Switch to `software` if you see decode errors or need a codec not supported by the GPU.

## NDI

NDI input is supported when the application is built with the NDI SDK. You can discover and use NDI sources as inputs; the NDI SDK is bundled in the Debian package. For setup and testing, see [tests/NDI_QUICK_START.md](../tests/NDI_QUICK_START.md) and [tests/README_NDI.md](../tests/README_NDI.md).

## HAP

HAP (and HAP Q, HAP Q Alpha) is supported for low-latency, GPU-friendly playback. Use HAP-encoded files for best performance in multi-layer setups. For creating and testing HAP files, see [HAP_TESTING_GUIDE.md](../HAP_TESTING_GUIDE.md) and [tests/README_CODEC_TESTS.md](../tests/README_CODEC_TESTS.md).

## Command-line options (summary)

Common options:

| Option | Description |
|--------|-------------|
| `-h`, `--help` | Show help and exit |
| `-V`, `--version` | Print version |
| `-O`, `--osc PORT` | OSC port (default 7000) |
| `-m`, `--midi PORT` | MIDI port for MTC (-1 = autodetect) |
| `-f`, `--fps FPS` | Playback framerate |
| `-o`, `--offset N` | Time offset in frames |
| `-s`, `--fullscreen` | Start fullscreen |
| `-a`, `--ontop` | Window on top |
| `--hw-decode MODE` | `auto`, `vaapi`, `cuda`, `software` |
| `-r`, `--resolution MODE` | `1080p`, `720p`, `4k`, `native`, `maximum` |
| `--no-splash` | Disable startup logo |
| `-q`, `--quiet` | Less output |
| `-v`, `--verbose` | More output |

Run `cuems-videocomposer --help` for the full list and MIDI/remote-control notes.
