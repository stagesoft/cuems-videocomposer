#!/bin/bash
# Script to create test video files with different codecs and formats
# Uses FFmpeg to transcode source videos into various formats

set -e

SOURCE_DIR="${1:-/home/Videos}"
OUTPUT_DIR="${2:-$(dirname "$0")/../video_test_files}"
DURATION="${3:-30}"  # Duration in seconds for test clips

# Create output directory
mkdir -p "$OUTPUT_DIR"

echo "Creating test video files..."
echo "Source directory: $SOURCE_DIR"
echo "Output directory: $OUTPUT_DIR"
echo "Duration: ${DURATION}s"

# Find a source video file
SOURCE_VIDEO=""
if [ -d "$SOURCE_DIR" ]; then
    SOURCE_VIDEO=$(find "$SOURCE_DIR" -type f \( -name "*.mp4" -o -name "*.mov" -o -name "*.avi" -o -name "*.mkv" \) -size +10M 2>/dev/null | head -1)
fi

# If no source found, try to use existing test file
if [ -z "$SOURCE_VIDEO" ]; then
    if [ -f "$OUTPUT_DIR/problematic.mp4" ]; then
        SOURCE_VIDEO="$OUTPUT_DIR/problematic.mp4"
        echo "Using existing test file: $SOURCE_VIDEO"
    else
        echo "ERROR: No source video found. Please provide a source video file."
        echo "Usage: $0 [SOURCE_DIR] [OUTPUT_DIR] [DURATION]"
        exit 1
    fi
fi

if [ ! -f "$SOURCE_VIDEO" ]; then
    echo "ERROR: Source video not found: $SOURCE_VIDEO"
    exit 1
fi

echo "Using source video: $SOURCE_VIDEO"

# Function to create test video
create_test_video() {
    local output="$1"
    local codec="$2"
    local extra_args="$3"
    local format="$4"
    
    echo "Creating: $output (codec: $codec, format: $format)"
    
    if [ -f "$output" ]; then
        echo "  Skipping (already exists)"
        return
    fi
    
    ffmpeg -y -i "$SOURCE_VIDEO" \
        -t "$DURATION" \
        -c:v "$codec" \
        $extra_args \
        -c:a copy \
        -f "$format" \
        "$output" 2>&1 | grep -E "(error|Error|ERROR)" || true
    
    if [ -f "$output" ]; then
        echo "  ✓ Created successfully"
    else
        echo "  ✗ Failed to create"
    fi
}

# Create H.264 test files (hardware decodable)
echo ""
echo "=== Creating H.264 test files ==="
create_test_video "$OUTPUT_DIR/test_h264_mp4.mp4" "libx264" "-preset fast -crf 23" "mp4"
create_test_video "$OUTPUT_DIR/test_h264_mov.mov" "libx264" "-preset fast -crf 23" "mov"
create_test_video "$OUTPUT_DIR/test_h264_avi.avi" "libx264" "-preset fast -crf 23" "avi"

# Create HEVC test files (hardware decodable)
echo ""
echo "=== Creating HEVC test files ==="
create_test_video "$OUTPUT_DIR/test_hevc_mp4.mp4" "libx265" "-preset fast -crf 23" "mp4"
create_test_video "$OUTPUT_DIR/test_hevc_mkv.mkv" "libx265" "-preset fast -crf 23" "matroska"

# Create AV1 test files (hardware decodable on newer hardware)
echo ""
echo "=== Creating AV1 test files ==="
create_test_video "$OUTPUT_DIR/test_av1_mp4.mp4" "libaom-av1" "-cpu-used 4 -crf 30" "mp4" || \
    create_test_video "$OUTPUT_DIR/test_av1_mp4.mp4" "libsvtav1" "-preset 4 -crf 30" "mp4" || \
    echo "  ⚠ AV1 encoder not available, skipping"

# Create VP9 test files (software codec)
echo ""
echo "=== Creating VP9 test files ==="
create_test_video "$OUTPUT_DIR/test_vp9_webm.webm" "libvpx-vp9" "-cpu-used 2 -crf 30" "webm" || \
    echo "  ⚠ VP9 encoder not available, skipping"

# Create HAP test files (GPU-optimized codec)
echo ""
echo "=== Creating HAP test files ==="
create_test_video "$OUTPUT_DIR/test_hap.mov" "hap" "-format hap" "mov" || \
    echo "  ⚠ HAP encoder not available, skipping"

# Create software-only codec (MPEG-4)
echo ""
echo "=== Creating MPEG-4 test files (software codec) ==="
create_test_video "$OUTPUT_DIR/test_mpeg4_avi.avi" "mpeg4" "-qscale:v 3" "avi"

# Create different frame rates
echo ""
echo "=== Creating different frame rate test files ==="
create_test_video "$OUTPUT_DIR/test_h264_24fps.mp4" "libx264" "-preset fast -crf 23 -r 24" "mp4"
create_test_video "$OUTPUT_DIR/test_h264_30fps.mp4" "libx264" "-preset fast -crf 23 -r 30" "mp4"
create_test_video "$OUTPUT_DIR/test_h264_50fps.mp4" "libx264" "-preset fast -crf 23 -r 50" "mp4"

# Create different resolutions
echo ""
echo "=== Creating different resolution test files ==="
create_test_video "$OUTPUT_DIR/test_h264_720p.mp4" "libx264" "-preset fast -crf 23 -vf scale=1280:720" "mp4"
create_test_video "$OUTPUT_DIR/test_h264_480p.mp4" "libx264" "-preset fast -crf 23 -vf scale=854:480" "mp4"

echo ""
echo "=== Test video creation complete ==="
echo "Test files created in: $OUTPUT_DIR"
ls -lh "$OUTPUT_DIR"/test_*.{mp4,mov,avi,mkv,webm} 2>/dev/null | awk '{print $9, $5}'

