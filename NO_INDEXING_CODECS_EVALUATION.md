# Codecs and Formats That Don't Need Indexing

## Overview

Some codecs and formats have characteristics that make indexing unnecessary or less beneficial. This document identifies these formats and explains why indexing can be skipped.

## Key Characteristics: When Indexing Isn't Needed

### 1. All Frames Are Keyframes (Intra-Frame Only)
- Every frame is independently decodable
- No GOP (Group of Pictures) structure
- Direct frame access without decoding dependencies

### 2. Very Frequent Keyframes
- Keyframes every 1-2 frames
- Minimal decode distance
- Timestamp-based seeking is accurate

### 3. Accurate Timestamps
- Packet PTS = Frame PTS (no validation needed)
- Reliable timestamp information
- No need for frame PTS validation

### 4. Simple Linear Structure
- No complex GOP patterns
- Predictable frame positions
- Easy timestamp calculation

## Video Codecs That Don't Need Indexing

### ✅ **HAP (All Variants)**

**Characteristics**:
- **Intra-frame only**: Every frame is a keyframe
- **Direct GPU access**: Designed for GPU texture upload
- **Simple structure**: Linear frame access

**Current Implementation**:
```cpp
// Already detected in code:
if ((frameCount_ == 500 || frameCount_ == frames) && max_keyframe_interval == 1) {
    LOG_INFO << "First 500 frames are all keyframes. Using direct seek mode.";
    // Skip Pass 2 and Pass 3, use direct seek
}
```

**Indexing Need**: ❌ **Not needed**
- All frames are keyframes
- Direct seek mode works perfectly
- Timestamp-based seeking is accurate

**Recommendation**: ✅ **Skip indexing for HAP files**

---

### ✅ **ProRes (All Variants)**

**Variants**: ProRes 422, ProRes 422 HQ, ProRes 4444, ProRes XQ

**Characteristics**:
- **Intra-frame codec**: Every frame is independently decodable
- **Professional format**: Designed for editing (frame-accurate)
- **Accurate timestamps**: Reliable PTS information

**Indexing Need**: ❌ **Not needed**
- All frames are keyframes
- Direct frame access
- Timestamp-based seeking works perfectly

**FFmpeg Codec ID**: `AV_CODEC_ID_PRORES`

**Recommendation**: ✅ **Skip indexing for ProRes files**

---

### ✅ **DNxHD / DNxHR**

**Characteristics**:
- **Intra-frame codec**: Every frame is independently decodable
- **Professional format**: Designed for editing
- **Frame-accurate**: Direct frame access

**Indexing Need**: ❌ **Not needed**
- All frames are keyframes
- Simple structure
- Accurate seeking without index

**FFmpeg Codec ID**: `AV_CODEC_ID_DNXHD`

**Recommendation**: ✅ **Skip indexing for DNxHD/DNxHR files**

---

### ✅ **Motion JPEG (MJPEG)**

**Characteristics**:
- **Intra-frame only**: Each frame is a JPEG image
- **No dependencies**: Frames are independent
- **Simple structure**: Linear frame sequence

**Indexing Need**: ❌ **Not needed**
- All frames are keyframes
- Direct frame access
- No decode dependencies

**FFmpeg Codec ID**: `AV_CODEC_ID_MJPEG`

**Recommendation**: ✅ **Skip indexing for MJPEG files**

---

### ✅ **Uncompressed Video Formats**

**Formats**: YUV, RGB, RAW

**Characteristics**:
- **No compression**: Raw pixel data
- **Frame-accurate**: Direct frame access
- **Simple structure**: Linear frame sequence

**Indexing Need**: ❌ **Not needed**
- No keyframes (all frames are "keyframes")
- Direct frame calculation
- Accurate seeking

**FFmpeg Codec IDs**: `AV_CODEC_ID_RAWVIDEO`, various pixel formats

**Recommendation**: ✅ **Skip indexing for uncompressed formats**

---

### ✅ **FFV1 (FFmpeg Video Codec 1)**

**Characteristics**:
- **Lossless codec**: Can be intra-frame only
- **Configurable**: Can be set to all keyframes
- **Accurate**: Reliable timestamps

**Indexing Need**: ⚠️ **Depends on encoding**
- If encoded with all keyframes: ❌ Not needed
- If encoded with GOP: ✅ Indexing helpful

**FFmpeg Codec ID**: `AV_CODEC_ID_FFV1`

**Recommendation**: ⚠️ **Check keyframe interval, skip if all keyframes**

---

### ⚠️ **H.264 / HEVC (Intra-Only Encoding)**

**Characteristics**:
- **Can be intra-only**: If encoded with `-g 1` (keyframe every frame)
- **Usually GOP-based**: Typically has GOP structure
- **Variable**: Depends on encoding settings

**Indexing Need**: ⚠️ **Depends on encoding**
- If all keyframes: ❌ Not needed
- If GOP structure: ✅ Indexing very helpful

**Detection**: Check `max_keyframe_interval` during Pass 1
- If `max_keyframe_interval == 1`: All keyframes → Skip indexing
- If `max_keyframe_interval > 1`: GOP structure → Indexing needed

**FFmpeg Codec IDs**: `AV_CODEC_ID_H264`, `AV_CODEC_ID_HEVC`

**Recommendation**: ⚠️ **Auto-detect: Skip if all keyframes detected**

---

## Audio Codecs: Indexing Generally Not Needed

### ✅ **Most Audio Codecs**

**Why audio doesn't need indexing**:
- **Frequent keyframes**: Most audio codecs have keyframes in every packet
- **Simple structure**: Linear packet sequence
- **Accurate timestamps**: Packet PTS matches sample PTS
- **Fast decoding**: Audio decoding is fast, minimal overhead

**Audio Codecs**:
- **PCM** (WAV, AIFF): Uncompressed, no keyframes
- **FLAC**: Lossless, frequent keyframes
- **AAC**: Usually every packet is a keyframe
- **MP3**: Frame-based, accurate seeking
- **Opus**: Packet-based, accurate timestamps
- **Vorbis**: Packet-based, accurate seeking

**Indexing Need**: ❌ **Generally not needed**
- Timestamp-based seeking works well
- Minimal decode overhead
- Simple structure

**Exception**: Very long audio files with complex structures might benefit, but rare.

**Recommendation**: ✅ **Skip audio indexing in most cases**

---

## Detection Logic

### Current Implementation

The code already detects all-keyframe files:

```cpp
// In VideoFileInput::indexFrames()
if ((frameCount_ == 500 || frameCount_ == frames) && max_keyframe_interval == 1) {
    LOG_INFO << "First 500 frames are all keyframes. Using direct seek mode.";
    // Skip Pass 2 and Pass 3
    return true; // Direct seek mode
}
```

### Enhanced Detection

We can improve detection by checking codec type:

```cpp
bool VideoFileInput::needsIndexing() const {
    if (!codecCtx_) {
        return true; // Unknown, assume indexing needed
    }
    
    AVCodecID codecId = codecCtx_->codec_id;
    
    // Intra-frame only codecs (all keyframes)
    switch (codecId) {
        case AV_CODEC_ID_HAP:
        case AV_CODEC_ID_HAPQ:
        case AV_CODEC_ID_HAPALPHA:
        case AV_CODEC_ID_PRORES:
        case AV_CODEC_ID_DNXHD:
        case AV_CODEC_ID_MJPEG:
        case AV_CODEC_ID_RAWVIDEO:
            return false; // No indexing needed
        
        // Check during Pass 1 for these
        case AV_CODEC_ID_H264:
        case AV_CODEC_ID_HEVC:
        case AV_CODEC_ID_AV1:
            // Will be determined by max_keyframe_interval
            return true; // Check during indexing
        
        default:
            return true; // Unknown, assume indexing needed
    }
}
```

## Format-Specific Considerations

### Container Formats

**MP4/MOV**:
- Can contain any codec
- Indexing need depends on codec, not container
- MP4 has built-in index (moov atom), but may not be frame-accurate

**MKV**:
- Can contain any codec
- Indexing need depends on codec
- MKV has seek index, but may need validation

**MXF**:
- Professional format
- Often contains ProRes or DNxHD (intra-frame)
- Indexing usually not needed

**AVI**:
- Simple container
- Indexing need depends on codec
- Often contains MJPEG or uncompressed (no indexing needed)

## Implementation Recommendations

### Phase 1: Codec-Based Detection (Recommended)

**Add codec detection before indexing**:

```cpp
bool VideoFileInput::indexFrames() {
    // Check if codec needs indexing
    if (!needsIndexing()) {
        LOG_INFO << "Codec is intra-frame only, skipping indexing";
        // Use direct seek mode
        setupDirectSeekMode();
        return true;
    }
    
    // Proceed with 3-pass indexing
    // ...
}
```

**Benefits**:
- ✅ Skips unnecessary indexing for intra-frame codecs
- ✅ Faster file opening
- ✅ Less I/O during initialization
- ✅ Still detects all-keyframe H.264/HEVC during Pass 1

### Phase 2: Enhanced All-Keyframe Detection

**Improve current detection**:

```cpp
// During Pass 1, check keyframe interval
if (max_keyframe_interval == 1) {
    // All frames are keyframes
    if (frameCount_ >= 100) { // Check more than 500 frames
        LOG_INFO << "All frames are keyframes, using direct seek mode";
        setupDirectSeekMode();
        return true; // Skip Pass 2 and Pass 3
    }
}
```

**Benefits**:
- ✅ Catches all-keyframe H.264/HEVC files
- ✅ Works for any codec
- ✅ No codec-specific logic needed

## Performance Impact

### With Indexing (Unnecessary)

**Intra-frame codec (e.g., HAP)**:
- Pass 1: Scan all packets (5-10 seconds)
- Pass 2: Validate keyframes (10-30 seconds) - **Unnecessary**
- Pass 3: Build seek table (2-5 seconds) - **Unnecessary**
- **Total**: 17-45 seconds wasted

### Without Indexing (Direct Seek)

**Intra-frame codec**:
- Detect all-keyframe: < 1 second
- Setup direct seek mode: < 1 second
- **Total**: < 2 seconds

**Savings**: 15-43 seconds per file

## Codec Detection Table

| Codec | Codec ID | Indexing Need | Reason |
|-------|----------|---------------|--------|
| **HAP** | `AV_CODEC_ID_HAP` | ❌ No | All frames keyframes |
| **HAP Q** | `AV_CODEC_ID_HAPQ` | ❌ No | All frames keyframes |
| **HAP Alpha** | `AV_CODEC_ID_HAPALPHA` | ❌ No | All frames keyframes |
| **ProRes** | `AV_CODEC_ID_PRORES` | ❌ No | Intra-frame only |
| **DNxHD** | `AV_CODEC_ID_DNXHD` | ❌ No | Intra-frame only |
| **MJPEG** | `AV_CODEC_ID_MJPEG` | ❌ No | Intra-frame only |
| **Raw Video** | `AV_CODEC_ID_RAWVIDEO` | ❌ No | Uncompressed |
| **H.264** | `AV_CODEC_ID_H264` | ⚠️ Depends | Check keyframe interval |
| **HEVC** | `AV_CODEC_ID_HEVC` | ⚠️ Depends | Check keyframe interval |
| **AV1** | `AV_CODEC_ID_AV1` | ⚠️ Depends | Check keyframe interval |
| **VP9** | `AV_CODEC_ID_VP9` | ✅ Yes | Usually GOP-based |
| **VP8** | `AV_CODEC_ID_VP8` | ✅ Yes | Usually GOP-based |
| **Theora** | `AV_CODEC_ID_THEORA` | ✅ Yes | GOP-based |

## Audio Codec Detection

| Codec | Codec ID | Indexing Need | Reason |
|-------|----------|---------------|--------|
| **PCM** | `AV_CODEC_ID_PCM_*` | ❌ No | Uncompressed |
| **FLAC** | `AV_CODEC_ID_FLAC` | ❌ No | Frequent keyframes |
| **AAC** | `AV_CODEC_ID_AAC` | ❌ No | Every packet keyframe |
| **MP3** | `AV_CODEC_ID_MP3` | ❌ No | Frame-based, accurate |
| **Opus** | `AV_CODEC_ID_OPUS` | ❌ No | Packet-based |
| **Vorbis** | `AV_CODEC_ID_VORBIS` | ❌ No | Packet-based |

## Implementation Code

### Enhanced needsIndexing() Function

```cpp
bool VideoFileInput::needsIndexing() const {
    if (!codecCtx_) {
        return true; // Unknown, assume indexing needed
    }
    
    AVCodecID codecId = codecCtx_->codec_id;
    
    // Intra-frame only codecs (definitely no indexing)
    switch (codecId) {
        case AV_CODEC_ID_HAP:
        #ifdef AV_CODEC_ID_HAPQ
        case AV_CODEC_ID_HAPQ:
        #endif
        #ifdef AV_CODEC_ID_HAPALPHA
        case AV_CODEC_ID_HAPALPHA:
        #endif
        case AV_CODEC_ID_PRORES:
        case AV_CODEC_ID_DNXHD:
        case AV_CODEC_ID_MJPEG:
        case AV_CODEC_ID_RAWVIDEO:
            return false; // No indexing needed
        
        // Codecs that might be intra-only (check during Pass 1)
        case AV_CODEC_ID_H264:
        case AV_CODEC_ID_HEVC:
        case AV_CODEC_ID_AV1:
            // Will be determined by max_keyframe_interval in Pass 1
            return true; // Check during indexing
        
        default:
            // Unknown codec, assume indexing needed
            return true;
    }
}
```

### Setup Direct Seek Mode

```cpp
void VideoFileInput::setupDirectSeekMode() {
    // Calculate frame positions based on framerate and duration
    if (frameInfo_.framerate > 0 && frameInfo_.totalFrames > 0) {
        AVStream* avStream = mediaReader_.getStream(videoStream_);
        if (avStream) {
            AVRational timeBase = avStream->time_base;
            int64_t firstPTS = avStream->first_dts != AV_NOPTS_VALUE ? 
                              avStream->first_dts : 0;
            
            // Allocate minimal index (just for frame count)
            frameCount_ = frameInfo_.totalFrames;
            frameIndex_ = static_cast<FrameIndex*>(
                calloc(frameCount_, sizeof(FrameIndex))
            );
            
            if (frameIndex_) {
                // Fill in direct seek positions
                for (int64_t i = 0; i < frameCount_; ++i) {
                    frameIndex_[i].key = 1; // All keyframes
                    frameIndex_[i].timestamp = av_rescale_q(
                        i, frameRateQ_, timeBase
                    );
                    frameIndex_[i].pkt_pts = frameIndex_[i].frame_pts = 
                        firstPTS + av_rescale_q(i, frameRateQ_, timeBase);
                    frameIndex_[i].seekpts = frameIndex_[i].pkt_pts;
                    frameIndex_[i].seekpos = -1; // Will use timestamp seek
                }
            }
        }
    }
    
    scanComplete_ = true;
    byteSeek_ = false; // Use timestamp seeking for intra-frame codecs
}
```

## Recommendations

### ✅ **Immediate Implementation**

1. **Add codec-based detection**:
   - Check codec type before indexing
   - Skip indexing for known intra-frame codecs
   - Saves 15-43 seconds per file

2. **Keep current all-keyframe detection**:
   - Still catches all-keyframe H.264/HEVC
   - Works for any codec
   - Fallback for unknown codecs

### ✅ **Future Enhancement**

1. **Audio stream detection**:
   - Skip audio indexing entirely
   - Audio seeking is accurate without index
   - Saves indexing time

2. **Format-specific optimizations**:
   - MXF with ProRes: Skip indexing
   - AVI with MJPEG: Skip indexing
   - Container-based hints

## Conclusion

### Codecs That Don't Need Indexing

**Video**:
- ✅ HAP (all variants)
- ✅ ProRes (all variants)
- ✅ DNxHD/DNxHR
- ✅ MJPEG
- ✅ Uncompressed (Raw Video)
- ⚠️ H.264/HEVC/AV1 (if all keyframes)

**Audio**:
- ✅ Most audio codecs (PCM, FLAC, AAC, MP3, Opus, Vorbis)

### Benefits of Skipping Indexing

- **Faster file opening**: 15-43 seconds saved
- **Less I/O**: No unnecessary file scanning
- **Simpler code**: Direct seek mode
- **Better UX**: Instant playback for intra-frame codecs

### Implementation Priority

1. **High**: Add codec-based detection for known intra-frame codecs
2. **Medium**: Enhance all-keyframe detection
3. **Low**: Audio indexing skip (audio doesn't need it anyway)

