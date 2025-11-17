# cuems-videocomposer Integration Tests

This directory contains integration tests for cuems-videocomposer.

## test_mtc_integration.py

Integration test that verifies MTC (MIDI Time Code) synchronization works correctly with videocomposer.

### What it does

1. Starts MTC timecode generation using libmtcmaster Python interface
2. Launches videocomposer with a test video file
3. Monitors the application to ensure it runs without errors
4. Verifies that MTC sync is working

### Prerequisites

- Built videocomposer executable in `build/cuems-videocomposer`
- libmtcmaster library available at `../libmtcmaster/libmtcmaster.so`
- Test video file (default: `ORIGIN_HD_422_25FPS_709.mov`)
- ALSA MIDI support (for MTC transmission)

### Usage

Basic usage (uses default video file `ORIGIN_HD_422_25FPS_709.mov`):

```bash
python3 tests/test_mtc_integration.py
```

With custom video file:

```bash
python3 tests/test_mtc_integration.py --video-path /path/to/video.mov
```

With custom test duration:

```bash
python3 tests/test_mtc_integration.py --duration 30
```

With custom MTC framerate:

```bash
python3 tests/test_mtc_integration.py --fps 29.97
```

Full options:

```bash
python3 tests/test_mtc_integration.py --help
```

### Test Duration

By default, the test runs for 10 seconds. You can adjust this with the `--duration` option.

### Exit Codes

- `0`: Test passed - videocomposer ran successfully for the test duration
- `1`: Test failed - videocomposer crashed or exited with an error

### Notes

- The test requires ALSA MIDI to be available
- Both libmtcmaster and videocomposer automatically connect to "Midi Through" if available, so no manual port connection is needed
- The MTC sender uses ALSA port 0 by default (can be changed with `--mtc-port`)
- The test will automatically clean up (stop MTC and terminate videocomposer) on exit

