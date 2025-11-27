#!/bin/bash
#
# Download HAP sample files from Vidvox/VDMX
#
# These sample packs may contain HAP, HAP Alpha, HAP Q, and potentially HAP Q Alpha files
#

set -e

DOWNLOAD_DIR="${1:-video_test_files/hap_samples}"
mkdir -p "$DOWNLOAD_DIR"

echo "=========================================="
echo "Downloading HAP Sample Files"
echo "=========================================="
echo ""
echo "Source: Vidvox/VDMX"
echo "Destination: $DOWNLOAD_DIR"
echo ""

# VDMX sample pack URLs
SAMPLE_1080P="http://vidvox.net/download/SamplePackOneHap1080p.zip"
SAMPLE_480P="http://vidvox.net/download/SamplePackOneHap480p.zip"

download_and_extract() {
    local url=$1
    local name=$2
    local zipfile="$DOWNLOAD_DIR/${name}.zip"
    
    echo "Downloading $name..."
    if curl -L -f -o "$zipfile" "$url" 2>/dev/null; then
        echo "  ✓ Downloaded: $(du -h "$zipfile" | cut -f1)"
        
        echo "  Extracting..."
        if unzip -q -o "$zipfile" -d "$DOWNLOAD_DIR/$name" 2>/dev/null; then
            echo "  ✓ Extracted to: $DOWNLOAD_DIR/$name"
            
            # Check for HAP Q Alpha files
            echo "  Checking contents..."
            find "$DOWNLOAD_DIR/$name" -type f \( -name "*.mov" -o -name "*.mp4" \) | while read file; do
                # Try to detect HAP Q Alpha using ffprobe
                if command -v ffprobe >/dev/null 2>&1; then
                    codec=$(ffprobe -v quiet -select_streams v:0 -show_entries stream=codec_name -of default=noprint_wrappers=1:nokey=1 "$file" 2>/dev/null)
                    if [ "$codec" = "hap" ]; then
                        # Check if it's HAQA (would need to check frame structure)
                        echo "    Found HAP file: $(basename "$file")"
                    fi
                else
                    echo "    Found: $(basename "$file")"
                fi
            done
            
            # Clean up zip file
            rm -f "$zipfile"
            return 0
        else
            echo "  ✗ Extraction failed"
            return 1
        fi
    else
        echo "  ✗ Download failed"
        return 1
    fi
}

# Download 1080p samples
if download_and_extract "$SAMPLE_1080P" "hap_samples_1080p"; then
    echo ""
fi

# Download 480p samples
if download_and_extract "$SAMPLE_480P" "hap_samples_480p"; then
    echo ""
fi

echo "=========================================="
echo "Checking for HAP Q Alpha files..."
echo "=========================================="
echo ""

# Check all downloaded files for HAP Q Alpha
find "$DOWNLOAD_DIR" -type f \( -name "*.mov" -o -name "*.mp4" \) | while read file; do
    if command -v ffprobe >/dev/null 2>&1; then
        # Check codec
        codec=$(ffprobe -v quiet -select_streams v:0 -show_entries stream=codec_name -of default=noprint_wrappers=1:nokey=1 "$file" 2>/dev/null)
        
        if [ "$codec" = "hap" ]; then
            # Try to detect if it's HAQA by checking for dual textures
            # This is a heuristic - HAQA files have texture_count=2
            echo "Checking: $(basename "$file")"
            
            # Use FFmpeg to try decoding a frame and check for errors
            # HAQA files should decode successfully with texture_count=2
            # (This is a rough check - actual detection would need to parse HAP frame structure)
            echo "  Codec: HAP (variant unknown from metadata)"
        fi
    fi
done

echo ""
echo "=========================================="
echo "Download Complete"
echo "=========================================="
echo ""
echo "Files downloaded to: $DOWNLOAD_DIR"
echo ""
echo "Note: These sample packs may contain:"
echo "  - HAP (standard)"
echo "  - HAP Alpha"
echo "  - HAP Q"
echo "  - HAP Q Alpha (if available)"
echo ""
echo "To identify HAP Q Alpha files, you can:"
echo "  1. Try playing them and check for alpha channel"
echo "  2. Use ffprobe to examine frame structure"
echo "  3. Test with: ./build/cuems-videocomposer --file <file>"
echo ""

