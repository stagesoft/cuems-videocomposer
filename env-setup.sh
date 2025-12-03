#!/bin/bash
#
# Environment setup for cuems-videocomposer
# Usage: source env-setup.sh
#        Then run: ./build/cuems-videocomposer [args]
#

# NDI SDK paths to check
NDI_PATHS=(
    "/opt/NDI SDK for Linux/lib/x86_64-linux-gnu"
    "$HOME/ndi-sdk/lib/x86_64-linux-gnu"
    "$HOME/NDI SDK for Linux/lib/x86_64-linux-gnu"
)

# Check if NDI_SDK_DIR is already set
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
        if [ -d "$path" ]; then
            NDI_LIB_PATH="$path"
            break
        fi
    done
fi

# Set LD_LIBRARY_PATH
if [ -n "$NDI_LIB_PATH" ]; then
    if [ -n "$LD_LIBRARY_PATH" ]; then
        export LD_LIBRARY_PATH="$NDI_LIB_PATH:$LD_LIBRARY_PATH"
    else
        export LD_LIBRARY_PATH="$NDI_LIB_PATH"
    fi
    echo "✓ NDI library path set: $NDI_LIB_PATH"
    echo "  LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
else
    echo "⚠ NDI SDK not found. Checked:"
    for path in "${NDI_PATHS[@]}"; do
        echo "    - $path"
    done
    echo "  Set NDI_SDK_DIR if installed elsewhere."
fi

echo ""
echo "You can now run:"
echo "  ./build/cuems-videocomposer [your args]"
echo "  sudo ./build/cuems-videocomposer [your args]  # for DRM output"

