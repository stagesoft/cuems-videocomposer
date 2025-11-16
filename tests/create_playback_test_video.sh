#!/bin/bash
# Script to create a test video with moving patterns for visualizing playback problems
# This video includes:
# - Moving bars/stripes to detect frame drops and stuttering
# - Frame counter overlay to verify frame accuracy
# - Color gradients to detect compression artifacts
# - Smooth motion patterns

set -e

OUTPUT_DIR="${1:-$(dirname "$0")/../video_test_files}"
DURATION="${2:-50}"  # Duration in seconds
FPS="${3:-25}"        # Frame rate
RESOLUTION="${4:-1920x1080}"  # Resolution

OUTPUT_FILE="$OUTPUT_DIR/test_playback_patterns.mov"

# Create output directory if it doesn't exist
mkdir -p "$OUTPUT_DIR"

echo "Creating playback test video with moving patterns..."
echo "Output: $OUTPUT_FILE"
echo "Duration: ${DURATION}s"
echo "FPS: $FPS"
echo "Resolution: $RESOLUTION"

# Create a test video with multiple moving patterns
# Using a simpler approach with known-working FFmpeg filters
ffmpeg -y \
    -f lavfi \
    -i "testsrc2=duration=${DURATION}:size=${RESOLUTION}:rate=${FPS}" \
    -vf "drawtext=text='Frame %{n}':fontsize=48:fontcolor=white:x=10:y=10:box=1:boxcolor=black@0.7:boxborderw=5,drawtext=text='%{pts\:hms}':fontsize=36:fontcolor=white:x='w-text_w-10':y=10:box=1:boxcolor=black@0.7:boxborderw=5,drawbox=x='100*t':y=0:w=50:h=ih:color=red@0.8:t=fill,drawbox=x=0:y='100*t':w=iw:h=50:color=blue@0.8:t=fill,drawbox=x='iw/2-50+50*cos(2*PI*t/3)':y='ih/2-50+50*sin(2*PI*t/3)':w=100:h=100:color=white:t=fill" \
    -c:v prores_ks \
    -profile:v 3 \
    -pix_fmt yuv422p10le \
    -r $FPS \
    -t $DURATION \
    "$OUTPUT_FILE" 2>&1 | tail -20

if [ -f "$OUTPUT_FILE" ]; then
    FILE_SIZE=$(du -h "$OUTPUT_FILE" | cut -f1)
    echo ""
    echo "✓ Successfully created: $OUTPUT_FILE"
    echo "  Size: $FILE_SIZE"
    echo ""
    echo "This test video includes:"
    echo "  - Moving colored bars (red vertical, blue horizontal)"
    echo "  - Frame counter overlay (top-left)"
    echo "  - Timestamp overlay (top-right)"
    echo "  - Rotating white box pattern in center"
    echo "  - Base test pattern with gradients"
    echo ""
    echo "Video encoded with ProRes 422 HQ codec (MOV container)"
    echo "  - Professional-grade codec, minimal compression artifacts"
    echo "  - High quality suitable for professional video production"
    echo "  - Larger file size but excellent quality for testing"
    echo ""
    echo "Use this video to detect:"
    echo "  - Frame drops (check frame counter jumps)"
    echo "  - Stuttering (watch smooth motion patterns)"
    echo "  - Playback artifacts (any visual issues are from playback, not compression)"
    echo "  - Sync issues (compare timestamp with MTC)"
else
    echo "✗ Failed to create test video"
    exit 1
fi

