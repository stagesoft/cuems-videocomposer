# FFmpeg HAP Q Alpha Patch - Implementation Notes

This document provides detailed implementation notes for adding HAP Q Alpha support to FFmpeg.

## Source Code Analysis (FFmpeg 5.1.7)

### Current Structure

**File**: `libavcodec/hapenc.c` (365 lines)

**Key Functions**:
- `hap_encode_frame()` - Main encoding function (line ~200)
- `compress_texture()` - Compresses single texture (line ~80)
- `hap_compress_frame()` - Applies Snappy compression (line ~120)
- `hap_init()` - Initializes encoder context (line ~280)

**Format Options** (line 331-334):
```c
{ "format", NULL, OFFSET(opt_tex_fmt), AV_OPT_TYPE_INT, 
  { .i64 = HAP_FMT_RGBDXT1 }, HAP_FMT_RGBDXT1, HAP_FMT_YCOCGDXT5, FLAGS, "format" },
    { "hap",       "Hap 1 (DXT1 textures)", 0, AV_OPT_TYPE_CONST, 
      { .i64 = HAP_FMT_RGBDXT1   }, 0, 0, FLAGS, "format" },
    { "hap_alpha", "Hap Alpha (DXT5 textures)", 0, AV_OPT_TYPE_CONST, 
      { .i64 = HAP_FMT_RGBADXT5  }, 0, 0, FLAGS, "format" },
    { "hap_q",     "Hap Q (DXT5-YCoCg textures)", 0, AV_OPT_TYPE_CONST, 
      { .i64 = HAP_FMT_YCOCGDXT5 }, 0, 0, FLAGS, "format" },
```

## Required Changes

### 1. Add Format Constant

**File**: `libavcodec/hap.h`

```c
enum HapTextureFormat {
    HAP_FMT_RGBDXT1   = 0x0B,
    HAP_FMT_RGBADXT5  = 0x0E,
    HAP_FMT_YCOCGDXT5 = 0x0F,
    HAP_FMT_RGTC1     = 0x01,
    HAP_FMT_YCOCGDXT5_RGTC1 = 0x10,  // NEW
};
```

### 2. Update Format Range

**File**: `libavcodec/hapenc.c` (line ~331)

Change:
```c
{ .i64 = HAP_FMT_RGBDXT1 }, HAP_FMT_RGBDXT1, HAP_FMT_YCOCGDXT5, FLAGS, "format" },
```

To:
```c
{ .i64 = HAP_FMT_RGBDXT1 }, HAP_FMT_RGBDXT1, HAP_FMT_YCOCGDXT5_RGTC1, FLAGS, "format" },
```

### 3. Add Format Option

**File**: `libavcodec/hapenc.c` (after line 334)

```c
{ "hap_q_alpha", "Hap Q Alpha (DXT5-YCoCg + RGTC1 textures)", 0, AV_OPT_TYPE_CONST,
  { .i64 = HAP_FMT_YCOCGDXT5_RGTC1 }, 0, 0, FLAGS, "format" },
```

### 4. Modify Encoding Function

**File**: `libavcodec/hapenc.c` (in `hap_encode_frame()`)

Add dual-texture detection:
```c
static int hap_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                            const AVFrame *frame, int *got_packet)
{
    HapContext *ctx = avctx->priv_data;
    
    // NEW: Handle HAP Q Alpha (dual texture)
    if (ctx->opt_tex_fmt == HAP_FMT_YCOCGDXT5_RGTC1) {
        return hap_encode_dual_texture(avctx, pkt, frame, got_packet);
    }
    
    // Existing single-texture code...
}
```

### 5. Implement Dual-Texture Encoding

**File**: `libavcodec/hapenc.c` (new function)

```c
static int hap_encode_dual_texture(AVCodecContext *avctx, AVPacket *pkt,
                                   const AVFrame *frame, int *got_packet)
{
    HapContext *ctx = avctx->priv_data;
    int ret;
    
    // Set texture count
    ctx->texture_count = 2;
    
    // Allocate space for both textures
    // Texture 0: YCoCg DXT5 (same as HAP Q)
    // Texture 1: Alpha RGTC1
    
    // TODO: Implement dual-texture encoding
    // 1. Convert RGBA → YCoCg (for texture 0)
    // 2. Extract alpha channel (for texture 1)
    // 3. Compress texture 0 as DXT5 YCoCg
    // 4. Compress texture 1 as RGTC1
    // 5. Write HAP frame with two texture sections
    
    return AVERROR(ENOSYS);  // Not implemented yet
}
```

## Implementation Challenges

### Challenge 1: RGTC1/BC4 Compression

FFmpeg's `texturedsp` module may not support RGTC1/BC4 compression. Options:

1. **Add RGTC1 support to texturedsp**
2. **Use external library** (e.g., squish, nvtt)
3. **Implement simple RGTC1 encoder** (basic BC4 compression)

### Challenge 2: YCoCg Conversion

Need to convert RGBA → YCoCg color space:
- Extract Y (luma) from RGB
- Calculate Co and Cg chrominance
- Apply scale factor encoding
- Pack into DXT5 format

Reference: Vidvox HAP SDK or HAP specification.

### Challenge 3: Frame Structure

HAP Q Alpha frame structure:
```
[Frame Header]
  [Section: Decode Instructions]
  [Section: Compressor Table]
  [Section: Size Table]
  [Section: Offset Table]
  [Section: Texture 0 Data (YCoCg DXT5)]
  [Section: Texture 1 Data (Alpha RGTC1)]
```

Each section has a header (4 or 8 bytes) + data.

## Testing

After implementing:

```bash
# Test encoding
ffmpeg -i input_with_alpha.mov -c:v hap -format hap_q_alpha output.mov

# Verify format
ffprobe output.mov

# Test decoding (should work with existing FFmpeg)
ffmpeg -i output.mov -frames:v 1 test_frame.png
```

## Alternative: Use Vidvox HAP SDK Directly

Instead of patching FFmpeg, create a standalone encoder using the HAP SDK:

```cpp
// tools/hap_q_alpha_encoder.cpp
#include "external/hap/source/hap.h"
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

// Use FFmpeg to decode input video
// Use HAP SDK to encode frames as HAP Q Alpha
// Use FFmpeg to mux into MOV container
```

This approach:
- ✅ Uses existing, proven HAP SDK
- ✅ Easier to implement
- ✅ No FFmpeg patching needed
- ❌ Requires separate tool

## References

- FFmpeg HAP Encoder: `/tmp/ffmpeg-5.1.7/libavcodec/hapenc.c`
- Vidvox HAP SDK: `external/hap/source/hap.c`
- HAP Specification: https://github.com/Vidvox/hap/blob/master/documentation/HapVideoDRAFT.md

