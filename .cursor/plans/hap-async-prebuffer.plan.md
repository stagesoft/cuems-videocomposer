# HAP Async I/O Pre-buffering

## Goal

Support 4-6 simultaneous HAP layers at 60fps without frame drops.

## Problem Analysis

### Current HAP Pipeline (Single-threaded)

```
Frame N Timeline (must complete in 16.67ms):
├── Wait for MTC sync                    ~0.1ms
├── Layer 1: fread() → DXT upload        ~1-2ms  ← BLOCKING I/O
├── Layer 2: fread() → DXT upload        ~1-2ms  ← BLOCKING I/O
├── Layer 3: fread() → DXT upload        ~1-2ms  ← BLOCKING I/O
├── Layer 4: fread() → DXT upload        ~1-2ms  ← BLOCKING I/O
├── Layer 5: fread() → DXT upload        ~1-2ms  ← BLOCKING I/O
├── Layer 6: fread() → DXT upload        ~1-2ms  ← BLOCKING I/O
├── Render composited frame              ~3-5ms
└── Total                                ~9-17ms ⚠️ Risk of frame drops!
```

### Bottleneck: Sequential Disk I/O

| Metric | 4 Layers | 6 Layers | Requirement |
|--------|----------|----------|-------------|
| Data per frame | ~20MB | ~30MB | - |
| Bandwidth needed | 1.2 GB/s | 1.8 GB/s | NVMe SSD |
| Sequential read time | ~4-8ms | ~6-12ms | Problem! |

### Solution: Async Pre-buffering

```
Render Thread (Frame N):          I/O Thread (Frame N+1, N+2):
├── GPU upload Layer 1 (cached)   ├── fread() Layer 1 → buffer
├── GPU upload Layer 2 (cached)   ├── fread() Layer 2 → buffer
├── GPU upload Layer 3 (cached)   ├── fread() Layer 3 → buffer
├── GPU upload Layer 4 (cached)   ├── fread() Layer 4 → buffer
├── GPU upload Layer 5 (cached)   ├── fread() Layer 5 → buffer
├── GPU upload Layer 6 (cached)   ├── fread() Layer 6 → buffer
├── Render                        └── (continues to N+2)
└── Total: ~4-6ms ✅
```

## Architecture

### Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        HAPVideoInput                             │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌─────────────┐     ┌──────────────────────────────────────┐  │
│  │ I/O Thread  │────▶│      Pre-buffer Ring (3-5 frames)    │  │
│  │             │     │  ┌───┐ ┌───┐ ┌───┐ ┌───┐ ┌───┐      │  │
│  │ - Async read│     │  │F+1│ │F+2│ │F+3│ │F+4│ │F+5│      │  │
│  │ - Decompress│     │  └───┘ └───┘ └───┘ └───┘ └───┘      │  │
│  │   (if HAP-Q)│     └──────────────────────────────────────┘  │
│  └─────────────┘                      │                         │
│                                       ▼                         │
│  ┌─────────────┐     ┌──────────────────────────────────────┐  │
│  │ Main Thread │◀────│         readFrame() API               │  │
│  │             │     │  - Return pre-buffered frame          │  │
│  │ - Upload DXT│     │  - Signal I/O thread for next frame   │  │
│  │ - Render    │     └──────────────────────────────────────┘  │
│  └─────────────┘                                                │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Key Components

#### 1. Pre-buffer Ring

```cpp
struct HAPFrameBuffer {
    int64_t frameNumber;
    std::vector<uint8_t> dxtData;      // Compressed DXT data
    HAPTextureFormat format;            // DXT1, DXT5, etc.
    int width, height;
    bool ready;
    std::chrono::steady_clock::time_point readTime;  // For stats
};

class HAPPreBuffer {
public:
    static constexpr int BUFFER_SIZE = 5;  // Frames ahead
    
    // Get frame if available (non-blocking)
    bool getFrame(int64_t frameNumber, HAPFrameBuffer& out);
    
    // Request frames to be loaded (called by main thread)
    void requestFrames(int64_t startFrame, int count);
    
    // I/O thread fills these
    void pushFrame(HAPFrameBuffer&& frame);
    
private:
    std::array<HAPFrameBuffer, BUFFER_SIZE> ring_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<int64_t> oldestFrame_{-1};
    std::atomic<int64_t> newestFrame_{-1};
};
```

#### 2. I/O Thread

```cpp
class HAPAsyncReader {
public:
    void start();
    void stop();
    
    // Request to pre-load frames
    void prefetch(int64_t startFrame, int count);
    
    // Seek notification (invalidates buffer)
    void onSeek(int64_t targetFrame);
    
private:
    void ioThreadLoop();
    bool readHAPFrame(int64_t frameNumber, HAPFrameBuffer& out);
    
    std::thread ioThread_;
    std::atomic<bool> running_{false};
    
    // Work queue
    struct ReadRequest {
        int64_t frameNumber;
        int priority;  // Lower = higher priority
    };
    std::priority_queue<ReadRequest> workQueue_;
    std::mutex queueMutex_;
    std::condition_variable queueCV_;
    
    // File handle (kept open)
    std::unique_ptr<HAPFileReader> fileReader_;
    
    // Pre-buffer storage
    HAPPreBuffer preBuffer_;
};
```

#### 3. Modified HAPVideoInput

```cpp
class HAPVideoInput : public InputSource {
public:
    bool readFrame(int64_t frameNumber, FrameBuffer& buffer) override {
        // Check pre-buffer first (fast path)
        HAPFrameBuffer cached;
        if (asyncReader_->getPreBuffer().getFrame(frameNumber, cached)) {
            // Frame already in memory - just copy DXT data
            buffer.setDXTData(cached.dxtData, cached.format, cached.width, cached.height);
            
            // Request next frames to be loaded
            asyncReader_->prefetch(frameNumber + 1, LOOKAHEAD_COUNT);
            return true;
        }
        
        // Cache miss - synchronous read (fallback)
        LOG_WARNING << "HAP pre-buffer miss for frame " << frameNumber;
        return readFrameSync(frameNumber, buffer);
    }
    
private:
    static constexpr int LOOKAHEAD_COUNT = 5;
    std::unique_ptr<HAPAsyncReader> asyncReader_;
};
```

## Implementation Plan

### Phase 1: Pre-buffer Infrastructure (4-6 hours)

1. Create `HAPPreBuffer` class with ring buffer
2. Add thread-safe get/push operations
3. Add frame invalidation on seek
4. Add statistics (hit rate, read latency)

**Files:**
- `src/cuems_videocomposer/cpp/input/HAPPreBuffer.h`
- `src/cuems_videocomposer/cpp/input/HAPPreBuffer.cpp`

### Phase 2: Async I/O Thread (4-6 hours)

1. Create `HAPAsyncReader` class
2. Implement priority work queue
3. Implement I/O thread loop
4. Handle seek/jump invalidation
5. Add graceful shutdown

**Files:**
- `src/cuems_videocomposer/cpp/input/HAPAsyncReader.h`
- `src/cuems_videocomposer/cpp/input/HAPAsyncReader.cpp`

### Phase 3: Integrate with HAPVideoInput (2-3 hours)

1. Modify `HAPVideoInput::open()` to start async reader
2. Modify `HAPVideoInput::readFrame()` to use pre-buffer
3. Modify `HAPVideoInput::seek()` to invalidate buffer
4. Add fallback to sync read on cache miss

**Files:**
- `src/cuems_videocomposer/cpp/input/HAPVideoInput.cpp`
- `src/cuems_videocomposer/cpp/input/HAPVideoInput.h`

### Phase 4: Optimization (2-3 hours)

1. Tune buffer size based on frame rate
2. Add memory-mapped I/O option for large files
3. Add read-ahead heuristics (detect forward/backward playback)
4. Profile and optimize hot paths

### Phase 5: Testing (2-3 hours)

1. Test with 4-6 HAP layers
2. Stress test with seeks/jumps
3. Measure cache hit rate
4. Verify no memory leaks
5. Test with different HAP variants (HAP, HAP Alpha, HAP Q)

## Expected Performance

### Before (Sequential I/O)

| Layers | Read Time | Upload Time | Render | Total | Status |
|--------|-----------|-------------|--------|-------|--------|
| 4 | 4-8ms | 0.4ms | 3ms | 7-11ms | ⚠️ Tight |
| 6 | 6-12ms | 0.6ms | 4ms | 10-16ms | ❌ Drops |

### After (Async Pre-buffer)

| Layers | Read Time | Upload Time | Render | Total | Status |
|--------|-----------|-------------|--------|-------|--------|
| 4 | ~0ms (cached) | 0.4ms | 3ms | 3.4ms | ✅ Great |
| 6 | ~0ms (cached) | 0.6ms | 4ms | 4.6ms | ✅ Great |

**Improvement: 3-4× faster frame preparation**

## Memory Requirements

| Layers | Frames Buffered | Memory per Layer | Total Memory |
|--------|-----------------|------------------|--------------|
| 4 | 5 | ~25MB | ~100MB |
| 6 | 5 | ~25MB | ~150MB |

Memory usage is acceptable for modern systems.

## Disk Requirements

For reliable 6-layer HAP playback:

| Storage Type | Max Bandwidth | 6-Layer Support |
|--------------|---------------|-----------------|
| HDD (7200rpm) | ~150 MB/s | ❌ No |
| SATA SSD | ~500 MB/s | ⚠️ Marginal |
| NVMe SSD | ~2-3 GB/s | ✅ Yes |
| RAID0 NVMe | ~5+ GB/s | ✅ Excellent |

**Recommendation:** NVMe SSD required for 6-layer HAP.

## API Changes

### New Configuration Options

```cpp
// In ConfigurationManager
int hapPreBufferFrames = 5;      // Frames to buffer ahead
bool hapAsyncIO = true;          // Enable async I/O (default: true)
bool hapMemoryMap = false;       // Use mmap for large files
```

### New OSC Commands (Optional)

```
/videocomposer/hap/prebuffer/size <frames>   - Set buffer size
/videocomposer/hap/prebuffer/stats           - Get hit rate, latency
```

## Risk Assessment

| Risk | Impact | Mitigation |
|------|--------|------------|
| Seek invalidates buffer | Frame drop on seek | Pre-load target + neighbors |
| Memory pressure | OOM on low-RAM systems | Configurable buffer size |
| I/O thread stall | Delayed frames | Timeout + sync fallback |
| HAP-Q decode CPU | Adds latency | Decode in I/O thread |

## Success Criteria

1. ✅ 6 HAP layers at 60fps with <1% frame drops
2. ✅ Pre-buffer hit rate >95% during normal playback
3. ✅ Seek response <100ms (invalidate + reload)
4. ✅ Memory usage <200MB for 6 layers
5. ✅ No deadlocks or race conditions

## Estimated Effort

| Phase | Hours |
|-------|-------|
| Phase 1: Pre-buffer | 4-6 |
| Phase 2: Async I/O | 4-6 |
| Phase 3: Integration | 2-3 |
| Phase 4: Optimization | 2-3 |
| Phase 5: Testing | 2-3 |
| **Total** | **14-21 hours** |

## Alternative: io_uring (Linux-specific)

For even better I/O performance on Linux, consider `io_uring`:

```cpp
// io_uring provides kernel-level async I/O
// Can submit multiple read requests in one syscall
// Ideal for multi-file parallel reads

// Pros:
// - Lower syscall overhead
// - True async (no thread pool needed)
// - Batched operations

// Cons:
// - Linux 5.1+ only
// - More complex API
// - Platform-specific
```

Could be added as Phase 6 optimization if needed.

## Conclusion

HAP async pre-buffering is the right solution for 4-6 layer playback:
- Targets the actual bottleneck (disk I/O)
- Moderate implementation complexity
- Significant performance improvement (3-4×)
- Reasonable memory overhead

This is more effective than multi-threaded decoding for HAP because:
- HAP "decoding" is just reading (no CPU decode work)
- The GPU handles DXT decompression
- I/O latency is the bottleneck, not CPU

