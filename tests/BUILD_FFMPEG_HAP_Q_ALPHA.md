# Building Custom FFmpeg with HAP Q Alpha Support

This guide explains how to build a custom FFmpeg with HAP Q Alpha encoding support, since the standard FFmpeg build doesn't include this feature.

## Current Status

**FFmpeg 5.1.7 (Debian 12)** does NOT support HAP Q Alpha encoding:
- Format range: 11-15
- Available: `hap` (11), `hap_alpha` (14), `hap_q` (15)
- Missing: `hap_q_alpha` (would need format 16+)

## Investigation: Adding HAP Q Alpha Support

### 1. Understanding HAP Q Alpha Format

HAP Q Alpha requires **dual-texture encoding**:
- **Texture 0**: YCoCg DXT5 (color information)
- **Texture 1**: Alpha RGTC1 (alpha channel)

This is different from single-texture formats that FFmpeg currently supports.

### 2. FFmpeg HAP Encoder Source Location

The HAP encoder is located in:
```
libavcodec/hapenc.c  (365 lines)
libavcodec/hap.h     (Format definitions)
libavcodec/hap.c     (Decoder - for reference)
```

**Key findings from FFmpeg 5.1.7 source:**

1. **Format Constants** (in `hap.h`):
   ```c
   enum HapTextureFormat {
       HAP_FMT_RGBDXT1   = 0x0B,    // Standard HAP
       HAP_FMT_RGBADXT5  = 0x0E,    // HAP Alpha
       HAP_FMT_YCOCGDXT5 = 0x0F,    // HAP Q
       HAP_FMT_RGTC1     = 0x01,    // Alpha texture (used in HAP Q Alpha)
   };
   ```

2. **Format Options** (in `hapenc.c` line 333-334):
   ```c
   { "hap_alpha", "Hap Alpha (DXT5 textures)", 0, AV_OPT_TYPE_CONST, 
     { .i64 = HAP_FMT_RGBADXT5 }, 0, 0, FLAGS, "format" },
   { "hap_q", "Hap Q (DXT5-YCoCg textures)", 0, AV_OPT_TYPE_CONST, 
     { .i64 = HAP_FMT_YCOCGDXT5 }, 0, 0, FLAGS, "format" },
   ```

3. **Dual-Texture Support**: The `HapContext` structure already has:
   ```c
   int texture_count;  // 2 for HAQA, 1 for other versions
   TextureDSPThreadContext dec[2];  // Decoder contexts for 2 textures
   ```
   This suggests the infrastructure for dual textures exists, but encoding support is missing.

### 3. Required Modifications

Based on the FFmpeg 5.1.7 source code analysis, here are the exact changes needed:

#### A. Add Format Constant

In `libavcodec/hap.h`, add a new format constant:
```c
enum HapTextureFormat {
    HAP_FMT_RGBDXT1   = 0x0B,
    HAP_FMT_RGBADXT5  = 0x0E,
    HAP_FMT_YCOCGDXT5 = 0x0F,
    HAP_FMT_RGTC1     = 0x01,
    HAP_FMT_YCOCGDXT5_RGTC1 = 0x10,  // NEW: HAP Q Alpha (dual texture)
};
```

#### B. Add Format Option

In `libavcodec/hapenc.c` (around line 334, after `hap_q`), add:
```c
{ "hap_q_alpha", "Hap Q Alpha (DXT5-YCoCg + RGTC1 textures)", 0, AV_OPT_TYPE_CONST,
  { .i64 = HAP_FMT_YCOCGDXT5_RGTC1 }, 0, 0, FLAGS, "format" },
```

Also update the format range in the AVOption definition (line ~331):
```c
// Change from:
{ "format", NULL, OFFSET(opt_tex_fmt), AV_OPT_TYPE_INT, 
  { .i64 = HAP_FMT_RGBDXT1 }, HAP_FMT_RGBDXT1, HAP_FMT_YCOCGDXT5, FLAGS, "format" },
// To:
{ "format", NULL, OFFSET(opt_tex_fmt), AV_OPT_TYPE_INT, 
  { .i64 = HAP_FMT_RGBDXT1 }, HAP_FMT_RGBDXT1, HAP_FMT_YCOCGDXT5_RGTC1, FLAGS, "format" },
```

#### C. Implement Dual-Texture Encoding

The main encoding function is `hap_encode_frame()` (around line 200-250). You need to:

1. **Detect HAP Q Alpha format** in the encoding function:
```c
static int hap_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                            const AVFrame *frame, int *got_packet)
{
    HapContext *ctx = avctx->priv_data;
    
    // Check if this is HAP Q Alpha (dual texture)
    if (ctx->opt_tex_fmt == HAP_FMT_YCOCGDXT5_RGTC1) {
        return hap_encode_frame_dual_texture(avctx, pkt, frame, got_packet);
    }
    
    // Existing single-texture encoding...
}
```

2. **Create dual-texture encoding function**:
```c
static int hap_encode_frame_dual_texture(AVCodecContext *avctx, AVPacket *pkt,
                                         const AVFrame *frame, int *got_packet)
{
    HapContext *ctx = avctx->priv_data;
    
    // Set texture count to 2
    ctx->texture_count = 2;
    
    // Allocate buffers for both textures
    // Texture 0: YCoCg DXT5 (color)
    // Texture 1: RGTC1 (alpha)
    
    // 1. Convert RGBA to YCoCg
    // 2. Compress YCoCg as DXT5 (using existing texture compression)
    // 3. Extract and compress alpha as RGTC1
    // 4. Write both textures to HAP frame structure
    
    // Reference: Vidvox HAP SDK in external/hap/source/hap.c
    // See HapEncodeChunk() and related functions
}
```

3. **Key implementation details**:
   - Use `ff_texturedsp_compress_thread` for DXT5 compression (already exists)
   - Need RGTC1/BC4 compression (may need additional library or implementation)
   - Frame structure: Two texture sections in HAP frame
   - Section headers: Each texture needs its own section header

#### D. Reference Implementation

The Vidvox HAP SDK (already in this repo at `external/hap/source/hap.c`) contains:
- `HapEncode()` - Main encoding function
- Dual-texture handling logic
- RGTC1 compression (if available)

**Study this code** to understand the exact frame structure and compression requirements.

### 4. Building Custom FFmpeg

#### Prerequisites

```bash
# Install build dependencies
sudo apt-get update
sudo apt-get build-dep ffmpeg
sudo apt-get install \
    build-essential \
    git \
    yasm \
    nasm \
    libx264-dev \
    libx265-dev \
    libvpx-dev \
    libfdk-aac-dev \
    libmp3lame-dev \
    libopus-dev \
    libvorbis-dev \
    libtheora-dev \
    libxvidcore-dev \
    libsnappy-dev \
    pkg-config
```

#### Step 1: Clone FFmpeg Source

```bash
cd ~/src
git clone https://git.ffmpeg.org/ffmpeg.git
cd ffmpeg
git checkout release/5.1  # Or latest stable release
```

#### Step 2: Apply HAP Q Alpha Patch

Create a patch file or manually modify:
- `libavcodec/hapenc.c` - Add HAP Q Alpha encoding logic
- `libavcodec/hap.h` - Add format constant

**Reference Implementation:**
- Vidvox HAP SDK: `external/hap/source/hap.c` (already in this repo)
- HAP specification: https://github.com/Vidvox/hap

#### Step 3: Configure Build

```bash
./configure \
    --prefix=/usr/local \
    --enable-gpl \
    --enable-version3 \
    --enable-nonfree \
    --enable-libx264 \
    --enable-libx265 \
    --enable-libvpx \
    --enable-libfdk-aac \
    --enable-libmp3lame \
    --enable-libopus \
    --enable-libvorbis \
    --enable-libtheora \
    --enable-libxvid \
    --enable-libsnappy \
    --enable-shared \
    --enable-pic \
    --extra-cflags="-I/usr/local/include" \
    --extra-ldflags="-L/usr/local/lib"
```

#### Step 4: Build and Install

```bash
make -j$(nproc)
sudo make install
sudo ldconfig  # Update library cache
```

#### Step 5: Verify HAP Q Alpha Support

```bash
/usr/local/bin/ffmpeg -h encoder=hap | grep hap_q_alpha
```

If it shows `hap_q_alpha`, the build was successful!

### 5. Implementation Challenges

#### Challenge 1: Dual-Texture Frame Structure

HAP Q Alpha frames have a specific structure:
```
[HAP Frame Header]
  [Section 1: YCoCg DXT5 texture]
  [Section 2: Alpha RGTC1 texture]
```

The encoder must:
- Create two separate compressed textures
- Combine them into a single HAP frame
- Handle frame headers correctly

#### Challenge 2: YCoCg Color Space Conversion

Need to convert RGBA → YCoCg:
- Extract Y (luma) from RGB
- Calculate Co and Cg chrominance
- Apply scale factor encoding
- Pack into DXT5 format

#### Challenge 3: Alpha RGTC1 Encoding

RGTC1 (BC4) is a single-channel compression:
- Compress alpha channel separately
- Use BC4/BC5 compression (may need additional library)
- Integrate with HAP frame structure

### 6. Alternative: Use Existing HAP SDK

Since we already have Vidvox HAP SDK in this repo (`external/hap/`), we could:

1. **Create a wrapper tool** that uses the HAP SDK directly
2. **Bypass FFmpeg** for HAP Q Alpha encoding
3. **Use FFmpeg** only for demuxing/remuxing

**Example approach:**
```cpp
// Use FFmpeg to decode source video to RGBA frames
// Use Vidvox HAP SDK to encode frames as HAP Q Alpha
// Use FFmpeg to mux into MOV container
```

### 7. Recommended Approach

**Option A: Patch FFmpeg** (Complex, but integrated)
- Modify FFmpeg source code
- Add HAP Q Alpha encoding
- Build custom FFmpeg
- **Pros**: Single tool, standard workflow
- **Cons**: Requires deep FFmpeg knowledge, maintenance burden

**Option B: Standalone HAP Q Alpha Encoder** (Easier, modular)
- Create tool using Vidvox HAP SDK
- Use FFmpeg for input/output only
- **Pros**: Easier to implement, uses existing SDK
- **Cons**: Additional tool to maintain

**Option C: Use Existing Tools** (Pragmatic)
- Use HAP Exporter for Adobe CC
- Use AfterCodecs
- **Pros**: No development needed, proven tools
- **Cons**: Requires Adobe software

### 8. Quick Start: Standalone Encoder

If we want to create a standalone HAP Q Alpha encoder using the existing HAP SDK:

```bash
# Create encoder tool
mkdir -p tools/hap_encoder
cd tools/hap_encoder

# Use existing HAP SDK from external/hap/
# Create simple encoder that:
# 1. Reads RGBA frames (from FFmpeg decode)
# 2. Encodes as HAP Q Alpha using Vidvox SDK
# 3. Writes HAP frames to MOV container
```

This would be simpler than patching FFmpeg and leverages the SDK we already have.

## Next Steps

1. **Investigate FFmpeg source**: Check if HAP Q Alpha support exists but is disabled
2. **Review Vidvox HAP SDK**: Understand encoding API for dual textures
3. **Decide on approach**: Patch FFmpeg vs. standalone tool vs. use existing tools
4. **Implement solution**: Based on chosen approach

## References

- FFmpeg Source: https://git.ffmpeg.org/ffmpeg.git
- Vidvox HAP SDK: https://github.com/Vidvox/hap
- HAP Specification: https://github.com/Vidvox/hap/blob/master/documentation/HapVideoDRAFT.md
- FFmpeg HAP Encoder: `libavcodec/hapenc.c`

