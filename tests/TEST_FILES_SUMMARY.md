# Test Video Files Summary

## Created Test Files

The `create_test_videos.sh` script creates test files with different codecs and formats for comprehensive testing.

### Hardware-Decodable Codecs

#### H.264 (AVC)
- `test_h264_mp4.mp4` - MP4 container
- `test_h264_mov.mov` - MOV container
- `test_h264_avi.avi` - AVI container
- **Expected**: GPU_HARDWARE (if VAAPI/CUDA available) or CPU_SOFTWARE

#### HEVC (H.265)
- `test_hevc_mp4.mp4` - MP4 container
- `test_hevc_mkv.mkv` - MKV container
- **Expected**: GPU_HARDWARE (if VAAPI/CUDA available) or CPU_SOFTWARE

#### AV1
- `test_av1_mp4.mp4` - MP4 container
- **Expected**: GPU_HARDWARE (if hardware decoder available) or CPU_SOFTWARE
- **Note**: AV1 hardware decoding requires newer hardware

### GPU-Optimized Codec

#### HAP (Hardware Accelerated Performance)
- `test_hap.mov` - MOV container with HAP codec
- **Expected**: HAP_DIRECT (zero-copy GPU decoding)
- **Note**: This is the optimal codec for multiple concurrent streams

### Software-Only Codecs

#### VP9
- `test_vp9_webm.webm` - WebM container
- **Expected**: CPU_SOFTWARE
- **Note**: VP9 is typically software-decoded

#### MPEG-4
- `test_mpeg4_avi.avi` - AVI container
- **Expected**: CPU_SOFTWARE
- **Note**: Legacy codec, software-decoded

### Frame Rate Variants

- `test_h264_24fps.mp4` - 24 fps
- `test_h264_30fps.mp4` - 30 fps
- `test_h264_50fps.mp4` - 50 fps
- **Purpose**: Test framerate conversion and sync

### Resolution Variants

- `test_h264_720p.mp4` - 1280x720
- `test_h264_480p.mp4` - 854x480
- **Purpose**: Test different resolutions and scaling

## Usage

### Quick Test
```bash
# Test a specific codec
python3 tests/test_codec_formats.py --video test_hap.mov --duration 3

# Test all hardware codecs
python3 tests/test_codec_formats.py --test-hw --duration 3

# Test all software codecs
python3 tests/test_codec_formats.py --test-sw --duration 3
```

### Full Test Suite
```bash
# Test everything
python3 tests/test_codec_formats.py --test-all --duration 5
```

## File Sizes

Test files are created with 30-second clips (configurable) to keep file sizes manageable while providing enough content for testing.

Typical sizes:
- H.264: ~7-8 MB (30s, 1080p)
- HEVC: ~5 MB (30s, 1080p, better compression)
- AV1: ~4 MB (30s, 1080p, best compression)
- HAP: ~180 MB (30s, 1080p, uncompressed GPU texture format)
- MPEG-4: ~19 MB (30s, 1080p, older codec)
- VP9: Variable (depends on encoder availability)

## Regenerating Test Files

To regenerate test files:
```bash
./tests/create_test_videos.sh "" video_test_files 30
```

This will skip existing files (won't overwrite). To force regeneration, delete the files first:
```bash
rm video_test_files/test_*.{mp4,mov,avi,mkv,webm}
./tests/create_test_videos.sh "" video_test_files 30
```

