# NDI Testing Guide

This guide explains how to test NDI input functionality in cuems-videocomposer.

## Prerequisites

### 1. NDI SDK Installed
Ensure NDI SDK is installed and accessible:
```bash
# Check if NDI SDK is found
ls -la "/opt/NDI SDK for Linux/lib/x86_64-linux-gnu/libndi.so*"

# Or set NDI_SDK_DIR if installed elsewhere
export NDI_SDK_DIR=/path/to/ndi-sdk
```

### 2. NDI Test Sources
You need at least one NDI source to test with. Options:

#### Option A: NDI Test Patterns (Recommended)
Download from: https://ndi.video/tools
- Provides test patterns (color bars, clock, etc.)
- Available for Windows, macOS, and Linux
- Simple to use for basic testing

#### Option B: OBS Studio
Install OBS Studio with NDI plugin:
```bash
# Ubuntu/Debian
sudo apt-get install obs-studio

# Or download from: https://obsproject.com/
```
- Can output any source as NDI
- Good for testing with real video content

#### Option C: FFmpeg NDI Output
If you have FFmpeg compiled with NDI support:
```bash
ffmpeg -re -i test_video.mp4 -f libndi_newtek test_ndi_source
```

#### Option D: Another Application
Any application that outputs NDI (vMix, Wirecast, etc.)

## Testing Methods

### Method 1: Manual Testing

#### Step 1: Discover NDI Sources
```bash
# Use the test script to discover sources
python3 tests/test_ndi_discovery.py

# Or use NDI Test Patterns application
# It will show up as "NDI Test Patterns" in the list
```

#### Step 2: Start NDI Source
- Launch NDI Test Patterns (or your NDI source)
- Note the source name (e.g., "DESKTOP-ABC (NDI Test Patterns)")

#### Step 3: Test with videocomposer
```bash
# Method A: Using ndi:// prefix
./build/cuems-videocomposer --layer 1 --file "ndi://DESKTOP-ABC (NDI Test Patterns)"

# Method B: Using source name directly
./build/cuems-videocomposer --layer 1 --file "DESKTOP-ABC (NDI Test Patterns)"

# Method C: Using OSC to load NDI source
./build/cuems-videocomposer &
# Then send OSC command:
python3 -c "from pythonosc import udp_client; c = udp_client.SimpleUDPClient('127.0.0.1', 7770); c.send_message('/layer/1/load', ['ndi://DESKTOP-ABC (NDI Test Patterns)'])"
```

### Method 2: Automated Testing

Use the automated test script:
```bash
# Test NDI discovery
python3 tests/test_ndi_input.py --discover

# Test with specific NDI source
python3 tests/test_ndi_input.py --source "DESKTOP-ABC (NDI Test Patterns)" --duration 30

# Test with auto-discovery (uses first available source)
python3 tests/test_ndi_input.py --auto --duration 30

# Verbose output for debugging
python3 tests/test_ndi_input.py --source "YOUR-SOURCE" --duration 10 --verbose
```

### Method 3: Integration Testing

Test NDI alongside other features:
```bash
# Test NDI + MTC sync
python3 tests/test_ndi_input.py --source "YOUR-SOURCE" --mtc --duration 30

# Test NDI + OSC control
python3 tests/test_ndi_input.py --source "YOUR-SOURCE" --osc --duration 30
```

## Verification Checklist

### Basic Functionality
- [ ] NDI sources are discovered
- [ ] Application connects to NDI source
- [ ] Video frames are received
- [ ] Video displays correctly
- [ ] Frame rate is correct
- [ ] Resolution is correct

### Error Handling
- [ ] Invalid source name is handled gracefully
- [ ] Connection timeout works
- [ ] Source disconnection is handled
- [ ] Error messages are clear

### Performance
- [ ] No frame drops (check statistics)
- [ ] Low latency (check capture times)
- [ ] CPU usage is reasonable
- [ ] Memory usage is stable

### Statistics
Check application logs for:
```
NDI: Connected to source: ...
NDI: Source format: 1920x1080 @ 30 fps
NDI: 1000 frames, 0 dropped, avg capture: 33.3ms
```

## Troubleshooting

### No NDI Sources Found

**Problem**: `test_ndi_discovery.py` shows no sources

**Solutions**:
1. Ensure NDI source is running
2. Check network connectivity (NDI uses multicast)
3. Check firewall (NDI uses UDP port 5960-5969)
4. Verify NDI SDK is properly installed

```bash
# Check NDI SDK
ls -la "/opt/NDI SDK for Linux/lib/x86_64-linux-gnu/libndi.so*"

# Check network
netstat -ulnp | grep 5960
```

### Connection Timeout

**Problem**: Application times out connecting to NDI source

**Solutions**:
1. Verify source name is correct (case-sensitive)
2. Check source is actually broadcasting
3. Increase timeout in code (default: 5 seconds)
4. Check network connectivity

### No Video Frames

**Problem**: Connected but no video appears

**Solutions**:
1. Check application logs for errors
2. Verify source is sending video (not just audio)
3. Check frame buffer statistics
4. Verify format is supported (RGBA32)

### High CPU Usage

**Problem**: CPU usage is very high

**Solutions**:
1. Check if hardware decoding is available (NDI is typically software-decoded)
2. Reduce buffer size (default: 3 frames)
3. Check capture loop performance
4. Profile with `perf` or `valgrind`

## Advanced Testing

### Test Multiple NDI Sources
```bash
# Load multiple NDI sources on different layers
./build/cuems-videocomposer \
  --layer 1 --file "ndi://Source1" \
  --layer 2 --file "ndi://Source2" \
  --layer 3 --file "ndi://Source3"
```

### Test NDI + File Sources
```bash
# Mix NDI and file sources
./build/cuems-videocomposer \
  --layer 1 --file "ndi://NDI Source" \
  --layer 2 --file "video_test_files/test_h264_mp4.mp4"
```

### Test NDI Statistics
The application logs statistics every 30 seconds:
```
NDI: 1800 frames, 2 dropped, avg capture: 33.1ms
```

Monitor these to verify:
- Frame capture rate matches source framerate
- Low drop count (should be 0 for good network)
- Reasonable capture time (< 50ms for 30fps)

## Network Testing

### Test on Same Machine
- Use loopback interface
- Should have minimal latency
- Good for basic functionality testing

### Test on Local Network
- Use multicast (default NDI behavior)
- Test with different network conditions
- Verify firewall doesn't block NDI

### Test Across Subnets
- May require NDI Bridge or Router configuration
- Test with different network topologies

## Performance Benchmarks

Expected performance on modern hardware:

| Resolution | FPS | CPU Usage | Latency |
|------------|-----|-----------|---------|
| 1920x1080  | 30  | 10-20%    | 50-100ms |
| 1920x1080  | 60  | 20-30%    | 30-60ms  |
| 3840x2160  | 30  | 30-50%    | 100-200ms |

*Note: These are approximate values. Actual performance depends on hardware, network, and source encoding.*

## Next Steps

After basic testing:
1. Test with real production sources
2. Test with multiple simultaneous sources
3. Test with different resolutions and frame rates
4. Test error recovery (source disconnection/reconnection)
5. Test with MTC sync enabled
6. Test with OSC control

