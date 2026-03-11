# CUEMS Video Composer – Documentation

Index of documentation for users and developers.

## For users

| Document | Description |
|----------|-------------|
| [User guide](user-guide.md) | Starting the app, loading videos, layers, timecode sync, OSC, display, hardware decode, NDI, HAP |
| [OSC reference](../OSC_CONTROLS_SUMMARY.md) | Full list of OSC paths and arguments |
| [Building and installation](building.md) | CMake build and Debian package build/install |
| [Running as a service](../contrib/README.md) | Systemd unit for cuems-commons (DRI at boot) |

## For developers

| Document | Description |
|----------|-------------|
| [Architecture](../ARCHITECTURE.md) | Input-to-render flow, components (VideoLayer, SyncSource, DisplayBackend), design principles |

Additional design and implementation notes (evaluations, plans, status) live as `.md` files in the repository root; they are historical/developer notes rather than user-facing docs.

## Other docs in this repo

- [docs/VIRTUAL_CANVAS_IMPLEMENTATION_PLAN.md](VIRTUAL_CANVAS_IMPLEMENTATION_PLAN.md) – virtual canvas feature plan
- [debian/BUILD-INSTRUCTIONS.md](../debian/BUILD-INSTRUCTIONS.md) – Debian/NDI build details and troubleshooting
- [tests/README.md](../tests/README.md) – integration tests; [tests/NDI_QUICK_START.md](../tests/NDI_QUICK_START.md), [tests/README_NDI.md](../tests/README_NDI.md) – NDI testing
