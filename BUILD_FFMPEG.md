# Building Custom FFmpeg for cuems-videocomposer

This guide explains how to build a custom FFmpeg with all the hardware acceleration features needed for cuems-videocomposer.

## Current System Status

Your system has:
- **Intel Iris Xe Graphics** (TigerLake)
- **VAAPI** support (working)
- **QSV** support available (`h264_qsv` decoder exists)
- **Missing**: `h264_vaapi` decoder in current FFmpeg build

## Required FFmpeg Features for cuems-videocomposer

### Core Libraries (Required)
- `libavformat` - Container format support
- `libavcodec` - Codec support (decoding/encoding)
- `libavutil` - Utility functions
- `libswscale` - Image scaling/conversion

### Hardware Acceleration (Recommended)
- **VAAPI** - Linux hardware acceleration (Intel/AMD)
- **QSV (libmfx)** - Intel Quick Sync Video
- **CUDA** - NVIDIA GPU acceleration (optional)
- **VideoToolbox** - macOS hardware acceleration (optional)
- **DXVA2** - Windows hardware acceleration (optional)

### Codec Support
- **H.264** - Primary video codec (with hardware decoders: `h264_vaapi`, `h264_qsv`, `h264_cuvid`)
- **HEVC/H.265** - High efficiency codec
- **AV1** - Modern codec (optional)
- **HAP** - Hardware Accelerated Performance codec (for VJ use)

## Build Instructions

### 1. Install Dependencies

```bash
# Debian/Ubuntu
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    pkg-config \
    yasm \
    nasm \
    cmake \
    git \
    libva-dev \
    libva-drm2-dev \
    libva-x11-2-dev \
    libmfx-dev \
    libvdpau-dev \
    libx264-dev \
    libx265-dev \
    libvpx-dev \
    libmp3lame-dev \
    libopus-dev \
    libvorbis-dev \
    libfreetype6-dev \
    libfontconfig1-dev \
    libfribidi-dev \
    libass-dev \
    libbluray-dev \
    libwebp-dev \
    libopenjp2-7-dev \
    libdav1d-dev \
    libdrm-dev \
    libx11-dev \
    libxext-dev \
    libxfixes-dev
```

### 2. Download FFmpeg Source

```bash
cd ~/src
git clone https://git.ffmpeg.org/ffmpeg.git
cd ffmpeg
# Use latest stable release or master
git checkout release/6.1  # or latest stable
```

### 3. Configure FFmpeg

Create a configuration script with all required options:

```bash
#!/bin/bash
# configure_ffmpeg.sh

./configure \
    --prefix=/usr/local/ffmpeg-cuems \
    --enable-shared \
    --enable-static \
    --enable-pic \
    --enable-gpl \
    --enable-version3 \
    --enable-nonfree \
    \
    # Hardware Acceleration
    --enable-vaapi \
    --enable-libmfx \
    --enable-vdpau \
    --enable-cuda \
    --enable-cuvid \
    --enable-nvdec \
    --enable-nvenc \
    --enable-libdrm \
    \
    # Video Codecs
    --enable-libx264 \
    --enable-libx265 \
    --enable-libvpx \
    --enable-libdav1d \
    --enable-libsvtav1 \
    --enable-libxvid \
    --enable-libwebp \
    --enable-libopenjpeg \
    \
    # Audio Codecs
    --enable-libmp3lame \
    --enable-libopus \
    --enable-libvorbis \
    --enable-libfdk-aac \
    --enable-libsoxr \
    \
    # Subtitle/Text
    --enable-libass \
    --enable-libfreetype \
    --enable-libfontconfig \
    --enable-libfribidi \
    \
    # Other Features
    --enable-libbluray \
    --enable-libzvbi \
    --enable-libzimg \
    --enable-opengl \
    --enable-opencl \
    --enable-vulkan \
    --enable-libvmaf \
    \
    # Build Options
    --enable-pthreads \
    --enable-hardcoded-tables \
    --disable-debug \
    --disable-doc \
    --disable-stripping \
    --extra-cflags="-O3 -march=native" \
    --extra-ldflags="-Wl,-rpath,/usr/local/ffmpeg-cuems/lib"
```

### 4. Build FFmpeg

```bash
chmod +x configure_ffmpeg.sh
./configure_ffmpeg.sh

# Build (adjust -j to number of CPU cores)
make -j$(nproc)

# Install
sudo make install

# Update library cache
sudo ldconfig
```

### 5. Verify Build

```bash
# Check installed version
/usr/local/ffmpeg-cuems/bin/ffmpeg -version

# Check hardware decoders
/usr/local/ffmpeg-cuems/bin/ffmpeg -hide_banner -decoders 2>&1 | grep -E "h264.*vaapi|h264.*qsv|h264.*cuvid"

# Check hardware acceleration methods
/usr/local/ffmpeg-cuems/bin/ffmpeg -hide_banner -hwaccels 2>&1
```

Expected output should include:
- `h264_vaapi` decoder
- `h264_qsv` decoder
- `h264_cuvid` decoder (if NVIDIA GPU)
- `vaapi`, `qsv`, `cuda` in hwaccels list

## Using Custom FFmpeg with cuems-videocomposer

### Option 1: System-wide Installation (Recommended)

```bash
# Backup existing FFmpeg
sudo mv /usr/bin/ffmpeg /usr/bin/ffmpeg.old
sudo mv /usr/lib/x86_64-linux-gnu/libav* /usr/lib/x86_64-linux-gnu/backup/

# Link custom FFmpeg
sudo ln -s /usr/local/ffmpeg-cuems/bin/ffmpeg /usr/bin/ffmpeg
sudo ln -s /usr/local/ffmpeg-cuems/lib/libav*.so* /usr/lib/x86_64-linux-gnu/

# Update pkg-config
export PKG_CONFIG_PATH=/usr/local/ffmpeg-cuems/lib/pkgconfig:$PKG_CONFIG_PATH
```

### Option 2: Use Custom FFmpeg via Environment Variables

```bash
# Set PKG_CONFIG_PATH to use custom FFmpeg
export PKG_CONFIG_PATH=/usr/local/ffmpeg-cuems/lib/pkgconfig:$PKG_CONFIG_PATH

# Rebuild cuems-videocomposer
cd /home/ion/src/cuems/cuems-videocomposer
rm -rf build
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Option 3: Local Installation (No Root Access)

```bash
# Install to user directory
./configure --prefix=$HOME/ffmpeg-cuems ...

# Set environment variables
export PATH=$HOME/ffmpeg-cuems/bin:$PATH
export LD_LIBRARY_PATH=$HOME/ffmpeg-cuems/lib:$LD_LIBRARY_PATH
export PKG_CONFIG_PATH=$HOME/ffmpeg-cuems/lib/pkgconfig:$PKG_CONFIG_PATH

# Rebuild cuems-videocomposer
cd /home/ion/src/cuems/cuems-videocomposer
rm -rf build
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Minimal Configuration (Intel GPU Focus)

For Intel GPUs specifically, here's a minimal configuration:

```bash
./configure \
    --prefix=/usr/local/ffmpeg-cuems \
    --enable-shared \
    --enable-pic \
    --enable-gpl \
    --enable-version3 \
    \
    # Intel-specific hardware acceleration
    --enable-vaapi \
    --enable-libmfx \
    --enable-libdrm \
    \
    # Essential codecs
    --enable-libx264 \
    --enable-libx265 \
    --enable-libvpx \
    --enable-libdav1d \
    \
    # Essential audio
    --enable-libmp3lame \
    --enable-libopus \
    \
    # Essential features
    --enable-libfreetype \
    --enable-libfontconfig \
    --enable-libass \
    --enable-opengl \
    \
    # Build options
    --enable-pthreads \
    --disable-debug \
    --disable-doc \
    --extra-cflags="-O3 -march=native" \
    --extra-ldflags="-Wl,-rpath,/usr/local/ffmpeg-cuems/lib"
```

## Troubleshooting

### Issue: `h264_vaapi` decoder still not found

**Solution**: Ensure VAAPI development libraries are installed:
```bash
sudo apt-get install libva-dev libva-drm2-dev libva-x11-2-dev
```

### Issue: `h264_qsv` decoder not found

**Solution**: Install Intel Media SDK (libmfx):
```bash
sudo apt-get install libmfx-dev
```

### Issue: pkg-config can't find custom FFmpeg

**Solution**: Update PKG_CONFIG_PATH:
```bash
export PKG_CONFIG_PATH=/usr/local/ffmpeg-cuems/lib/pkgconfig:$PKG_CONFIG_PATH
pkg-config --modversion libavcodec
```

### Issue: Runtime library errors

**Solution**: Update LD_LIBRARY_PATH or run ldconfig:
```bash
export LD_LIBRARY_PATH=/usr/local/ffmpeg-cuems/lib:$LD_LIBRARY_PATH
# Or
sudo ldconfig
```

## Verification Checklist

After building, verify:

- [ ] `h264_vaapi` decoder is available
- [ ] `h264_qsv` decoder is available  
- [ ] `h264_cuvid` decoder is available (if NVIDIA GPU)
- [ ] VAAPI hardware acceleration works
- [ ] QSV hardware acceleration works
- [ ] cuems-videocomposer detects hardware decoders
- [ ] Hardware decoding actually works in playback

## Performance Comparison

After building custom FFmpeg, you should see:
- Lower CPU usage during video playback
- Faster frame decoding
- Better performance with multiple video layers
- Hardware-accelerated decoding working correctly

## Notes

- Building FFmpeg can take 30-60 minutes depending on CPU
- The custom FFmpeg will be ~50-100MB larger than minimal build
- Keep system FFmpeg as backup if needed
- Test thoroughly before deploying to production

