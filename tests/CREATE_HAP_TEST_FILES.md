# Creating HAP Test Files

This document explains how to create HAP test video files for testing the HAP direct texture upload implementation.

## Available HAP Variants

| Variant | Format | FFmpeg Support | Test File | Notes |
|---------|--------|----------------|-----------|-------|
| **HAP** | DXT1 RGB | ✅ Yes | `test_hap.mov` | Lowest data rate |
| **HAP Alpha** | DXT5 RGBA | ✅ Yes | `test_hap_alpha.mov` | Good quality + alpha |
| **HAP Q** | DXT5 YCoCg | ✅ Yes | `test_hap_hq.mov` | Higher quality |
| **HAP Q Alpha** | Dual DXT5 (YCoCg + Alpha) | ❌ No | - | Legacy format |
| **HAP R** | Unknown (Best quality) | ❌ No | - | **NEW! Recommended** |

## Creating HAP Test Files with FFmpeg

### Prerequisites

- FFmpeg with HAP encoder support (usually included in standard FFmpeg builds)
- Source video with alpha channel (for alpha variants)

### Standard HAP (DXT1 RGB)

```bash
ffmpeg -i source.mov -c:v hap -format hap -pix_fmt rgb24 video_test_files/test_hap.mov
```

### HAP Q (DXT5 YCoCg)

```bash
ffmpeg -i source.mov -c:v hap -format hap_q -pix_fmt rgb24 video_test_files/test_hap_hq.mov
```

### HAP Alpha (DXT5 RGBA)

```bash
ffmpeg -i source_with_alpha.mov -c:v hap -format hap_alpha -pix_fmt rgba video_test_files/test_hap_alpha.mov
```

### HAP R (NEW! Best Quality + Alpha) - ❌ Not Supported by FFmpeg

**HAP R** is the newest HAP variant with the **best image quality** and alpha channel support.

According to the [official HAP documentation](https://hap.video/using-hap):
> "**HAP R** (new!) has best image quality of the HAP codecs and includes an Alpha channel."
> 
> "The new HAP R should be used instead of HAP Q and HAP Q Alpha whenever possible."

**FFmpeg Support**: ❌ **NOT supported** (as of FFmpeg 5.1.7 and latest git)

**Encoding Options**:
1. **AVF Batch Exporter** (Free, macOS) - Supports HAP R
2. **AfterCodecs / Jokyo HAP Encoder** (Commercial) - Adobe CC plugins
3. **QuickTime Export** (with HAP QuickTime component)

**Command structure** (when using tools that support it):
```bash
# Format would be: -format hap_r (if FFmpeg supported it)
# Currently must use alternative tools
```

### HAP Q Alpha (Dual Texture) - ⚠️ Limited FFmpeg Support

**Note**: FFmpeg support for HAP Q Alpha encoding varies by build and version.

**Command structure** (if supported):
```bash
ffmpeg -i source_with_alpha.mov -c:v hap -format hap_q_alpha -pix_fmt rgba video_test_files/test_hap_hq_alpha.mov
```

**With optimization options**:
```bash
ffmpeg -i source_with_alpha.mov -c:v hap -format hap_q_alpha -chunks 4 -compressor snappy -pix_fmt rgba video_test_files/test_hap_hq_alpha.mov
```

**Current Status** (FFmpeg 5.1.7):
- ❌ **Not supported** in Debian 12 FFmpeg build
- Format range: 11-15 (hap_q_alpha would need format 16+)
- Available formats: `hap` (11), `hap_alpha` (14), `hap_q` (15)

**If your FFmpeg supports it**:
- Check with: `ffmpeg -h encoder=hap | grep hap_q_alpha`
- If available, use the command above
- If not, use alternative tools listed below

**HAP Q Alpha requires encoding two separate textures**:
- YCoCg DXT5 texture (color)
- Alpha RGTC1 texture (alpha channel)

Some FFmpeg builds may not include this dual-texture encoding support.

## Alternative Methods for HAP Q Alpha

Since FFmpeg doesn't support HAP Q Alpha encoding, you need to use alternative tools:

### Option 1: HAP Exporter for Adobe CC (Recommended - Free/Open Source)

**GitHub**: [disguise-one/hap-encoder-adobe-cc](https://github.com/disguise-one/hap-encoder-adobe-cc)

A community-supported plugin for Adobe Creative Cloud applications that supports HAP Q Alpha encoding.

**Installation:**
1. Download from GitHub releases
2. Install the plugin for Adobe Media Encoder / Premiere Pro / After Effects
3. Export as "HAP Video" format → Select "HAP Q Alpha"

**Usage:**
- Works with Adobe Media Encoder (GUI)
- May support command-line batch processing via Media Encoder's command-line interface

### Option 2: AfterCodecs (Commercial Plugin)

**Website**: [autokroma.com/AfterCodecs](https://www.autokroma.com/AfterCodecs/Codecs_containers)

Commercial plugin for Adobe CC that supports:
- HAP, HAP Alpha, HAP Q, **HAP Q Alpha**
- GPU acceleration
- Customizable encoding options

**Features:**
- Native integration with After Effects, Premiere Pro, Media Encoder
- Advanced encoding options (chunks, compression)
- Professional support

### Option 3: HAP QuickTime Codec + Adobe Media Encoder

1. Install HAP QuickTime codec from [Vidvox GitHub](https://github.com/Vidvox/hap-qt-codec/releases/)
2. Open Adobe Media Encoder
3. Import source video with alpha
4. Export as QuickTime → Codec: **HAP Q Alpha**

**Note**: Requires macOS or Windows (QuickTime codec support)

### Option 4: HAPpy (Windows GUI Tool)

**GitHub**: [Tedcharlesbrown/HAPpy](https://github.com/Tedcharlesbrown/HAPpy)

User-friendly GUI tool that wraps FFmpeg. However, **HAP Q Alpha support is unclear** - it may still be limited by FFmpeg's capabilities.

**Status**: Verify if latest version supports HAP Q Alpha

### Option 5: VDMX Batch Exporter (macOS)

For macOS users working with Apple Motion or similar:
1. Export as ProRes 4444 (with alpha)
2. Use VDMX Batch Exporter utility to transcode to HAP Q Alpha

**Reference**: [VDMX Tutorial](https://vdmx.vidvox.net/tutorials/working-with-hap-alpha-encoded-movies-in-vdmx)

### Option 6: Command-Line with Adobe Media Encoder

Adobe Media Encoder has a command-line interface that can be scripted:

```bash
# Example (requires Adobe Media Encoder installed)
"/Applications/Adobe Media Encoder CC 2024/Adobe Media Encoder" \
  -render \
  -project "path/to/project.aep" \
  -output "output_hap_q_alpha.mov" \
  -preset "HAP Q Alpha"
```

**Note**: Exact command syntax depends on Adobe Media Encoder version and platform.

## Current Test Files

| File | Size | Status | Created From |
|------|------|--------|--------------|
| `test_hap.mov` | 84 MB | ✅ Ready | Standard HAP encoding |
| `test_hap_hq.mov` | 159 MB | ✅ Ready | HAP Q encoding |
| `test_hap_alpha.mov` | 153 MB | ✅ Ready | Created from `test_with_alpha.mov` |
| `test_hap_hq_alpha.mov` | - | ❌ Missing | Cannot create with FFmpeg (legacy) |
| `test_hap_r.mov` | - | ❌ Missing | **NEW!** Cannot create with FFmpeg |

## Testing HAP Files

Use the test script to verify HAP files:

```bash
# Test all HAP variants
python3 tests/test_codec_formats.py --test-hap

# Test specific HAP file
python3 tests/test_codec_formats.py --video video_test_files/test_hap_alpha.mov
```

## Notes

- **Resolution**: HAP requires width and height to be divisible by 4 for optimal performance
- **Alpha Channel**: Source videos for alpha variants must have a proper alpha channel
- **Quality**: HAP Q provides better quality than standard HAP but uses more bandwidth
- **Performance**: All HAP variants support direct GPU texture upload (zero-copy)

## FFmpeg HAP Encoder Options

Available formats (check with `ffmpeg -h encoder=hap`):
- `hap` (11) - Standard HAP (DXT1)
- `hap_alpha` (14) - HAP Alpha (DXT5 RGBA)
- `hap_q` (15) - HAP Q (DXT5 YCoCg)
- `hap_q_alpha` - **May be available in newer/custom FFmpeg builds**

**To check if your FFmpeg supports HAP Q Alpha**:
```bash
ffmpeg -h encoder=hap | grep hap_q_alpha
```

If the command above shows `hap_q_alpha`, you can use:
```bash
ffmpeg -i input.mov -c:v hap -format hap_q_alpha -pix_fmt rgba output.mov
```

**Note**: Debian 12 FFmpeg 5.1.7 does NOT include `hap_q_alpha` support. You may need:
- A newer FFmpeg version (6.0+)
- A custom FFmpeg build with HAP Q Alpha patches
- Or use alternative tools listed below

## Recommended Workflow for HAP Q Alpha

For Linux/command-line environments:

1. **Best Option**: Use **HAP Exporter for Adobe CC** plugin with Adobe Media Encoder
   - Free and open source
   - Community maintained
   - Supports HAP Q Alpha

2. **Alternative**: Use **AfterCodecs** (commercial) if you need advanced features

3. **Workaround**: If you have access to macOS/Windows:
   - Install HAP QuickTime codec
   - Use Adobe Media Encoder or other QuickTime-compatible software
   - Export as HAP Q Alpha

## Quick Reference: Tool Comparison

| Tool | Platform | Cost | HAP Q Alpha | Command-Line | Notes |
|------|----------|------|-------------|--------------|-------|
| **FFmpeg** | All | Free | ❌ No | ✅ Yes | Standard HAP only |
| **HAP Exporter for Adobe CC** | Windows/macOS | Free | ✅ Yes | ⚠️ Via AME CLI | Community plugin |
| **AfterCodecs** | Windows/macOS | Commercial | ✅ Yes | ⚠️ Via AME CLI | Professional features |
| **HAP QuickTime Codec** | macOS/Windows | Free | ✅ Yes | ❌ GUI only | Requires QuickTime |
| **HAPpy** | Windows | Free | ❓ Unclear | ❌ GUI only | Wraps FFmpeg |
| **VDMX Batch Exporter** | macOS | Free | ✅ Yes | ⚠️ Limited | For VDMX users |

