# Building and installation

## CMake build (development)

### Dependencies

Install the development packages for:

- **FFmpeg:** libavformat, libavcodec, libavutil, libswscale
- **Display:** libx11, libxext, libxfixes, libgl1-mesa, libglew, libwayland, wayland-protocols, libdrm, libva, libva-drm, libva-x11, libegl, libgbm
- **Font/OSD:** libfreetype, libfontconfig
- **Audio/MIDI:** libasound, librtmidi
- **Other:** libsnappy, liblo (OSC), pkg-config, cmake

On Debian/Ubuntu you can install build dependencies used by the package:

```bash
sudo apt-get install \
  cmake pkg-config \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
  libx11-dev libxext-dev libxfixes-dev libxpm-dev libxv-dev libimlib2-dev \
  libgl1-mesa-dev libglew-dev \
  libfreetype6-dev libfontconfig1-dev \
  libasound2-dev librtmidi-dev libsnappy-dev \
  libwayland-dev wayland-protocols \
  libdrm-dev libva-dev libva-drm2-dev libva-x11-2-dev \
  libegl1-mesa-dev libgbm-dev liblo-dev python3
```

Exact package names may differ by distro; see `debian/control` in the repository for the list used by the Debian package.

### Configure and build

```bash
git clone https://github.com/cuems/cuems-videocomposer.git
cd cuems-videocomposer
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
sudo make install
```

By default the binary is installed under `/usr/local`. To install to `/usr` (e.g. to match the Debian layout):

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
```

### NDI support (optional)

To enable NDI input:

1. Install the [NDI SDK for Linux](https://ndi.video/) (e.g. to `/opt/NDI SDK for Linux/`).
2. Configure with NDI enabled and, if needed, the SDK path:

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_NDI=ON
# If NDI is not in the default path:
cmake .. -DCMAKE_BUILD_TYPE=Release -DENABLE_NDI=ON -DNDI_SDK_DIR=/path/to/ndi-sdk
make -j$(nproc)
sudo make install
```

Without the NDI SDK, the build completes but NDI sources will not be available.

### Running from the build directory

You can run the executable from `build/` without installing (e.g. `./cuems-videocomposer`). If NDI is enabled, ensure the NDI library can be found (e.g. set `LD_LIBRARY_PATH` to the NDI SDK lib directory, or use the install target which sets rpath when installed to the same prefix used at configure time).

---

## Debian package

### Prerequisites

- **NDI SDK** (optional but recommended for full feature set): place in `/opt/NDI SDK for Linux/` or set `NDI_SDK_DIR` to the SDK root. If not present, the package still builds but without NDI support.
- **Build dependencies:** install with `apt-get build-dep` if the package is in your apt sources, or install the list from `debian/BUILD-INSTRUCTIONS.md` in the repository.

### Build the package

From the repository root:

```bash
debuild -b -uc -us -nc
```

This produces a binary-only build (no source package), unsigned. For a full source+binary build:

```bash
debuild -us -uc
```

If the NDI SDK is not in the default location:

```bash
export NDI_SDK_DIR=/path/to/ndi-sdk
debuild -b -uc -us -nc
```

Output: `../cuems-videocomposer_<version>_amd64.deb` (and related files).

### Install the package

```bash
sudo dpkg -i ../cuems-videocomposer_*.deb
sudo apt-get install -f   # resolve dependencies if needed
```

### Full instructions and troubleshooting

For detailed steps, NDI compliance verification, and troubleshooting (missing NDI SDK, build deps, runtime library path), see `debian/BUILD-INSTRUCTIONS.md` in the repository.
