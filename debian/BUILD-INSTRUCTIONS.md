# Building Debian Package with NDI Compliance

## Prerequisites

1. **NDI SDK** must be available in one of these locations:
   - `/opt/NDI SDK for Linux/` (default)
   - Or set `NDI_SDK_DIR` environment variable

2. **Build dependencies** (install with `apt-get build-dep` or manually):
   ```bash
   sudo apt-get install \
     debhelper cmake pkg-config \
     libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
     libx11-dev libxext-dev libxfixes-dev \
     libgl1-mesa-dev libglew-dev \
     libfreetype6-dev libfontconfig1-dev \
     libasound2-dev librtmidi-dev \
     libsnappy-dev libopenmp-dev \
     libwayland-dev wayland-protocols \
     libdrm-dev libva-dev libva-drm2-dev libva-x11-2-dev \
     libegl1-mesa-dev libgbm-dev
   ```

## Building the Package

### Option 1: Using dpkg-buildpackage (Simple)

```bash
cd /path/to/cuems-videocomposer
dpkg-buildpackage -us -uc
```

This will create:
- `../cuems-videocomposer_0.1.0-1_amd64.deb` - Binary package
- `../cuems-videocomposer_0.1.0-1.dsc` - Source package description
- `../cuems-videocomposer_0.1.0-1.tar.xz` - Source tarball

### Option 2: Using debuild (Recommended)

```bash
cd /path/to/cuems-videocomposer
debuild -us -uc
```

This performs additional checks and is the preferred method.

### Option 3: With NDI_SDK_DIR Environment Variable

If NDI SDK is not in `/opt/NDI SDK for Linux/`:

```bash
export NDI_SDK_DIR=/path/to/ndi-sdk
cd /path/to/cuems-videocomposer
dpkg-buildpackage -us -uc
```

## Installing the Package

After building:

```bash
sudo dpkg -i ../cuems-videocomposer_0.1.0-1_amd64.deb
```

If there are missing dependencies:

```bash
sudo apt-get install -f
```

## Verifying NDI Compliance

After installation, verify:

```bash
# Check NDI libraries are in private directory
ls -la /usr/lib/cuems-videocomposer/libndi.so*

# Check rpath is set correctly
readelf -d /usr/bin/cuems-videocomposer | grep RPATH
# Should show: /usr/lib/cuems-videocomposer

# Check documentation is installed
ls -la /usr/share/doc/cuems-videocomposer/NDI-COMPLIANCE/

# Check copyright file includes NDI section
grep -A 5 "NDI SDK" /usr/share/doc/cuems-videocomposer/copyright
```

## Troubleshooting

### NDI SDK Not Found During Build

If you see:
```
WARNING: NDI SDK libraries not found. Package will be built without NDI support.
```

Solutions:
1. Install NDI SDK to `/opt/NDI SDK for Linux/`
2. Or set `NDI_SDK_DIR` environment variable before building
3. The package will still build, but without NDI support

### Missing Build Dependencies

If build fails with missing dependencies:
```bash
sudo apt-get build-dep cuems-videocomposer
```

Or install manually from the list in Prerequisites.

### Runtime Library Not Found

If application can't find NDI libraries at runtime:
1. Check rpath: `readelf -d /usr/bin/cuems-videocomposer | grep RPATH`
2. Verify libraries exist: `ls -la /usr/lib/cuems-videocomposer/libndi.so*`
3. Check CMakeLists.txt rpath configuration

## Package Structure

The built package will contain:

```
/usr/bin/cuems-videocomposer          # Main executable
/usr/lib/cuems-videocomposer/         # Private directory
  └── libndi.so*                      # NDI SDK libraries
/usr/share/doc/cuems-videocomposer/
  ├── copyright                        # Copyright file (includes NDI)
  ├── README.Debian                    # Debian-specific docs
  └── NDI-COMPLIANCE/
      ├── EULA                         # NDI SDK EULA
      └── README                       # NDI information
```

## Compliance Checklist

✅ NDI libraries in private directory (not system paths)
✅ Complete EULA with all required terms
✅ Trademark attribution (NDI®)
✅ Website links (ndi.video)
✅ Copyright notices
✅ No NDI Tools bundled (link provided)
✅ Runtime library path configured (rpath)

See `debian/NDI-COMPLIANCE-SUMMARY.md` for detailed compliance information.

