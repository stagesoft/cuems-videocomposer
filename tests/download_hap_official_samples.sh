#!/bin/bash
# Download official HAP test samples from Vidvox
# Source: https://hap.video/

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DOWNLOAD_DIR="$PROJECT_ROOT/video_test_files/hap_samples/official"

# Create download directory
mkdir -p "$DOWNLOAD_DIR"

# HAP test sample URLs
declare -A HAP_SAMPLES=(
    ["Odd_Dimensions"]="https://d3omao0uy1rjjh.cloudfront.net/hap/Hap_Test_Odd_Dimensions.zip"
    ["16K"]="https://d3omao0uy1rjjh.cloudfront.net/hap/Hap_Test_16K.zip"
    ["FFmpeg"]="https://d3omao0uy1rjjh.cloudfront.net/hap/Hap_Test_FFmpeg.zip"
    ["TouchDesigner"]="https://d3omao0uy1rjjh.cloudfront.net/hap/Hap_Test_Derivative_TouchDesigner.zip"
    ["AVF_Batch_Converter"]="https://d3omao0uy1rjjh.cloudfront.net/hap/Hap_Test_Vidvox_AVF_Batch_Converter.zip"
    ["QuickTime_Codec"]="https://d3omao0uy1rjjh.cloudfront.net/hap/Hap_Test_Vidvox_QuickTime_Codec.zip"
    ["DirectShow_Codec"]="https://d3omao0uy1rjjh.cloudfront.net/hap/Hap_Test_RenderHeads_DirectShow_Codec.zip"
)

# File sizes (approximate, for progress indication)
declare -A SAMPLE_SIZES=(
    ["Odd_Dimensions"]="2 MB"
    ["16K"]="35 MB"
    ["FFmpeg"]="24 MB"
    ["TouchDesigner"]="1.5 GB"
    ["AVF_Batch_Converter"]="35 MB"
    ["QuickTime_Codec"]="20 MB"
    ["DirectShow_Codec"]="5 MB"
)

echo "=========================================="
echo "HAP Official Test Samples Downloader"
echo "=========================================="
echo ""
echo "Download directory: $DOWNLOAD_DIR"
echo ""

# Function to download and extract a sample
download_sample() {
    local name=$1
    local url=$2
    local size=${SAMPLE_SIZES[$name]}
    local zip_file="$DOWNLOAD_DIR/${name}.zip"
    local extract_dir="$DOWNLOAD_DIR/$name"
    
    echo "----------------------------------------"
    echo "Downloading: $name (~$size)"
    echo "URL: $url"
    echo "----------------------------------------"
    
    # Check if already extracted
    if [ -d "$extract_dir" ] && [ "$(ls -A $extract_dir 2>/dev/null)" ]; then
        echo "✓ Already extracted: $extract_dir"
        echo "  (Skipping download)"
        return 0
    fi
    
    # Download
    if [ ! -f "$zip_file" ]; then
        echo "Downloading..."
        if command -v wget &> /dev/null; then
            wget -O "$zip_file" "$url" || {
                echo "✗ Download failed with wget"
                return 1
            }
        elif command -v curl &> /dev/null; then
            curl -L -o "$zip_file" "$url" || {
                echo "✗ Download failed with curl"
                return 1
            }
        else
            echo "✗ Error: Neither wget nor curl found. Please install one."
            return 1
        fi
        echo "✓ Download complete"
    else
        echo "✓ Zip file already exists: $zip_file"
    fi
    
    # Extract
    if [ -f "$zip_file" ]; then
        echo "Extracting..."
        mkdir -p "$extract_dir"
        if command -v unzip &> /dev/null; then
            unzip -q -o "$zip_file" -d "$extract_dir" || {
                echo "✗ Extraction failed"
                return 1
            }
            echo "✓ Extraction complete"
            
            # Count files
            file_count=$(find "$extract_dir" -type f | wc -l)
            echo "  Files extracted: $file_count"
        else
            echo "✗ Error: unzip not found. Please install unzip."
            return 1
        fi
    fi
    
    echo ""
}

# Parse command line arguments
DOWNLOAD_ALL=false
SELECTED_SAMPLES=()

if [ $# -eq 0 ]; then
    echo "Available samples:"
    for name in "${!HAP_SAMPLES[@]}"; do
        echo "  - $name (${SAMPLE_SIZES[$name]})"
    done
    echo ""
    echo "Usage:"
    echo "  $0 --all                    # Download all samples"
    echo "  $0 <sample1> [sample2] ...  # Download specific samples"
    echo ""
    echo "Examples:"
    echo "  $0 --all"
    echo "  $0 FFmpeg QuickTime_Codec"
    echo "  $0 Odd_Dimensions 16K"
    exit 0
fi

# Parse arguments
for arg in "$@"; do
    if [ "$arg" = "--all" ]; then
        DOWNLOAD_ALL=true
    else
        SELECTED_SAMPLES+=("$arg")
    fi
done

# Download samples
if [ "$DOWNLOAD_ALL" = true ]; then
    echo "Downloading ALL samples (this may take a while, especially TouchDesigner at 1.5 GB)..."
    echo ""
    for name in "${!HAP_SAMPLES[@]}"; do
        download_sample "$name" "${HAP_SAMPLES[$name]}"
    done
else
    # Download selected samples
    for sample in "${SELECTED_SAMPLES[@]}"; do
        if [ -z "${HAP_SAMPLES[$sample]}" ]; then
            echo "✗ Error: Unknown sample '$sample'"
            echo "  Available samples: ${!HAP_SAMPLES[*]}"
            exit 1
        fi
        download_sample "$sample" "${HAP_SAMPLES[$sample]}"
    done
fi

echo "=========================================="
echo "Download Complete!"
echo "=========================================="
echo ""
echo "Samples are available in: $DOWNLOAD_DIR"
echo ""
echo "To test with these samples:"
echo "  python3 tests/test_codec_formats.py --test-dir '$DOWNLOAD_DIR/<sample_name>'"
echo ""
echo "Example:"
echo "  python3 tests/test_codec_formats.py --test-dir '$DOWNLOAD_DIR/FFmpeg'"

