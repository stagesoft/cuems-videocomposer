# Codec and Format Testing

This directory contains scripts to test different video codecs and formats with cuems-videocomposer.

## Overview

The test suite verifies:
1. **Codec Detection**: That videocomposer correctly identifies different codecs (HAP, H.264, HEVC, AV1, VP9, MPEG-4)
2. **Hardware Decoding**: That hardware decoders (VAAPI, CUDA, VideoToolbox, DXVA2) are detected and used when available
3. **Software Decoding**: That software decoding works as fallback
4. **Format Support**: That different container formats (MP4, MOV, AVI, MKV, WebM) are supported

## Creating Test Videos

First, create test video files with different codecs:

```bash
# Use existing test file (problematic.mp4) as source
./tests/create_test_videos.sh "" video_test_files 30

# Or use a specific source directory
./tests/create_test_videos.sh /path/to/source/videos video_test_files 30
```

This will create test files in `video_test_files/`:
- `test_h264_*.mp4/mov/avi` - H.264 codec (hardware decodable)
- `test_hevc_*.mp4/mkv` - HEVC codec (hardware decodable)
- `test_av1_*.mp4` - AV1 codec (hardware decodable on newer hardware)
- `test_vp9_*.webm` - VP9 codec (software only)
- `test_hap.mov` - HAP codec (GPU-optimized, zero-copy)
- `test_mpeg4_*.avi` - MPEG-4 codec (software only)
- `test_h264_*fps.mp4` - Different frame rates (24, 30, 50 fps)
- `test_h264_*p.mp4` - Different resolutions (720p, 480p)

## Running Tests

### Test All Codecs

```bash
# Test all available test videos
python3 tests/test_codec_formats.py --test-all

# Test only hardware-decoded codecs
python3 tests/test_codec_formats.py --test-hw

# Test only software codecs
python3 tests/test_codec_formats.py --test-sw

# Test HAP specifically
python3 tests/test_codec_formats.py --test-hap
```

### Test Specific Video

```bash
# Test a specific video file
python3 tests/test_codec_formats.py --video test_h264_mp4.mp4

# With custom duration
python3 tests/test_codec_formats.py --video test_hap.mov --duration 10
```

### Options

- `--video-dir PATH`: Directory containing test videos (default: `video_test_files/`)
- `--videocomposer PATH`: Path to videocomposer binary (auto-detected from build/)
- `--duration SECONDS`: Test duration per video (default: 5)
- `--test-all`: Test all available videos
- `--test-hw`: Test hardware-decoded codecs (H.264, HEVC, AV1)
- `--test-sw`: Test software codecs (VP9, MPEG-4)
- `--test-hap`: Test HAP codec specifically

## Expected Results

### HAP Codec
- **Expected**: `HAP_DIRECT` (zero-copy GPU decoding)
- **Verification**: Should see "HAP" in logs, no CPU→GPU transfer

### H.264, HEVC, AV1
- **Expected**: `GPU_HARDWARE` if hardware decoder available, else `CPU_SOFTWARE`
- **Verification**: Should see "hardware decoder detected" or "software decoding" in logs

### VP9, MPEG-4
- **Expected**: `CPU_SOFTWARE`
- **Verification**: Should see "software decoding" in logs

## Test Output

The test script will:
1. Analyze each video file using `ffprobe` to get codec information
2. Run videocomposer with the video file
3. Analyze videocomposer output to detect:
   - Codec detection
   - Decoding path (hardware/software/HAP)
   - Errors and warnings
4. Print a summary with pass/fail status

Example output:
```
============================================================
Testing: test_h264_mp4.mp4
============================================================
  Codec: h264 (H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10)
  Format: mov,mp4,m4a,3gp,3g2,mj2
  Resolution: 1920x1080
  FPS: 25.00
  Duration: 30.00s
  Expected decoding: GPU_HARDWARE (if available) or CPU_SOFTWARE
  Running videocomposer for 5 seconds...
    Hardware decoder detected: VAAPI (Intel/AMD)
    Attempting to open hardware decoder for codec: h264
    Loaded hardware-decoded frame 125 to GPU texture

  Test Result: ✓ PASS
  Detected codec: h264
  Decoding path: GPU_HARDWARE
```

## Troubleshooting

### No test videos found
- Run `./tests/create_test_videos.sh` first to create test files
- Check that `video_test_files/` directory exists and contains test files

### Videocomposer binary not found
- Build videocomposer first: `cd build && make cuems-videocomposer`
- Or specify path: `--videocomposer /path/to/cuems-videocomposer`

### Hardware decoder not detected
- Check that hardware acceleration is available: `ffmpeg -hwaccels`
- Check system logs for hardware decoder initialization
- Hardware decoding may not be available on all systems

### Test timeout
- Reduce test duration: `--duration 3`
- Check that videocomposer is not waiting for MIDI input
- Ensure test videos are valid and can be decoded

## Integration with CI/CD

The test script exits with code 0 on success, 1 on failure, making it suitable for CI/CD:

```bash
python3 tests/test_codec_formats.py --test-all --duration 3
```

