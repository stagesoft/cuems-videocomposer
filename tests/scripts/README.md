# cuems-videocomposer Wrapper Script

## Overview

The `cuems-videocomposer.sh` wrapper script automatically sets the NDI library path and launches videocomposer with all your arguments. This eliminates the need to manually set `LD_LIBRARY_PATH` every time.

**Note**: This wrapper is **NOT included in the Debian package** because the package already sets rpath correctly. The wrapper is only needed for development/testing scenarios.

## Usage

### Basic Usage

```bash
# From the project root directory
./cuems-videocomposer.sh [videocomposer arguments]

# Examples:
./cuems-videocomposer.sh --discover-ndi
./cuems-videocomposer.sh "ndi://EXPLINUX (OBS _PGM)"
./cuems-videocomposer.sh --layer 1 --file "ndi://Source Name"
./cuems-videocomposer.sh video_test_files/test_h264_mp4.mp4
```

### System-Wide Installation

To use the script system-wide:

```bash
# Copy to a directory in your PATH
sudo cp cuems-videocomposer.sh /usr/local/bin/cuems-videocomposer
sudo chmod +x /usr/local/bin/cuems-videocomposer

# Now you can use it from anywhere
cuems-videocomposer --discover-ndi
```

Or create a symlink:

```bash
sudo ln -s /path/to/cuems-videocomposer/cuems-videocomposer.sh /usr/local/bin/cuems-videocomposer
```

## How It Works

1. **Finds NDI SDK**: Automatically detects NDI SDK in common locations:
   - `/opt/NDI SDK for Linux/lib/x86_64-linux-gnu` (default)
   - `$HOME/ndi-sdk/lib/x86_64-linux-gnu`
   - `$NDI_SDK_DIR` environment variable (if set)

2. **Sets Library Path**: Adds NDI library path to `LD_LIBRARY_PATH`

3. **Finds Executable**: Looks for videocomposer in:
   - `build/cuems-videocomposer` (development build)
   - Script directory
   - System PATH

4. **Passes Arguments**: All arguments are passed through to videocomposer

## Environment Variables

- `NDI_SDK_DIR`: Override NDI SDK location
  ```bash
  export NDI_SDK_DIR=/custom/path/to/ndi-sdk
  ./cuems-videocomposer.sh --discover-ndi
  ```

- `LD_LIBRARY_PATH`: If already set, NDI path is prepended

## Examples

### Discover NDI Sources
```bash
./cuems-videocomposer.sh --discover-ndi 5
```

### Load NDI Source
```bash
./cuems-videocomposer.sh "ndi://EXPLINUX (OBS _PGM)"
```

### Load Video File
```bash
./cuems-videocomposer.sh video_test_files/test_h264_mp4.mp4
```

### With OSC Control
```bash
./cuems-videocomposer.sh --osc 7770 "ndi://Source Name"
```

### Verbose Mode
```bash
./cuems-videocomposer.sh --verbose --discover-ndi
# Shows: "NDI library path: /opt/NDI SDK for Linux/lib/x86_64-linux-gnu"
```

## Troubleshooting

### Script Not Found
```bash
# Make sure it's executable
chmod +x cuems-videocomposer.sh

# Or use full path
/path/to/cuems-videocomposer/cuems-videocomposer.sh --help
```

### NDI Library Not Found
The script will warn if NDI SDK is not found (only when using NDI features):
```
WARNING: NDI SDK library path not found. NDI features may not work.
```

Solutions:
1. Install NDI SDK to `/opt/NDI SDK for Linux/`
2. Set `NDI_SDK_DIR` environment variable
3. The script will continue anyway (for non-NDI usage)

### Executable Not Found
The script checks multiple locations. If none found:
```
ERROR: cuems-videocomposer not found
```

Solutions:
1. Build the application: `cd build && cmake .. && make`
2. Install the Debian package
3. Add videocomposer to your PATH

## Integration with Tests

The test scripts can use this wrapper:

```bash
# In test scripts, use:
VIDEOCOMPOSER_BIN="./cuems-videocomposer.sh"

# Or if installed system-wide:
VIDEOCOMPOSER_BIN="cuems-videocomposer"
```

## Benefits

✅ **No manual LD_LIBRARY_PATH setup**  
✅ **Works in development and production**  
✅ **Automatic NDI SDK detection**  
✅ **All arguments passed through**  
✅ **Helpful error messages**

