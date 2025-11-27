#!/bin/bash
#
# Helper script to create HAP Q Alpha test file
# 
# This script provides instructions and automation for creating HAP Q Alpha files
# using various tools since FFmpeg doesn't support it.
#
# Usage:
#   ./create_hap_q_alpha.sh [input_file] [output_file]
#

set -e

INPUT_FILE="${1:-video_test_files/test_with_alpha.mov}"
OUTPUT_FILE="${2:-video_test_files/test_hap_hq_alpha.mov}"

echo "=========================================="
echo "HAP Q Alpha Encoder Helper"
echo "=========================================="
echo ""
echo "Input:  $INPUT_FILE"
echo "Output: $OUTPUT_FILE"
echo ""

# Check if input file exists
if [ ! -f "$INPUT_FILE" ]; then
    echo "❌ Error: Input file not found: $INPUT_FILE"
    exit 1
fi

# Check if FFmpeg supports HAP Q Alpha
echo "Checking FFmpeg HAP encoder support..."
if ffmpeg -h encoder=hap 2>&1 | grep -q "hap_q_alpha"; then
    echo "✅ FFmpeg supports HAP Q Alpha! Using FFmpeg..."
    echo "Encoding with optimized settings (4 chunks, snappy compression)..."
    ffmpeg -y -i "$INPUT_FILE" -c:v hap -format hap_q_alpha -chunks 4 -compressor snappy -pix_fmt rgba "$OUTPUT_FILE"
    if [ $? -eq 0 ]; then
        echo "✅ Created: $OUTPUT_FILE"
        exit 0
    else
        echo "❌ Encoding failed, trying without optimization..."
        ffmpeg -y -i "$INPUT_FILE" -c:v hap -format hap_q_alpha -pix_fmt rgba "$OUTPUT_FILE"
        if [ $? -eq 0 ]; then
            echo "✅ Created: $OUTPUT_FILE"
            exit 0
        fi
    fi
else
    echo "❌ FFmpeg does not support HAP Q Alpha encoding in this build"
    echo "   Available formats: $(ffmpeg -h encoder=hap 2>&1 | grep -A 5 'format.*<int>' | grep -E 'hap|hap_alpha|hap_q' | awk '{print $1}' | tr '\n' ' ')"
    echo ""
fi

# Check for Adobe Media Encoder (macOS)
if [ -d "/Applications/Adobe Media Encoder"* ] 2>/dev/null; then
    AME_PATH=$(ls -d "/Applications/Adobe Media Encoder"* 2>/dev/null | head -1)
    echo "✅ Found Adobe Media Encoder: $AME_PATH"
    echo ""
    echo "To create HAP Q Alpha file:"
    echo "1. Install HAP Exporter for Adobe CC:"
    echo "   https://github.com/disguise-one/hap-encoder-adobe-cc"
    echo ""
    echo "2. Open Adobe Media Encoder"
    echo "3. Import: $INPUT_FILE"
    echo "4. Export as: HAP Video → HAP Q Alpha"
    echo "5. Save to: $OUTPUT_FILE"
    echo ""
    echo "Or use command-line (if AME CLI is configured):"
    echo "  \"$AME_PATH\" -render -output \"$OUTPUT_FILE\" ..."
    exit 0
fi

# Check for Adobe Media Encoder (Windows - if running in WSL)
if command -v cmd.exe >/dev/null 2>&1; then
    echo "⚠️  Windows detected (via WSL)"
    echo ""
    echo "To create HAP Q Alpha on Windows:"
    echo "1. Install HAP Exporter for Adobe CC:"
    echo "   https://github.com/disguise-one/hap-encoder-adobe-cc"
    echo ""
    echo "2. Use Adobe Media Encoder GUI or command-line"
    echo ""
fi

# No suitable tool found
echo "=========================================="
echo "No HAP Q Alpha encoder found"
echo "=========================================="
echo ""
echo "FFmpeg does not support HAP Q Alpha encoding."
echo ""
echo "Options to create HAP Q Alpha:"
echo ""
echo "1. HAP Exporter for Adobe CC (Free, Open Source)"
echo "   - GitHub: https://github.com/disguise-one/hap-encoder-adobe-cc"
echo "   - Install in Adobe Media Encoder / Premiere / After Effects"
echo "   - Export as 'HAP Video' → 'HAP Q Alpha'"
echo ""
echo "2. AfterCodecs (Commercial)"
echo "   - Website: https://www.autokroma.com/AfterCodecs/"
echo "   - Professional plugin with GPU acceleration"
echo ""
echo "3. HAP QuickTime Codec (macOS/Windows)"
echo "   - Codec: https://github.com/Vidvox/hap-qt-codec/releases/"
echo "   - Use with QuickTime-compatible software"
echo ""
echo "4. Manual creation"
echo "   - Use any of the above tools to create the file"
echo "   - Place it at: $OUTPUT_FILE"
echo ""
echo "For more details, see: tests/CREATE_HAP_TEST_FILES.md"
echo ""

exit 1

