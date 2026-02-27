# CUEMS Video Composer

Multi-layer video compositor for live production, playback, and timecode sync.

## Features

- **Multi-layer composition** – stack and blend multiple video layers
- **MTC/LTC timecode sync** – lock playback to MIDI Time Code or LTC
- **NDI input** – receive NDI sources (NDI SDK bundled in Debian package)
- **Hardware decoding** – VAAPI (Intel/AMD), CUDA (NVIDIA), QSV (Intel Quick Sync)
- **HAP codec** – direct GPU texture upload for low-latency playback
- **Remote control** – OSC (default port 7000), IPC, message queues
- **Display** – DRM/KMS, X11, Wayland; configurable resolution (1080p, 4k, native)
- **Startup splash** – optional logo on all outputs (`--no-splash` to disable)
- **OSD** – on-screen timecode and overlays

## Quick start

```bash
# From Debian package
sudo apt install ./cuems-videocomposer_*.deb
cuems-videocomposer --osc 7000 /path/to/video.mp4
```

Or run from a local build (see [Building](#building)).

## Installation

Install the `.deb` package:

```bash
sudo dpkg -i cuems-videocomposer_*.deb
sudo apt-get install -f   # install dependencies if needed
```

To run as a system service at boot (DRI/GPU), use the systemd unit from [contrib/](contrib/README.md); it is intended for **cuems-commons**, not this package.

## Usage

Key options:

| Option | Description |
|--------|-------------|
| `--osc PORT` | Enable OSC on port (default 7000) |
| `--midi PORT` | MIDI port for MTC (-1 = autodetect) |
| `--hw-decode MODE` | `auto`, `vaapi`, `cuda`, or `software` |
| `--resolution MODE` | `1080p`, `720p`, `4k`, `native`, `maximum` |
| `--no-splash` | Disable startup logo |
| `--fullscreen` | Start fullscreen |

Example:

```bash
cuems-videocomposer --osc 7000 --resolution 1080p --fullscreen video.mov
```

Run `cuems-videocomposer --help` for the full list. For OSC commands and layer control, see [OSC reference](OSC_CONTROLS_SUMMARY.md) and [User guide](docs/user-guide.md).

## Building

**CMake** (development build):

- Dependencies: FFmpeg (libavformat, libavcodec, libavutil, libswscale), X11/Wayland, OpenGL, DRM, VAAPI, ALSA, librtmidi, liblo, and others (see [docs/building.md](docs/building.md)).
- Optional: NDI SDK for NDI input (`-DENABLE_NDI=ON`, `-DNDI_SDK_DIR=...`).

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
sudo make install
```

**Debian package:**

```bash
debuild -b -uc -us -nc
```

NDI SDK must be available (e.g. `/opt/NDI SDK for Linux/` or `NDI_SDK_DIR`). Full steps and troubleshooting: [docs/building.md](docs/building.md) and [debian/BUILD-INSTRUCTIONS.md](debian/BUILD-INSTRUCTIONS.md).

## Documentation

| Document | Description |
|----------|-------------|
| [docs/README.md](docs/README.md) | Documentation index |
| [docs/user-guide.md](docs/user-guide.md) | Usage, layers, sync, OSC, display, NDI, HAP |
| [docs/building.md](docs/building.md) | CMake and Debian build/install |
| [OSC_CONTROLS_SUMMARY.md](OSC_CONTROLS_SUMMARY.md) | Full OSC API reference |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Input/render flow and components |
| [contrib/README.md](contrib/README.md) | Systemd service (cuems-commons) |

## License and credits

- **CUEMS Video Composer:** LGPL-3.0 (see [debian/copyright](debian/copyright)).
- Based on **xjadeo**; see source files for xjadeo copyright.
- **NDI** support uses NDI SDK under its license; NDI® is a trademark of Vizrt NDI AB. See `/usr/share/doc/cuems-videocomposer/NDI-COMPLIANCE/` when installed.

Report bugs: [GitHub issues](https://github.com/cuems/cuems-videocomposer/issues)
