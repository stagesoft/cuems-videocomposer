# HAP Direct Texture Upload - Testing Guide

## Prerequisites

### 1. Build with HAP Support

**Note:** HAP direct texture upload is **enabled by default** (no need to specify `-DENABLE_HAP_DIRECT=ON`).

```bash
cd build
cmake ..
make -j$(nproc)
```

Verify HAP support is enabled in CMake output:
```
-- HAP direct texture upload enabled (with snappy)
```

If you see "Snappy not found" instead:
```bash
sudo apt-get install libsnappy-dev
# Then rebuild
cmake ..
make -j$(nproc)
```

**To disable HAP direct texture upload:**
```bash
cmake .. -DENABLE_HAP_DIRECT=OFF
```

### 2. Verify Test Videos

Required test videos in `video_test_files/`:
- `test_hap.mov` - Standard HAP (DXT1)
- `test_hap_hq.mov` - HAP Q (DXT5 YCoCg)
- `test_hap_alpha.mov` - HAP Alpha (DXT5 RGBA)
- `test_hap_hq_alpha.mov` - HAP Q Alpha (dual texture) - **May need to create**

Check existing videos:
```bash
ls -lh video_test_files/test_hap*.mov
```

## Testing Methods

### Method 1: Automated Test Suite

Run the comprehensive HAP test suite:

```bash
./tests/test_hap_direct.py
```

This tests:
- HAP variant detection
- Direct DXT decode verification
- Playback for all variants
- Fallback mechanism
- Seeking accuracy

### Method 2: Manual Testing with Verbose Logging

Test each HAP variant individually:

```bash
# Standard HAP (DXT1)
./build/cuems-videocomposer --file video_test_files/test_hap.mov --verbose

# HAP Q (YCoCg DXT5)
./build/cuems-videocomposer --file video_test_files/test_hap_hq.mov --verbose

# HAP Alpha (DXT5 RGBA)
./build/cuems-videocomposer --file video_test_files/test_hap_alpha.mov --verbose

# HAP Q Alpha (dual texture)
./build/cuems-videocomposer --file video_test_files/test_hap_hq_alpha.mov --verbose
```

### Method 3: Using Existing Codec Tests

```bash
# Test all HAP files
python3 tests/test_codec_formats.py --test-hap

# Test specific HAP file
python3 tests/test_codec_formats.py --video video_test_files/test_hap.mov --duration 5
```

## What to Look For

### ✅ Direct DXT Decode Working

Look for these log messages:
```
[INFO] HAP direct texture upload enabled (with snappy)
[VERBOSE] Decoded HAP texture 0: format=0x83f0, size=1048576 bytes
[VERBOSE] Uploaded HAP frame (format=0x83f0)
[VERBOSE] Loaded HAP frame N to GPU (direct DXT upload)
```

**Performance indicators:**
- Low GPU memory usage (check with `nvidia-smi` or `radeontop`)
- Smooth playback even with multiple layers
- No "sws_scale" in logs (that indicates CPU conversion)

### ⚠️ FFmpeg Fallback Active

Look for these warning messages:
```
[WARNING] HAP direct decode failed for frame N, falling back to FFmpeg RGBA path (reduced performance)
[WARNING] Error: [specific error from HapDecoder]
[VERBOSE] Uploaded HAP frame via FFmpeg fallback (uncompressed)
```

**Reasons for fallback:**
- Snappy library not installed
- `ENABLE_HAP_DIRECT=OFF` in CMake
- Corrupted HAP packet
- Unsupported HAP variant

### ❌ Errors to Watch For

```
[ERROR] Failed to allocate HAP texture
[ERROR] glCompressedTexImage2D failed
[ERROR] Unknown HAP variant in packet
```

These indicate issues with OpenGL or HAP decoding that need investigation.

## Variant-Specific Testing

### HAP (Standard DXT1)

**Expected behavior:**
- Smallest memory footprint (0.5 bytes/pixel)
- Standard RGB rendering
- No alpha channel

**Log verification:**
```
format=0x83f0  # GL_COMPRESSED_RGB_S3TC_DXT1_EXT
```

### HAP Q (YCoCg DXT5)

**Expected behavior:**
- Higher quality than standard HAP
- YCoCg color space (shader converts to RGB)
- No color shift or tinting

**Log verification:**
```
format=0x1     # HapTextureFormat_YCoCg_DXT5
Using HAP Q shader (YCoCg→RGB conversion)
```

**Visual verification:**
Colors should look natural, not shifted. If colors look wrong, the YCoCg shader may have issues.

### HAP Alpha (DXT5 RGBA)

**Expected behavior:**
- Alpha channel transparency
- Standard RGBA rendering

**Log verification:**
```
format=0x83f3  # GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
```

**Visual verification:**
Transparent areas should composite correctly with background.

### HAP Q Alpha (Dual Texture)

**Expected behavior:**
- High quality color (YCoCg)
- Separate alpha channel
- Two textures uploaded

**Log verification:**
```
HAP frame has 2 texture(s)
Uploaded HAP Q Alpha frame (color + alpha)
Using HAP Q Alpha shader (dual texture)
```

**Visual verification:**
Both quality and alpha should be correct.

## Performance Testing

### Single Layer Performance

```bash
# Measure decode time
time ./build/cuems-videocomposer --file video_test_files/test_hap.mov --duration 10
```

### Multi-Layer Stress Test

```bash
# Load multiple HAP layers (requires OSC control)
# TODO: Add multi-layer test script
```

### Bandwidth Measurement

Compare GPU memory bandwidth:

**Direct DXT (1920x1080 @ 30fps):**
- HAP: ~30 MB/s
- HAP Q: ~60 MB/s

**FFmpeg Fallback:**
- All variants: ~240 MB/s (8x more!)

Monitor with:
```bash
# NVIDIA
watch -n 1 nvidia-smi

# AMD
radeontop
```

## Creating Missing Test Videos

If `test_hap_hq_alpha.mov` doesn't exist:

```bash
# Create HAP Q Alpha test video (requires source with alpha channel)
ffmpeg -i video_test_files/test_alpha_source.mov \
       -c:v hap -format hap_q_alpha \
       -pix_fmt yuva444p10le \
       video_test_files/test_hap_hq_alpha.mov
```

Or use the test video creation script:
```bash
./tests/create_test_videos.sh
```

## Troubleshooting

### Build fails with "snappy not found"

```bash
# Debian/Ubuntu
sudo apt-get install libsnappy-dev

# Fedora/RHEL
sudo dnf install snappy-devel

# Rebuild
cd build
cmake .. -DENABLE_HAP_DIRECT=ON
make clean && make -j$(nproc)
```

### HAP files play but use FFmpeg fallback

Check CMake configuration (HAP is enabled by default):
```bash
cd build
cmake ..  # No need for -DENABLE_HAP_DIRECT=ON (it's the default)
# Verify output shows: "-- HAP direct texture upload enabled (with snappy)"
```

### YCoCg colors look wrong (HAP Q)

Check shader compilation:
```bash
./build/cuems-videocomposer --file video_test_files/test_hap_hq.mov --verbose 2>&1 | grep -i shader
```

Should see:
```
All video shaders compiled successfully
```

### Performance not improved

1. Verify direct DXT is being used (check logs)
2. Check GPU memory usage (should be 4-8x lower than before)
3. Compare with FFmpeg fallback explicitly disabled

## Expected Results

| Test | Expected Result |
|------|----------------|
| HAP Detection | Variant correctly identified in logs |
| Direct Decode | "direct DXT upload" in verbose logs |
| HAP Playback | Smooth playback, no errors |
| HAP Q Playback | Correct colors (no tinting) |
| HAP Alpha | Alpha transparency works |
| HAP Q Alpha | Quality + alpha both working |
| Fallback | Graceful degradation with warning |

## Next Steps

After successful testing:

1. Test with real-world multi-layer HAP setups
2. Benchmark performance improvement (before/after)
3. Test with different HAP encoders (Adobe, FFmpeg, etc.)
4. Verify memory usage reduction
5. Test seeking performance

---

**Document Version:** 1.0  
**Last Updated:** 2025-11-27

