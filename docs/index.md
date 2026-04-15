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
- **OSD** – on-screen timecode and overlays

## Quick start

```bash
# From Debian package
sudo apt install ./cuems-videocomposer_*.deb
cuems-videocomposer --osc 7000 /path/to/video.mp4
```

Or build from source — see [Building and installation](building.md).

## Documentation

| Document | Description |
|----------|-------------|
| [User guide](user-guide.md) | Starting the app, layers, sync, OSC, display, NDI, HAP |
| [Building and installation](building.md) | CMake build and Debian package build/install |
| [Architecture](architecture.md) | Input-to-render flow, components, design principles |
| [API Reference](api.md) | Auto-generated C++ class and method documentation |

## Release notes

Please refer to the repository for [release notes](https://github.com/stagesoft/cuems-videocomposer/releases).
