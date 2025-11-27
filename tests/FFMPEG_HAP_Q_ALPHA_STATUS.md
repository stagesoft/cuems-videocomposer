# FFmpeg HAP Q Alpha Support Status

**Last Checked**: November 2025  
**FFmpeg Version**: Latest git (master branch)

## Summary

✅ **DECODING**: FFmpeg **CAN decode** HAP Q Alpha (HAQA) files  
❌ **ENCODING**: FFmpeg **CANNOT encode** HAP Q Alpha files

## Decoding Support

FFmpeg has **full decoding support** for HAP Q Alpha:

### 1. HAP Decoder (`libavcodec/hapdec.c`)

The decoder supports dual-texture HAP Q Alpha:
- **Texture 0**: YCoCg DXT5 (color) - decoded using `dxt5ys_block`
- **Texture 1**: Alpha RGTC1 - decoded using `rgtc1u_alpha_block`
- Output: RGBA format

**Code evidence**:
```c
// From hapdec.c
ctx->dec[0].tex_funct = dxtc.dxt5ys_block;      // YCoCg texture
ctx->dec[1].tex_funct = dxtc.rgtc1u_alpha_block; // Alpha texture
ctx->texture_count = 2;  // Dual texture mode
```

### 2. HAPQA Extract Bitstream Filter (`libavcodec/bsf/hapqa_extract.c`)

Added in 2017 by Jokyo Images, this filter can extract individual textures from HAQA files:

```bash
# Extract RGB texture (YCoCg)
ffmpeg -i hap_q_alpha.mov -bsf:v hapqa_extract=texture=0 output_rgb.mov

# Extract Alpha texture
ffmpeg -i hap_q_alpha.mov -bsf:v hapqa_extract=texture=1 output_alpha.mov
```

**Purpose**: Allows extracting color or alpha channel separately from HAQA files.

## Encoding Support

❌ **NO encoding support** for HAP Q Alpha in FFmpeg.

### Current Encoder Status

**File**: `libavcodec/hapenc.c`

**Supported formats**:
- `hap` (HAP_FMT_RGBDXT1 = 0x0B) - Standard HAP
- `hap_alpha` (HAP_FMT_RGBADXT5 = 0x0E) - HAP Alpha
- `hap_q` (HAP_FMT_YCOCGDXT5 = 0x0F) - HAP Q

**Missing**:
- `hap_q_alpha` - **NOT IMPLEMENTED**

### Format Definitions

From `libavcodec/hap.h`:
```c
enum HapTextureFormat {
    HAP_FMT_RGBDXT1   = 0x0B,  // Standard HAP
    HAP_FMT_RGBADXT5  = 0x0E,  // HAP Alpha
    HAP_FMT_YCOCGDXT5 = 0x0F,  // HAP Q
    HAP_FMT_RGTC1     = 0x01,  // Alpha texture (used in HAQA)
    // NO HAP_FMT_YCOCGDXT5_RGTC1 constant exists
};
```

### Infrastructure Present

The encoder infrastructure **partially supports** dual textures:
- `HapContext` has `texture_count` field (can be set to 2)
- Comment in code: `/* 2 for HAQA, 1 for other version */`
- But encoding logic for dual textures is **not implemented**

## Why Encoding Isn't Supported

1. **Complexity**: Requires encoding two separate textures:
   - YCoCg DXT5 compression (similar to HAP Q)
   - RGTC1/BC4 compression for alpha (not in FFmpeg's texturedsp)

2. **Frame Structure**: HAQA frames have a specific structure:
   ```
   [Frame Header]
     [Section: Multi-texture header (0x0D)]
     [Section: Texture 0 (YCoCg DXT5)]
     [Section: Texture 1 (Alpha RGTC1)]
   ```

3. **RGTC1 Compression**: FFmpeg's `texturedsp` module doesn't include RGTC1/BC4 compression, which is needed for the alpha texture.

## Testing Decoding

You can test FFmpeg's HAQA decoding:

```bash
# Decode HAQA file (if you have one)
ffmpeg -i test_hap_q_alpha.mov -frames:v 1 test_frame.png

# Extract RGB texture only
ffmpeg -i test_hap_q_alpha.mov -bsf:v hapqa_extract=texture=0 output_rgb.mov

# Extract Alpha texture only
ffmpeg -i test_hap_q_alpha.mov -bsf:v hapqa_extract=texture=1 output_alpha.mov
```

## Future Prospects

**No active development** for HAQA encoding in FFmpeg:
- Last HAP encoder commit: Very few since 2020
- No open issues or patches for HAQA encoding
- Community relies on alternative tools (Vidvox codec, AfterCodecs, etc.)

## Recommendations

For creating HAP Q Alpha files:

1. **Use Vidvox HAP QuickTime Codec** (Free)
   - Works with Adobe Media Encoder, Premiere, After Effects
   - Official Vidvox implementation

2. **Use AfterCodecs** (Commercial)
   - Professional plugin for Adobe CC
   - GPU acceleration support

3. **Use HAP Exporter for Adobe CC** (Free, Open Source)
   - Community plugin
   - GitHub: https://github.com/disguise-one/hap-encoder-adobe-cc

4. **Build Custom FFmpeg** (Advanced)
   - See `BUILD_FFMPEG_HAP_Q_ALPHA.md` for implementation guide
   - Requires implementing RGTC1 compression and dual-texture encoding

## Conclusion

FFmpeg is excellent for **decoding** HAP Q Alpha files, but you'll need alternative tools for **encoding**. The decoding support is mature and well-tested, but encoding support would require significant development effort that hasn't been prioritized by the FFmpeg project.

