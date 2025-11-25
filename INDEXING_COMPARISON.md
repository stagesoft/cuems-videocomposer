# Indexing Comparison: xjadeo vs cuems-videocomposer

## Overview

Both xjadeo and cuems-videocomposer use frame indexing to enable fast, accurate seeking in video files. However, the implementations differ significantly in complexity and accuracy.

## xjadeo Indexing (3-Pass System)

### Structure
```c
struct FrameIndex {
    int64_t pkt_pts;      // Packet PTS (from container)
    int64_t pkt_pos;      // Packet byte position
    int64_t frame_pts;    // Frame PTS (from decoded frame) - CRITICAL
    int64_t frame_pos;    // Frame byte position (from decoded frame)
    int64_t timestamp;    // Calculated timestamp in stream timebase
    int64_t seekpts;      // Optimal keyframe PTS for seeking to this frame
    int64_t seekpos;      // Optimal keyframe byte position for seeking
    uint8_t key;          // Is this a keyframe?
};
```

### Pass 1: Packet Scanning
- **Purpose**: Read all packets, identify keyframes, collect packet-level metadata
- **Actions**:
  - Scans entire file reading all video packets
  - Records `pkt_pts`, `pkt_pos`, and `key` flag for each packet
  - Calculates `timestamp` using `av_rescale_q(frameCount, frameRate, timeBase)`
  - Tracks `max_keyframe_interval` and `keyframe_byte_distance`
  - **Special case**: If first 500 frames are all keyframes, disables indexing and uses direct seek mode
  - Handles PTS/DTS fallback (switches to DTS if PTS unavailable)
  - Detects problematic files (excessive keyframe intervals)

### Pass 2: Keyframe Verification
- **Purpose**: Validate keyframes by actually decoding them
- **Actions**:
  - For each keyframe found in Pass 1:
    - Seeks to the keyframe's `pkt_pts`
    - Flushes codec buffers
    - Decodes one frame after seeking
    - Records the **actual decoded frame PTS** (`frame_pts`)
    - Records the **actual frame position** (`frame_pos`)
  - Marks invalid keyframes (those that can't be decoded or have no PTS)
  - This is **critical** because packet PTS and frame PTS can differ!

### Pass 3: Seek Table Creation
- **Purpose**: Build optimal seek table for each frame
- **Actions**:
  - For each frame, uses `keyframe_lookup_helper()` to find the best keyframe to seek to
  - `keyframe_lookup_helper()`:
    - Searches backwards from a given position
    - Finds the most recent keyframe with `frame_pts <= target_timestamp`
    - Uses **frame_pts** (from decoded frame), not packet PTS
  - Assigns `seekpts` and `seekpos` from the found keyframe
  - This ensures seeking always lands on a valid, decodable keyframe

### Seeking Logic
```c
// Check if seek is needed
if (last_decoded_pts < 0 || last_decoded_pts > timestamp) {
    need_seek = 1;
} else if (frameNumber == last_decoded_frameno + 1) {
    // Sequential frame - no seek needed
} else if (fidx[frameNumber].seekpts != fidx[last_decoded_frameno].seekpts) {
    need_seek = 1;  // Different keyframe needed
}

// Seek using optimal keyframe
if (byte_seek && fidx[frameNumber].seekpos > 0) {
    av_seek_frame(..., fidx[frameNumber].seekpos, AVSEEK_FLAG_BYTE);
} else {
    av_seek_frame(..., fidx[frameNumber].seekpts, AVSEEK_FLAG_BACKWARD);
}
```

### Key Features
- ✅ **Validates keyframes** by decoding them
- ✅ **Uses frame PTS** (from decoded frame) for accurate seeking
- ✅ **Optimal seek table** - finds best keyframe for each frame
- ✅ **Byte seeking** when possible (faster than timestamp seeking)
- ✅ **Smart optimizations** - skips indexing for all-keyframe files
- ✅ **Handles edge cases** - PTS/DTS fallback, invalid keyframes

---

## cuems-videocomposer Indexing (1-Pass System)

### Structure
```cpp
struct FrameIndex {
    int64_t pkt_pts;      // Packet PTS
    int64_t pkt_pos;      // Packet byte position
    int64_t frame_pts;    // Same as pkt_pts (NOT from decoded frame)
    int64_t frame_pos;    // Same as pkt_pos (NOT from decoded frame)
    int64_t timestamp;   // Calculated timestamp
    int64_t seekpts;      // Same as pkt_pts (NOT optimal keyframe)
    int64_t seekpos;      // Same as pkt_pos (NOT optimal keyframe)
    uint8_t key;          // Is this a keyframe?
};
```

### Single Pass: Packet Scanning
- **Purpose**: Read all packets and record packet-level metadata
- **Actions**:
  - Scans entire file reading all video packets
  - Records `pkt_pts`, `pkt_pos`, and `key` flag
  - Sets `frame_pts = pkt_pts` (assumes they're the same)
  - Sets `frame_pos = pkt_pos` (assumes they're the same)
  - Sets `seekpts = pkt_pts` (uses packet PTS directly)
  - Sets `seekpos = pkt_pos` (uses packet position directly)
  - Calculates `timestamp` using `av_rescale_q(frameCount, frameRate, timeBase)`
  - **No keyframe verification**
  - **No seek table optimization**

### Seeking Logic
```cpp
// Check if seek is needed (similar to xjadeo)
if (lastDecodedPTS_ < 0 || lastDecodedPTS_ > timestamp) {
    needSeek = true;
} else if (frameNumber == lastDecodedFrameNo_ + 1) {
    // Sequential frame - no seek
} else if (idx.seekpts != frameIndex_[lastDecodedFrameNo_].seekpts) {
    needSeek = true;
}

// Seek using packet PTS directly (not optimal keyframe)
if (byteSeek_ && idx.seekpos > 0) {
    mediaReader_.seek(idx.seekpos, ..., AVSEEK_FLAG_BYTE);
} else {
    mediaReader_.seek(idx.seekpts, ..., AVSEEK_FLAG_BACKWARD);
}
```

### Key Features
- ✅ **Simpler** - single pass, faster indexing
- ✅ **Lower memory** - no need to decode frames during indexing
- ❌ **No keyframe validation** - assumes all marked keyframes are valid
- ❌ **Uses packet PTS** - may not match actual frame PTS
- ❌ **No seek optimization** - seeks to frame's own PTS, not optimal keyframe
- ❌ **May seek to non-keyframes** - if packet PTS doesn't align with keyframes

---

## Key Differences

| Feature | xjadeo | cuems-videocomposer |
|---------|--------|---------------------|
| **Passes** | 3 passes | 1 pass |
| **Keyframe Verification** | ✅ Yes (decodes frames) | ❌ No |
| **Frame PTS** | ✅ From decoded frame | ❌ Assumed = packet PTS |
| **Seek Table** | ✅ Optimized (finds best keyframe) | ❌ Direct (uses frame's own PTS) |
| **Byte Seeking** | ✅ Yes (validated) | ✅ Yes (unvalidated) |
| **All-Keyframe Optimization** | ✅ Skips indexing | ❌ No |
| **Indexing Speed** | Slower (decodes frames) | Faster (packet scan only) |
| **Seeking Accuracy** | High (validated keyframes) | Medium (may miss keyframes) |
| **Memory Usage** | Higher (stores frame PTS) | Lower (packet PTS only) |

---

## Implications

### When cuems-videocomposer's approach works well:
- ✅ Files with reliable keyframe markers
- ✅ Files where packet PTS matches frame PTS
- ✅ Sequential playback (minimal seeking)
- ✅ Files with frequent keyframes (every frame or every few frames)

### When xjadeo's approach is better:
- ✅ Files with irregular keyframe patterns
- ✅ Files where packet PTS ≠ frame PTS
- ✅ Random seeking (scrubbing, jumping)
- ✅ Files with sparse keyframes (e.g., long GOP structures)
- ✅ Problematic/corrupted files

### Real-World Impact:
1. **Seeking accuracy**: xjadeo's validated keyframes ensure seeking always lands on a decodable frame. cuems-videocomposer may seek to a non-keyframe and need to decode forward.
2. **Seek performance**: xjadeo's optimized seek table finds the closest keyframe, minimizing decode distance. cuems-videocomposer may seek to a frame far from its keyframe.
3. **Indexing time**: xjadeo's 3-pass system is slower but more accurate. cuems-videocomposer is faster but may require more decoding during seeks.

---

## Recommendations

### Option 1: Keep Current Implementation (Simplified)
- **Pros**: Fast indexing, lower memory, simpler code
- **Cons**: Less accurate seeking, may need more decoding during seeks
- **Best for**: Files with frequent keyframes, sequential playback

### Option 2: Implement xjadeo-Style 3-Pass Indexing
- **Pros**: Accurate seeking, optimal performance, handles edge cases
- **Cons**: Slower indexing, more complex code, higher memory
- **Best for**: Professional use, random seeking, problematic files

### Option 3: Hybrid Approach
- **Pass 1**: Packet scan (current implementation)
- **Pass 2**: Validate keyframes only (not all frames)
- **Pass 3**: Build optimized seek table using validated keyframes
- **Pros**: Balance between speed and accuracy
- **Cons**: More complex than current, less accurate than full xjadeo

### Option 4: Make Indexing Optional/Configurable
- Allow users to choose between:
  - Fast indexing (current, 1-pass)
  - Accurate indexing (xjadeo-style, 3-pass)
  - No indexing (timestamp-based seeking)
- **Pros**: Flexibility for different use cases
- **Cons**: More code to maintain

---

## Code Locations

### xjadeo
- Indexing: `src/xjadeo/xjadeo.c` lines 756-1017
- Seeking: `src/xjadeo/xjadeo.c` lines 537-700
- Keyframe lookup: `src/xjadeo/xjadeo.c` lines 741-753

### cuems-videocomposer
- Indexing: `src/cuems_videocomposer/cpp/input/VideoFileInput.cpp` lines 430-503
- Seeking: `src/cuems_videocomposer/cpp/input/VideoFileInput.cpp` lines 505-609
- HAP indexing: `src/cuems_videocomposer/cpp/input/HAPVideoInput.cpp` lines 274-374

