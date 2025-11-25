# Background Indexing Evaluation: xjadeo-Style with Low-Priority Toggle

## Concept

Implement xjadeo's 3-pass indexing system as a **background task** that runs after file opening:
1. **Immediate playback**: Use fast 1-pass indexing (current approach) for instant playback
2. **Background indexing**: Start xjadeo-style 3-pass indexing in a low-priority thread
3. **Seamless transition**: When background indexing completes, switch to accurate seeking mode

## Architecture Overview

```
File Open
  ↓
[Pass 1: Fast Packet Scan] ──→ Start Playback (immediate)
  ↓
[Background Thread Starts]
  ↓
[Pass 2: Keyframe Verification] (low priority)
  ↓
[Pass 3: Seek Table Creation] (low priority)
  ↓
[Switch to Accurate Seeking Mode]
```

## Implementation Design

### Phase 1: Fast Indexing (Immediate)
- **Location**: `VideoFileInput::open()` or `VideoFileInput::indexFrames()`
- **Action**: Run current 1-pass packet scan
- **Result**: Basic index ready, playback can start immediately
- **Time**: ~1-5 seconds for typical file

### Phase 2: Background Indexing (Low Priority)
- **Location**: New method `VideoFileInput::startBackgroundIndexing()`
- **Thread**: `std::thread` with low priority (if supported)
- **Actions**:
  1. Pass 2: Validate keyframes by decoding
  2. Pass 3: Build optimized seek table
- **Time**: ~10-60 seconds depending on file size and keyframe count

### State Management
```cpp
enum class IndexingState {
    NOT_STARTED,      // No indexing done
    FAST_COMPLETE,    // Pass 1 done, playback ready
    BACKGROUND_RUNNING, // Pass 2/3 in progress
    FULL_COMPLETE,    // All passes done, accurate seeking available
    FAILED            // Background indexing failed
};
```

## Pros

### ✅ User Experience
1. **Instant Playback**: No waiting for full indexing before playback starts
   - Current: 10-60 second delay before playback
   - With background: <5 second delay, playback starts immediately
   
2. **Progressive Enhancement**: Seeking accuracy improves over time
   - Initially: Basic seeking (may need extra decoding)
   - After completion: Accurate seeking (optimal keyframes)

3. **Non-Blocking**: User can interact with application while indexing continues
   - Can seek, play, pause during background indexing
   - Application remains responsive

### ✅ Performance
1. **CPU Utilization**: Background thread uses idle CPU cycles
   - Low-priority thread won't interfere with playback
   - Can pause/resume based on system load

2. **Memory Efficiency**: 
   - Can reuse same index array (grow as needed)
   - No duplicate memory allocation

3. **Smart Resource Management**:
   - Can pause background indexing during active playback/seeking
   - Resume when system is idle
   - Cancel if file is closed

### ✅ Flexibility
1. **Configurable**: Toggle option to enable/disable background indexing
   - `--enable-background-indexing` (default: enabled)
   - `--disable-background-indexing` (use fast indexing only)

2. **Graceful Degradation**: If background indexing fails, fall back to fast indexing
   - No impact on playback functionality
   - User still gets basic seeking

3. **Multi-File Support**: Each `VideoFileInput` instance can have its own background thread
   - Multiple files can index simultaneously
   - Thread pool could be used for many files

## Cons

### ❌ Complexity
1. **Threading Complexity**: 
   - Need thread-safe access to `frameIndex_` array
   - Must handle concurrent reads (playback) and writes (indexing)
   - Race conditions if not carefully designed

2. **State Synchronization**:
   - Need atomic/mutex-protected state variables
   - Seeking logic must check indexing state
   - Transition from fast to accurate seeking must be atomic

3. **Error Handling**:
   - Background thread errors must be handled gracefully
   - Need cancellation mechanism if file is closed
   - Resource cleanup on thread exit

### ❌ Resource Usage
1. **Thread Overhead**: 
   - Each file = 1 background thread
   - 10 files = 10 threads (manageable but not ideal)
   - Could use thread pool instead

2. **CPU Usage During Playback**:
   - Background decoding competes with playback decoding
   - Even low-priority thread uses CPU cycles
   - May impact playback performance on low-end systems

3. **Memory During Transition**:
   - During Pass 2, need to decode frames (temporary memory)
   - May cause memory spikes during keyframe verification
   - Need to manage memory carefully

### ❌ Implementation Challenges
1. **FFmpeg Context Sharing**:
   - Background thread needs access to `AVFormatContext`, `AVCodecContext`
   - FFmpeg contexts are **NOT thread-safe**
   - Need separate codec context for background indexing OR
   - Need mutex protection (slower but safer)

2. **Seeking During Indexing**:
   - If user seeks while background indexing is running:
     - Background thread may be seeking to different position
     - Need to pause/cancel background indexing during seeks
     - Resume after seek completes

3. **File Position Management**:
   - Background indexing reads entire file
   - Playback also reads file
   - Need to coordinate file access (mutex on `av_read_frame`)

## Technical Challenges & Solutions

### Challenge 1: Thread-Safe FFmpeg Access
**Problem**: FFmpeg contexts (`AVFormatContext`, `AVCodecContext`) are not thread-safe.

**Solution Options**:

#### Option A: Separate Codec Context (Recommended)
```cpp
// Main thread: playback codec context
AVCodecContext* playbackCodecCtx_;

// Background thread: indexing codec context
AVCodecContext* indexingCodecCtx_;
```
- **Pros**: No locking needed, true parallelism
- **Cons**: More memory, need to open codec twice

#### Option B: Mutex Protection
```cpp
std::mutex ffmpegMutex_;

// In background thread:
{
    std::lock_guard<std::mutex> lock(ffmpegMutex_);
    av_seek_frame(...);
    av_read_frame(...);
    avcodec_decode_video2(...);
}
```
- **Pros**: Single codec context, less memory
- **Cons**: Serializes access, may block playback

#### Option C: Copy Format Context
```cpp
// Clone format context for background thread
AVFormatContext* indexingFormatCtx_ = avformat_alloc_context();
avformat_copy_context(indexingFormatCtx_, formatCtx_);
```
- **Pros**: Independent file access
- **Cons**: More complex, may not work for all formats

**Recommendation**: **Option A** (separate codec context) - safest and most performant.

### Challenge 2: Index Array Thread Safety
**Problem**: Background thread writes to `frameIndex_` while playback reads it.

**Solution**:
```cpp
// Use atomic state and careful design
std::atomic<IndexingState> indexingState_;
std::mutex indexMutex_;  // Only for writes

// Reading (playback thread):
if (indexingState_ == IndexingState::FAST_COMPLETE) {
    // Read fast index (no lock needed, only reading)
    const FrameIndex& idx = frameIndex_[frameNumber];
}

// Writing (background thread):
{
    std::lock_guard<std::mutex> lock(indexMutex_);
    frameIndex_[i].frame_pts = decodedPTS;  // Update
}
```

### Challenge 3: Seeking During Indexing
**Problem**: User seeks while background indexing is running.

**Solution**:
```cpp
std::atomic<bool> indexingPaused_;

// In background thread:
while (indexingState_ == IndexingState::BACKGROUND_RUNNING) {
    if (indexingPaused_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
    }
    // ... do indexing work ...
}

// In seek():
void VideoFileInput::seek(int64_t frameNumber) {
    // Pause background indexing during seek
    indexingPaused_ = true;
    
    // Do seek...
    
    // Resume background indexing
    indexingPaused_ = false;
}
```

### Challenge 4: Low-Priority Thread
**Problem**: Ensure background indexing doesn't impact playback performance.

**Solution**:
```cpp
// Linux: set thread priority
#include <pthread.h>
#include <sched.h>

void setLowPriority() {
    struct sched_param param;
    param.sched_priority = sched_get_priority_min(SCHED_OTHER);
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
    
    // Also set nice value
    nice(19);  // Lowest priority
}
```

**Note**: Thread priority may not be available on all platforms. Consider:
- Yield periodically: `std::this_thread::yield()`
- Sleep between keyframes: `std::this_thread::sleep_for(std::chrono::milliseconds(10))`
- Check system load before intensive work

## Implementation Plan

### Step 1: Add State Management
```cpp
// In VideoFileInput.h
enum class IndexingState {
    NOT_STARTED,
    FAST_COMPLETE,
    BACKGROUND_RUNNING,
    FULL_COMPLETE,
    FAILED
};

std::atomic<IndexingState> indexingState_;
std::atomic<bool> indexingPaused_;
std::thread backgroundIndexingThread_;
std::mutex indexMutex_;
```

### Step 2: Split Indexing Methods
```cpp
// Fast indexing (current implementation)
bool indexFramesFast();  // Pass 1 only

// Background indexing (xjadeo-style)
void startBackgroundIndexing();
void indexFramesBackground();  // Pass 2 + 3
void indexKeyframesPass2();   // Validate keyframes
void indexSeekTablePass3();   // Build seek table
```

### Step 3: Modify open()
```cpp
bool VideoFileInput::open(const std::string& source) {
    // ... existing code ...
    
    // Fast indexing (blocking)
    if (!noIndex_) {
        if (!indexFramesFast()) {
            LOG_WARNING << "Fast indexing failed, using timestamp seeking";
        } else {
            indexingState_ = IndexingState::FAST_COMPLETE;
            scanComplete_ = true;  // Allow playback
        }
    }
    
    // Start background indexing (non-blocking)
    if (!noIndex_ && config_->getBool("background_indexing", true)) {
        startBackgroundIndexing();
    }
    
    ready_ = true;
    return true;
}
```

### Step 4: Update Seeking Logic
```cpp
bool VideoFileInput::seek(int64_t frameNumber) {
    // Pause background indexing during seek
    bool wasPaused = indexingPaused_.exchange(true);
    
    // Use appropriate seeking method based on state
    IndexingState state = indexingState_.load();
    bool result = false;
    
    if (state == IndexingState::FULL_COMPLETE) {
        // Use accurate seeking (xjadeo-style)
        result = seekToFrameAccurate(frameNumber);
    } else if (state == IndexingState::FAST_COMPLETE) {
        // Use fast seeking (current implementation)
        result = seekToFrameFast(frameNumber);
    } else {
        // Use timestamp-based seeking
        result = seekByTimestamp(frameNumber);
    }
    
    // Resume background indexing
    if (!wasPaused) {
        indexingPaused_ = false;
    }
    
    return result;
}
```

### Step 5: Cleanup
```cpp
void VideoFileInput::close() {
    // Stop background indexing
    if (backgroundIndexingThread_.joinable()) {
        indexingState_ = IndexingState::NOT_STARTED;
        indexingPaused_ = false;
        backgroundIndexingThread_.join();
    }
    
    // ... existing cleanup ...
}
```

## Performance Estimates

### Indexing Times (1080p, 30fps, 10-minute file)

| Phase | Current (Blocking) | With Background |
|-------|-------------------|-----------------|
| **Pass 1** | 2-5 seconds | 2-5 seconds (blocking) |
| **Playback Start** | After Pass 1 | After Pass 1 (same) |
| **Pass 2** | 10-30 seconds | 15-45 seconds (background, low priority) |
| **Pass 3** | 2-5 seconds | 3-7 seconds (background) |
| **Total Time** | 14-40 seconds | 2-5 seconds (user-visible) |

### CPU Impact

| Scenario | CPU Usage |
|----------|-----------|
| Playback only | 20-30% (single core) |
| Playback + Background Indexing | 30-50% (single core) |
| Background Indexing (paused during seek) | 0% (paused) |

### Memory Impact

| Phase | Memory Usage |
|-------|--------------|
| Fast Index (Pass 1) | ~10-50 MB (index array) |
| Background Index (Pass 2) | +50-200 MB (temporary decode buffers) |
| Full Index Complete | ~10-50 MB (index array only) |

## Configuration Options

### Command Line
```bash
# Enable background indexing (default)
cuems-videocomposer --enable-background-indexing video.mp4

# Disable background indexing (fast only)
cuems-videocomposer --disable-background-indexing video.mp4

# No indexing at all
cuems-videocomposer --no-index video.mp4
```

### Runtime API
```cpp
// Check indexing status
IndexingState state = videoInput->getIndexingState();
if (state == IndexingState::FULL_COMPLETE) {
    // Accurate seeking available
}

// Get indexing progress (0.0 to 1.0)
double progress = videoInput->getIndexingProgress();
```

## Testing Considerations

1. **Thread Safety**: Test concurrent access to index array
2. **Cancellation**: Test file closing during background indexing
3. **Seeking**: Test seeking during background indexing
4. **Multiple Files**: Test multiple files indexing simultaneously
5. **Error Handling**: Test background indexing failure scenarios
6. **Performance**: Measure CPU/memory impact during playback

## Alternative: Hybrid Approach (Simpler)

Instead of full 3-pass background indexing, consider:

### Option: Background Keyframe Validation Only
- **Pass 1**: Fast packet scan (blocking, immediate)
- **Pass 2**: Background keyframe validation (low priority)
- **Pass 3**: Skip (use validated keyframes directly)

**Pros**: Simpler, less background work, still improves accuracy
**Cons**: No optimized seek table (but keyframes are validated)

## Recommendation

### ✅ **Implement Background Indexing with Toggle**

**Rationale**:
1. **User Experience**: Instant playback is critical for professional use
2. **Progressive Enhancement**: Accuracy improves over time without blocking
3. **Flexibility**: Toggle allows users to choose based on their needs
4. **Threading Support**: Codebase already uses `std::thread` (ALSASeqMIDIDriver)

### Implementation Priority:
1. **Phase 1** (High Priority): Fast indexing + basic background thread structure
   - Split `indexFrames()` into fast and background methods
   - Add state management
   - Start background thread (even if just placeholder)

2. **Phase 2** (Medium Priority): Pass 2 (keyframe validation)
   - Implement keyframe validation in background
   - Handle thread safety
   - Test with real files

3. **Phase 3** (Low Priority): Pass 3 (seek table optimization)
   - Implement seek table creation
   - Optimize keyframe lookup
   - Fine-tune performance

### Critical Requirements:
- ✅ Thread-safe index access
- ✅ Separate codec context for background thread
- ✅ Pause/resume during seeks
- ✅ Graceful cancellation on file close
- ✅ Configurable toggle option
- ✅ Progress reporting (optional but useful)

## Conclusion

Background indexing with a toggle option is **highly recommended**:
- ✅ Solves the "waiting for indexing" problem
- ✅ Provides progressive accuracy improvement
- ✅ Maintains application responsiveness
- ✅ Allows user control via toggle
- ⚠️ Requires careful threading implementation
- ⚠️ Adds complexity but manageable

The benefits significantly outweigh the costs, especially for professional video playback where instant response is critical.

