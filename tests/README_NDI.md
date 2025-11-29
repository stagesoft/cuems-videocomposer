# NDI Testing Documentation

## Overview

This directory contains testing tools and documentation for NDI input functionality in cuems-videocomposer.

## Quick Links

- **[NDI_QUICK_START.md](NDI_QUICK_START.md)** - Get started in 5 minutes
- **[NDI_TESTING_GUIDE.md](NDI_TESTING_GUIDE.md)** - Comprehensive testing guide
- **[test_ndi_input.py](test_ndi_input.py)** - Automated test script
- **[test_ndi_discovery.py](test_ndi_discovery.py)** - NDI source discovery utility

## Testing Scripts

### test_ndi_input.py
Automated testing script for NDI input functionality.

**Basic usage:**
```bash
# Test with specific NDI source
python3 tests/test_ndi_input.py --source "DESKTOP-ABC (NDI Test Patterns)" --duration 30

# Auto-discover and test first available source
python3 tests/test_ndi_input.py --auto --duration 30

# Verbose output for debugging
python3 tests/test_ndi_input.py --source "YOUR-SOURCE" --verbose
```

**Options:**
- `--source NAME` - NDI source name to test
- `--auto` - Automatically use first available source
- `--discover` - Only discover sources, don't test
- `--duration SECONDS` - Test duration (default: 30)
- `--verbose` - Show all videocomposer logs
- `--mtc` - Enable MTC timecode sync
- `--osc` - Enable OSC control testing

### test_ndi_discovery.py
Simple utility to discover available NDI sources.

**Usage:**
```bash
python3 tests/test_ndi_discovery.py [--timeout SECONDS]
```

## Manual Testing

### Using Command Line

```bash
# Basic NDI input
./build/cuems-videocomposer "ndi://DESKTOP-ABC (NDI Test Patterns)"

# With layer specification
./build/cuems-videocomposer --layer 1 --file "ndi://DESKTOP-ABC (NDI Test Patterns)"

# Multiple sources (different layers)
./build/cuems-videocomposer \
  --layer 1 --file "ndi://Source1" \
  --layer 2 --file "ndi://Source2"
```

### Using OSC

```bash
# Start videocomposer
./build/cuems-videocomposer &

# Load NDI source via OSC
python3 -c "
from pythonosc import udp_client
c = udp_client.SimpleUDPClient('127.0.0.1', 7770)
c.send_message('/layer/1/load', ['ndi://DESKTOP-ABC (NDI Test Patterns)'])
"
```

## Getting NDI Test Sources

### Option 1: NDI Test Patterns (Recommended)
- Download: https://ndi.video/tools
- Provides test patterns (color bars, clock, etc.)
- Simple to use for basic testing

### Option 2: OBS Studio
- Install: `sudo apt-get install obs-studio`
- Enable NDI output in OBS: Tools → NDI Output Settings
- Can output any source as NDI

### Option 3: FFmpeg (if compiled with NDI)
```bash
ffmpeg -re -i test_video.mp4 -f libndi_newtek test_ndi_source
```

## Verification

### What to Look For

**Successful connection:**
```
NDI: Searching for source 'DESKTOP-ABC (NDI Test Patterns)' (timeout: 2000ms)
NDI: Found source 'DESKTOP-ABC (NDI Test Patterns)'
NDI: Connected to source: DESKTOP-ABC (NDI Test Patterns)
NDI: Source format: 1920x1080 @ 30 fps
```

**Statistics (every 30 seconds):**
```
NDI: 1800 frames, 0 dropped, avg capture: 33.1ms
```

**Errors to watch for:**
```
NDI: Source not found: ...
NDI: Failed to create receiver
NDI: 10 consecutive capture errors
```

### Performance Benchmarks

Expected performance on modern hardware:

| Resolution | FPS | CPU Usage | Latency |
|------------|-----|-----------|---------|
| 1920x1080  | 30  | 10-20%    | 50-100ms |
| 1920x1080  | 60  | 20-30%    | 30-60ms  |
| 3840x2160  | 30  | 30-50%    | 100-200ms |

## Troubleshooting

### No NDI Sources Found
- Ensure NDI source is running
- Check network connectivity
- Verify firewall allows NDI (UDP 5960-5969)
- Check NDI SDK is installed

### Connection Timeout
- Verify source name is exact (case-sensitive)
- Check source is broadcasting
- Increase timeout in code if needed

### No Video Frames
- Check source is sending video (not just audio)
- Verify format is supported
- Check application logs for errors

### High CPU Usage
- NDI is typically software-decoded
- Check buffer size (default: 3 frames)
- Monitor capture loop performance

## Next Steps

1. **Basic Testing**: Use NDI Test Patterns for initial testing
2. **Real Sources**: Test with OBS or production sources
3. **Multiple Sources**: Test with multiple simultaneous NDI inputs
4. **Integration**: Test with MTC sync and OSC control
5. **Performance**: Monitor statistics and optimize if needed

## Related Documentation

- [NDI Input Implementation Plan](../.cursor/plans/ndi-input-support.plan.md)
- [NDI SDK License Compliance](../debian/NDI-COMPLIANCE-SUMMARY.md)
- [Main Testing README](README.md)

