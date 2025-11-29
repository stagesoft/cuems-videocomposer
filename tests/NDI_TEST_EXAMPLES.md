# NDI Test Examples

## Using NDI Sources in Tests

The test scripts now support NDI sources alongside regular video files.

## test_dynamic_file_management.py

### Mix File and NDI Source

```bash
# Video file on layer 1, NDI source on layer 2
python3 tests/test_dynamic_file_management.py \
  --video1 video_test_files/test_av1_mp4.mp4 \
  --video2 "ndi://EXPLINUX (OBS _PGM)"
```

### Two NDI Sources

```bash
# Two different NDI sources
python3 tests/test_dynamic_file_management.py \
  --video1 "ndi://EXPLINUX (OBS _PGM)" \
  --video2 "ndi://DESKTOP-ABC (NDI Test Patterns)"
```

### NDI Source with Looping

```bash
# Note: Looping may not work for live NDI sources (they're continuous streams)
python3 tests/test_dynamic_file_management.py \
  --video1 "ndi://EXPLINUX (OBS _PGM)" \
  --loop
```

### Using Wrapper Script

The test automatically uses the wrapper script if the direct binary isn't found:

```bash
# Uses cuems-videocomposer.sh automatically if build/cuems-videocomposer doesn't exist
python3 tests/test_dynamic_file_management.py \
  --video1 video_test_files/test_av1_mp4.mp4 \
  --video2 "ndi://EXPLINUX (OBS _PGM)"
```

Or explicitly specify the wrapper:

```bash
python3 tests/test_dynamic_file_management.py \
  --videocomposer ./cuems-videocomposer.sh \
  --video1 "ndi://EXPLINUX (OBS _PGM)" \
  --video2 video_test_files/test_h264_mp4.mp4
```

## Supported Source Types

The test script automatically detects and handles:

- **File paths**: `/path/to/video.mp4`
- **NDI sources**: `ndi://Source Name` or just `Source Name`
- **V4L2 devices**: `/dev/video0`
- **Network streams**: `rtsp://...`, `http://...`

## Notes

- **File existence check**: Skipped for NDI sources and live streams
- **Looping**: May not work as expected for live NDI sources (they're continuous)
- **MTC sync**: Works with NDI sources (they follow MTC timecode)
- **OSC control**: All layer controls work with NDI sources

## Example Test Run

```bash
$ python3 tests/test_dynamic_file_management.py \
    --video1 video_test_files/test_av1_mp4.mp4 \
    --video2 "ndi://EXPLINUX (OBS _PGM)" \
    --fps 25.0

=== Test: Load layer with file ===
Cue ID: 550e8400-e29b-41d4-a716-446655440000
File: video_test_files/test_av1_mp4.mp4
Sent: /videocomposer/layer/load ('video_test_files/test_av1_mp4.mp4', '550e8400-e29b-41d4-a716-446655440000')

=== Test: Load layer with file ===
Cue ID: 660e8400-e29b-41d4-a716-446655440001
File: ndi://EXPLINUX (OBS _PGM)
Sent: /videocomposer/layer/load ('ndi://EXPLINUX (OBS _PGM)', '660e8400-e29b-41d4-a716-446655440001')
```

