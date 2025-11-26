#!/bin/bash
# Build script for VAAPI EGL minimal test

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "Building VAAPI EGL minimal test..."

g++ -o vaapi_egl_test vaapi_egl_minimal_test.cpp \
    $(pkg-config --cflags --libs libavformat libavcodec libavutil egl gl x11 libdrm libva libva-drm) \
    -lX11 -lGL -lEGL \
    -g -O0

echo "Build successful!"
echo ""
echo "Run with:"
echo "  ./vaapi_egl_test ../video_test_files/vaapi_debug_framenum.mp4"

