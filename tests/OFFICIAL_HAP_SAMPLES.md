# Official HAP Test Samples

This document lists the official HAP test samples provided by Vidvox for testing HAP codec implementations.

**Source**: [hap.video](https://hap.video/)

## Available Test Sample Packs

### 1. Odd Dimensions (2 MB)
**URL**: https://d3omao0uy1rjjh.cloudfront.net/hap/Hap_Test_Odd_Dimensions.zip

Samples with irregular/non-standard dimensions. Useful for testing edge cases and dimension handling.

**Use cases**:
- Testing non-power-of-2 dimensions
- Testing odd aspect ratios
- Edge case validation

### 2. 16K (35 MB)
**URL**: https://d3omao0uy1rjjh.cloudfront.net/hap/Hap_Test_16K.zip

Samples at very high resolution (16K). Tests performance and memory handling at extreme resolutions.

**Use cases**:
- High-resolution performance testing
- Memory usage validation
- GPU texture size limits

### 3. FFmpeg (24 MB)
**URL**: https://d3omao0uy1rjjh.cloudfront.net/hap/Hap_Test_FFmpeg.zip

Samples encoded using FFmpeg. Useful for testing compatibility with FFmpeg-encoded HAP files.

**Use cases**:
- FFmpeg encoding compatibility
- Cross-tool compatibility testing
- Standard HAP variant testing

### 4. TouchDesigner (1.5 GB) ⚠️ Large
**URL**: https://d3omao0uy1rjjh.cloudfront.net/hap/Hap_Test_Derivative_TouchDesigner.zip

Samples encoded using Derivative TouchDesigner. Large file size - download only if needed.

**Use cases**:
- TouchDesigner encoding compatibility
- Professional workflow testing
- Complex content testing

### 5. AVF Batch Converter (35 MB)
**URL**: https://d3omao0uy1rjjh.cloudfront.net/hap/Hap_Test_Vidvox_AVF_Batch_Converter.zip

Samples encoded using Vidvox AVF Batch Converter (official Vidvox tool).

**Use cases**:
- Official Vidvox encoder validation
- Reference quality testing
- All HAP variant testing (including HAP R)

### 6. QuickTime Codec (20 MB)
**URL**: https://d3omao0uy1rjjh.cloudfront.net/hap/Hap_Test_Vidvox_QuickTime_Codec.zip

Samples encoded using the Vidvox QuickTime codec component.

**Use cases**:
- QuickTime codec compatibility
- macOS workflow testing
- Professional encoding tool validation

### 7. DirectShow Codec (5 MB)
**URL**: https://d3omao0uy1rjjh.cloudfront.net/hap/Hap_Test_RenderHeads_DirectShow_Codec.zip

Samples encoded using the RenderHeads DirectShow codec (Windows).

**Use cases**:
- Windows DirectShow compatibility
- Windows workflow testing
- Cross-platform validation

## Downloading Samples

Use the provided download script:

```bash
# Download all samples (warning: TouchDesigner is 1.5 GB)
./tests/download_hap_official_samples.sh --all

# Download specific samples
./tests/download_hap_official_samples.sh FFmpeg QuickTime_Codec

# Download small samples for quick testing
./tests/download_hap_official_samples.sh Odd_Dimensions FFmpeg DirectShow_Codec
```

Samples will be downloaded to: `video_test_files/hap_samples/official/`

## Testing with Samples

After downloading, test with the codec format test script:

```bash
# Test FFmpeg samples
python3 tests/test_codec_formats.py --test-dir 'video_test_files/hap_samples/official/FFmpeg'

# Test all downloaded samples
for dir in video_test_files/hap_samples/official/*/; do
    if [ -d "$dir" ]; then
        echo "Testing: $dir"
        python3 tests/test_codec_formats.py --test-dir "$dir" --duration 10
    fi
done
```

## Recommended Testing Order

1. **Start with small samples**:
   - `Odd_Dimensions` (2 MB) - Quick edge case testing
   - `DirectShow_Codec` (5 MB) - Cross-platform validation

2. **Standard testing**:
   - `FFmpeg` (24 MB) - Most common encoding tool
   - `QuickTime_Codec` (20 MB) - macOS standard
   - `AVF_Batch_Converter` (35 MB) - Official Vidvox tool

3. **Advanced testing**:
   - `16K` (35 MB) - High resolution performance
   - `TouchDesigner` (1.5 GB) - Professional workflow (download only if needed)

## Notes

- All samples are provided by Vidvox for testing purposes
- Samples may include various HAP variants (HAP, HAP Q, HAP Alpha, HAP Q Alpha, HAP R)
- Use these samples to validate HAP decoding compatibility across different encoders
- The TouchDesigner pack is very large (1.5 GB) - only download if specifically needed

## HAP Variant Detection

After downloading, you can check which HAP variants are present in each sample pack:

```bash
# Check HAP variants in a sample pack
for file in video_test_files/hap_samples/official/FFmpeg/*.mov; do
    if [ -f "$file" ]; then
        ffprobe -v quiet -select_streams v:0 -show_entries stream=codec_name,codec_tag_string -of json "$file" | grep -E "codec_name|codec_tag_string"
    fi
done
```

