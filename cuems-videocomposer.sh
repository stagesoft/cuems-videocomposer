#!/bin/bash
#
# cuems-videocomposer wrapper script
# Sets NDI library path and launches videocomposer with all arguments
#

# Find the script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Default NDI SDK paths to check
NDI_PATHS=(
    "/opt/NDI SDK for Linux/lib/x86_64-linux-gnu"
    "$HOME/ndi-sdk/lib/x86_64-linux-gnu"
    "$HOME/NDI SDK for Linux/lib/x86_64-linux-gnu"
)

# Check if NDI_SDK_DIR is set
if [ -n "$NDI_SDK_DIR" ]; then
    if [ -d "$NDI_SDK_DIR/lib/x86_64-linux-gnu" ]; then
        NDI_LIB_PATH="$NDI_SDK_DIR/lib/x86_64-linux-gnu"
    elif [ -d "$NDI_SDK_DIR/lib" ]; then
        NDI_LIB_PATH="$NDI_SDK_DIR/lib"
    fi
fi

# If not set via environment, try default paths
if [ -z "$NDI_LIB_PATH" ]; then
    for path in "${NDI_PATHS[@]}"; do
        if [ -d "$path" ] && [ -f "$path/libndi.so.6" ] || [ -f "$path/libndi.so" ]; then
            NDI_LIB_PATH="$path"
            break
        fi
    done
fi

# Find videocomposer executable
VIDEOCOMPOSER_BIN=""

# Try build directory first (development)
if [ -f "$SCRIPT_DIR/build/cuems-videocomposer" ]; then
    VIDEOCOMPOSER_BIN="$SCRIPT_DIR/build/cuems-videocomposer"
# Try script directory
elif [ -f "$SCRIPT_DIR/cuems-videocomposer" ]; then
    VIDEOCOMPOSER_BIN="$SCRIPT_DIR/cuems-videocomposer"
# Try system path
elif command -v cuems-videocomposer >/dev/null 2>&1; then
    VIDEOCOMPOSER_BIN="cuems-videocomposer"
else
    echo "ERROR: cuems-videocomposer not found" >&2
    echo "  Checked:" >&2
    echo "    - $SCRIPT_DIR/build/cuems-videocomposer" >&2
    echo "    - $SCRIPT_DIR/cuems-videocomposer" >&2
    echo "    - system PATH" >&2
    exit 1
fi

# Set LD_LIBRARY_PATH if NDI library path found
if [ -n "$NDI_LIB_PATH" ]; then
    # Add to existing LD_LIBRARY_PATH if set, otherwise create new
    if [ -n "$LD_LIBRARY_PATH" ]; then
        export LD_LIBRARY_PATH="$NDI_LIB_PATH:$LD_LIBRARY_PATH"
    else
        export LD_LIBRARY_PATH="$NDI_LIB_PATH"
    fi
    
    # Verbose mode: show what we're doing (if --verbose or -v is in args)
    if [[ " $* " =~ [[:space:]]--verbose[[:space:]] ]] || [[ " $* " =~ [[:space:]]-v[[:space:]] ]]; then
        echo "NDI library path: $NDI_LIB_PATH" >&2
    fi
else
    # Warning if NDI SDK not found (but continue anyway - might not need NDI)
    if [[ " $* " =~ [[:space:]]--discover-ndi[[:space:]] ]] || [[ " $* " =~ [[:space:]]ndi:// ]]; then
        echo "WARNING: NDI SDK library path not found. NDI features may not work." >&2
        echo "  Set NDI_SDK_DIR environment variable or install NDI SDK to:" >&2
        for path in "${NDI_PATHS[@]}"; do
            echo "    - $path" >&2
        done
    fi
fi

# Execute videocomposer with all arguments
exec "$VIDEOCOMPOSER_BIN" "$@"

