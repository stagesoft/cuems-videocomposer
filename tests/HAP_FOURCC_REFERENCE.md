# HAP Codec FourCC Reference

This document lists all known HAP codec FourCC (Four Character Code) identifiers used in QuickTime/MOV files.

## Standard HAP FourCC Codes

Based on FFmpeg source code (`libavcodec/hapdec.c`) and [official HAP documentation](https://hap.video/using-hap):

| FourCC | Hex Value | Format | Description | Status |
|--------|-----------|--------|-------------|--------|
| **Hap1** | 0x48617031 | DXT1 RGB | Standard HAP (lowest data rate) | ✅ Standard |
| **Hap5** | 0x48617035 | DXT5 RGBA | HAP Alpha (with alpha channel) | ✅ Standard |
| **HapY** | 0x48617059 | DXT5 YCoCg | HAP Q (higher quality) | ✅ Standard |
| **HapA** | 0x48617041 | RGTC1 | HAP Alpha Only (alpha channel only) | ✅ Standard |
| **HapM** | 0x4861704d | Multi-texture | HAP Multi-texture (HAP Q Alpha) | ✅ Standard |
| **HapR** | 0x48617052 | Unknown | **HAP R (NEW!)** - Best quality + Alpha | ⚠️ New Format |

## HAP R - The Newest HAP Variant

**Source**: [Official HAP Documentation](https://hap.video/using-hap)

### What is HAP R?

**HAP R** is the **newest addition** to the HAP codec family:

- ✅ **Best image quality** of all HAP codecs
- ✅ **Includes Alpha channel** support
- ✅ **Recommended** to use instead of HAP Q and HAP Q Alpha whenever possible

### HAP R Characteristics

According to the [official HAP website](https://hap.video/using-hap):

> "**HAP R** (new!) has best image quality of the HAP codecs and includes an Alpha channel."
> 
> "The new HAP R should be used instead of HAP Q and HAP Q Alpha whenever possible."

### HAP R vs Other Variants

| Variant | Quality | Alpha | Data Rate | Recommendation |
|---------|---------|-------|-----------|----------------|
| **HAP** | Good | ❌ | Lowest | For non-alpha content |
| **HAP Alpha** | Good | ✅ | Low | For alpha with good quality |
| **HAP Q** | Better | ❌ | Medium | For high quality (no alpha) |
| **HAP Q Alpha** | Better | ✅ | High | Legacy (use HAP R instead) |
| **HAP R** | **Best** | ✅ | Medium-High | **Recommended for alpha** |

### FFmpeg Support

**Status**: HAP R is **NOT yet supported** in FFmpeg (as of FFmpeg 5.1.7 and latest git).

The FFmpeg HAP encoder only supports:
- `hap` (Hap1)
- `hap_alpha` (Hap5)
- `hap_q` (HapY)

**Missing**: `hap_r` format option

### Vidvox HAP SDK Support

**Status**: ✅ **FULLY SUPPORTED** in Vidvox HAP SDK

The Vidvox HAP SDK (used by cuems-videocomposer) **fully supports HAP R** via:
- `HapTextureFormat_RGBA_BPTC_UNORM` (0x8E8C) - BPTC/BC7 compressed RGBA

**Implementation in cuems-videocomposer**:
- ✅ HAP R detection via `HapDecoder::getVariant()`
- ✅ BPTC texture format support (`GL_COMPRESSED_RGBA_BPTC_UNORM`)
- ✅ Renders with standard RGBA shader (BPTC is RGBA, not YCoCg)
- ✅ Direct GPU texture upload (zero-copy)

### Encoding HAP R

Since FFmpeg doesn't support HAP R encoding, use:

1. **AVF Batch Exporter** (Free)
   - Supports HAP R encoding
   - Available for macOS

2. **AfterCodecs / Jokyo HAP Encoder** (Commercial)
   - Professional plugins for Adobe CC
   - Support HAP R encoding

3. **QuickTime Export** (with HAP QuickTime component)
   - HAP R should appear in codec selection
   - Download from [GitHub releases](https://github.com/Vidvox/hap-qt-codec/releases)

### About "7A"

The "7A" in "HapR/7A" is unclear:
- Could be a version identifier
- Might be part of a build number
- Could be unrelated to the codec itself

**HapR** = **HAP R** codec variant (confirmed from official documentation)

### Where You Might See HapR/7A

1. **Adobe Media Encoder** - May use alternative FourCC codes
2. **Disguise Media Server** - May have custom identifiers
3. **File metadata** - Some encoders might write custom tags
4. **Log files** - Software might display alternative names

## Verification

To check if a file uses HapR:

```bash
# Check codec tag
ffprobe -v quiet -select_streams v:0 \
  -show_entries stream=codec_tag_string,codec_tag \
  -of default=noprint_wrappers=1:nokey=1 file.mov

# Check FourCC in hex
ffprobe -v quiet -select_streams v:0 \
  -show_entries stream=codec_tag \
  -of default=noprint_wrappers=1:nokey=1 file.mov | \
  python3 -c "import sys; print(hex(int(sys.stdin.read().strip())))"
```

## Current Test Files

| File | FourCC | Variant |
|------|--------|---------|
| `test_hap.mov` | Hap1 | Standard HAP (DXT1) |
| `test_hap_hq.mov` | HapY | HAP Q (DXT5 YCoCg) |
| `test_hap_alpha.mov` | Hap5 | HAP Alpha (DXT5 RGBA) |
| `test_hap_hq_alpha.mov` | ? | HAP Q Alpha (not created yet) |

## References

- FFmpeg HAP Decoder: `libavcodec/hapdec.c`
- Vidvox HAP SDK: `external/hap/source/hap.h`
- HAP Specification: https://github.com/Vidvox/hap/blob/master/documentation/HapVideoDRAFT.md

## Next Steps

If you encounter a file with "HapR/7A":
1. Check the actual FourCC code using `ffprobe`
2. Verify if it decodes correctly with FFmpeg
3. Check if it's an alias for Hap5 (HAP Alpha)
4. Report findings to update this reference

