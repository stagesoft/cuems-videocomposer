# NDI Testing Quick Start

## Quick Test (5 minutes)

### 1. Get NDI Test Source

**Option A: NDI Test Patterns (Easiest)**
```bash
# Download from: https://ndi.video/tools
# Run the application - it will broadcast test patterns
```

**Option B: OBS Studio**
```bash
# Install OBS
sudo apt-get install obs-studio

# In OBS: Tools → NDI Output Settings → Enable NDI output
# Add any source (display capture, video file, etc.)
```

### 2. Find Source Name

The source name will look like:
- `DESKTOP-ABC (NDI Test Patterns)`
- `DESKTOP-XYZ (OBS)`

You can find it in:
- NDI Test Patterns window (shows source name)
- OBS NDI settings
- Or use: `python3 tests/test_ndi_discovery.py` (if implemented)

### 3. Test with videocomposer

```bash
# Build the application first
cd /path/to/cuems-videocomposer
mkdir -p build && cd build
cmake .. -DENABLE_NDI=ON
make -j$(nproc)

# Test with NDI source
./cuems-videocomposer --layer 1 --file "ndi://DESKTOP-ABC (NDI Test Patterns)"
```

### 4. Verify It Works

You should see:
- Video displaying on screen
- Log messages like:
  ```
  NDI: Connected to source: DESKTOP-ABC (NDI Test Patterns)
  NDI: Source format: 1920x1080 @ 30 fps
  ```

## Common Issues

### "NDI SDK not found"
```bash
# Check NDI SDK is installed
ls -la "/opt/NDI SDK for Linux/lib/x86_64-linux-gnu/libndi.so*"

# Or set NDI_SDK_DIR
export NDI_SDK_DIR=/path/to/ndi-sdk
```

### "Source not found"
- Check source name is exact (case-sensitive)
- Ensure NDI source is running
- Check network connectivity
- Try increasing timeout in code

### "No video frames"
- Check source is actually sending video (not just audio)
- Verify format is supported
- Check application logs for errors

## Automated Testing

```bash
# Run automated test
python3 tests/test_ndi_input.py \
  --source "DESKTOP-ABC (NDI Test Patterns)" \
  --duration 30 \
  --verbose
```

## Next Steps

- Test with multiple NDI sources
- Test with MTC sync: `--mtc`
- Test with OSC control: `--osc`
- See `NDI_TESTING_GUIDE.md` for detailed testing

