# NDI Discovery Implementation

## ✅ Implementation Complete

NDI source discovery has been fully implemented and is now available in videocomposer.

## Usage

### Command Line

```bash
# Discover NDI sources (default 5 second timeout)
./build/cuems-videocomposer --discover-ndi

# With custom timeout
./build/cuems-videocomposer --discover-ndi 10
```

### Python Script

```bash
# Use the test script
python3 tests/test_ndi_discovery.py --timeout 5
```

### From Python Code

```python
from test_ndi_discovery import discover_ndi_sources

sources = discover_ndi_sources(timeout_seconds=5)
for source in sources:
    print(f"Found: {source}")
```

## Output Format

When sources are found:
```
NDI Source Discovery
==================================================
Timeout: 5 seconds
Searching for NDI sources...

Found 2 NDI source(s):

  1. DESKTOP-ABC (NDI Test Patterns)
  2. DESKTOP-XYZ (OBS)

To use with videocomposer:
  ./cuems-videocomposer "ndi://DESKTOP-ABC (NDI Test Patterns)"
```

When no sources found:
```
NDI Source Discovery
==================================================
Timeout: 5 seconds
Searching for NDI sources...

No NDI sources found.

Troubleshooting:
  1. Ensure an NDI source is running (NDI Test Patterns, OBS, etc.)
  2. Check network connectivity
  3. Verify firewall allows NDI (UDP ports 5960-5969)
  4. Try increasing timeout: --discover-ndi 10
```

## Runtime Library Path

If you get an error about `libndi.so.6` not found:

```bash
# Set LD_LIBRARY_PATH
export LD_LIBRARY_PATH="/opt/NDI SDK for Linux/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH"

# Or use the full path
LD_LIBRARY_PATH="/opt/NDI SDK for Linux/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH" \
  ./build/cuems-videocomposer --discover-ndi
```

When installed via Debian package, the rpath is set automatically.

## Integration

The discovery is now integrated into:
- ✅ Command-line tool (`--discover-ndi` flag)
- ✅ Python test script (`test_ndi_discovery.py`)
- ✅ Automated testing (`test_ndi_input.py --auto`)

## Technical Details

- Uses `NDIVideoInput::discoverSources()` method
- Implements proper NDI SDK initialization/cleanup
- Handles timeouts gracefully
- Provides helpful error messages

## Next Steps

1. **Test with actual NDI sources**: Start NDI Test Patterns or OBS and run discovery
2. **Use in automated tests**: The `--auto` flag in `test_ndi_input.py` now works
3. **Integrate into workflows**: Use discovery to find sources before connecting

