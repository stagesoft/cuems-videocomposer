# HAP Async I/O Pre-buffering

## Goal

Support HAP layer playback at 60fps without frame drops, scaled to hardware capabilities:

| Platform | HAP Layers @ 1080p | HAP Layers @ 4K |
|----------|-------------------|-----------------|
| **Intel N100/N101** | 2-3 layers | 1 layer |
| **Intel i5 / Ryzen iGPU** | 4-5 layers | 2-3 layers |
| **NVIDIA Discrete** | 6+ layers | 4-6 layers |

## Target Hardware Platforms

### Low-End: Intel N100/N101

| Specification | Value | Impact on HAP |
|---------------|-------|---------------|
| CPU Cores | 4 E-cores | Limited parallel I/O |
| Memory Bandwidth | ~51 GB/s | Texture upload bottleneck |
| GPU (Intel UHD) | 24 EUs | DXT decompression OK |
| PCIe | Gen3 x4 (NVMe) | ~3.5 GB/s max |
| RAM | Typically 8-16GB | Pre-buffer memory limited |

**N100 HAP Constraints:**
- CPU-bound for memcpy during texture upload
- Memory bandwidth limits texture transfer
- **Recommend:** 2-3 HAP layers @ 1080p, or prefer H.264/H.265 VAAPI

### Mid-Range: Intel i5 / AMD Ryzen (iGPU)

| Specification | Intel i5 (12th+) | Ryzen 7000/8000 |
|---------------|------------------|-----------------|
| CPU Cores | 6P+4E or 6P+8E | 6-8 cores |
| Memory Bandwidth | ~76-89 GB/s | ~89 GB/s (DDR5) |
| GPU | Iris Xe (96 EUs) | RDNA 3 (4-12 CUs) |
| PCIe | Gen4 x4 | Gen4 x4 |

**Mid-Range HAP Capability:**
- Sufficient CPU for 4-5 HAP layers
- DDR5 bandwidth helps texture upload
- **Recommend:** 4-5 HAP layers @ 1080p, 2-3 @ 4K

### High-End: NVIDIA Discrete

| Specification | GTX 16xx | RTX 30xx/40xx |
|---------------|----------|---------------|
| Memory Bandwidth | 192-336 GB/s | 448-1008 GB/s |
| PCIe | Gen3 x16 | Gen4 x16 |
| CUDA Cores | 1280-1536 | 5888-16384 |

**NVIDIA HAP Capability:**
- Massive bandwidth for texture upload
- Fast DXT decompression
- **Recommend:** 6+ HAP layers, limited by disk I/O

---

## Problem Analysis

### Current HAP Pipeline (Single-threaded)

```
Frame N Timeline (must complete in 16.67ms @ 60fps):
├── Wait for MTC sync                    ~0.1ms
├── Layer 1: fread() → DXT upload        ~1-2ms  ← BLOCKING I/O
├── Layer 2: fread() → DXT upload        ~1-2ms  ← BLOCKING I/O
├── Layer 3: fread() → DXT upload        ~1-2ms  ← BLOCKING I/O
├── Layer N: ...
├── Render composited frame              ~2-5ms (GPU dependent)
└── Total                                Varies by platform
```

### Platform-Specific Bottlenecks

| Bottleneck | N100 | i5/Ryzen | NVIDIA |
|------------|------|----------|--------|
| Disk I/O | ⚠️ Critical | ⚠️ Critical | ⚠️ Critical |
| CPU memcpy | 🔴 Limiting | 🟡 OK | ✅ Fast |
| Memory bandwidth | 🔴 Limiting | 🟡 OK | ✅ Excellent |
| GPU DXT decompress | ✅ OK | ✅ OK | ✅ Excellent |

### Data Rate Requirements

| Resolution | HAP Size/Frame | 2 Layers | 4 Layers | 6 Layers |
|------------|----------------|----------|----------|----------|
| 1080p | ~5 MB | 300 MB/s | 600 MB/s | 900 MB/s |
| 4K | ~20 MB | 1.2 GB/s | 2.4 GB/s | 3.6 GB/s |

**Storage Requirements by Platform:**

| Platform | Recommended Storage | Max Layers (1080p) |
|----------|--------------------|--------------------|
| N100 | NVMe SSD (1+ GB/s) | 3 layers |
| i5/Ryzen | NVMe SSD (2+ GB/s) | 5 layers |
| NVIDIA | NVMe SSD or RAID0 | 6+ layers |

---

## Solution: Async Pre-buffering

### Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        HAPVideoInput                             │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌─────────────┐     ┌──────────────────────────────────────┐  │
│  │ I/O Thread  │────▶│      Pre-buffer Ring (N frames)      │  │
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

### Platform-Adaptive Configuration

```cpp
struct HAPPreBufferConfig {
    int bufferFrames;       // Frames to buffer ahead
    size_t maxMemoryMB;     // Memory limit
    bool useMemoryMap;      // mmap for large files
    int ioThreadPriority;   // Thread scheduling priority
    
    // Platform presets
    static HAPPreBufferConfig forN100() {
        return {
            .bufferFrames = 3,      // Smaller buffer (memory limited)
            .maxMemoryMB = 75,      // ~75MB max (conservative)
            .useMemoryMap = true,   // Reduce memcpy
            .ioThreadPriority = 0   // Normal priority
        };
    }
    
    static HAPPreBufferConfig forMidRange() {
        return {
            .bufferFrames = 5,
            .maxMemoryMB = 150,
            .useMemoryMap = true,
            .ioThreadPriority = -5  // Slightly elevated
        };
    }
    
    static HAPPreBufferConfig forHighEnd() {
        return {
            .bufferFrames = 8,
            .maxMemoryMB = 300,
            .useMemoryMap = false,  // Direct read is fast enough
            .ioThreadPriority = -10 // Elevated for I/O
        };
    }
};
```

---

## Key Components

### 1. Pre-buffer Ring

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
    // Platform-adaptive buffer size
    void initialize(const HAPPreBufferConfig& config);
    
    // Get frame if available (non-blocking)
    bool getFrame(int64_t frameNumber, HAPFrameBuffer& out);
    
    // Request frames to be loaded (called by main thread)
    void requestFrames(int64_t startFrame, int count);
    
    // I/O thread fills these
    void pushFrame(HAPFrameBuffer&& frame);
    
    // Memory pressure handling
    void trimToMemoryLimit(size_t maxBytes);
    
private:
    std::vector<HAPFrameBuffer> ring_;  // Dynamic size based on config
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<int64_t> oldestFrame_{-1};
    std::atomic<int64_t> newestFrame_{-1};
    size_t currentMemoryUsage_{0};
    size_t maxMemoryUsage_;
};
```

### 2. I/O Thread

```cpp
class HAPAsyncReader {
public:
    void start(const HAPPreBufferConfig& config);
    void stop();
    
    // Request to pre-load frames
    void prefetch(int64_t startFrame, int count);
    
    // Seek notification (invalidates buffer)
    void onSeek(int64_t targetFrame);
    
    // Platform detection
    static HAPPreBufferConfig detectOptimalConfig();
    
private:
    void ioThreadLoop();
    bool readHAPFrame(int64_t frameNumber, HAPFrameBuffer& out);
    
    // Platform-specific optimizations
    bool useMemoryMappedRead(const std::string& path, HAPFrameBuffer& out);
    bool useDirectRead(const std::string& path, HAPFrameBuffer& out);
    
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
    HAPPreBufferConfig config_;
};
```

### 3. Modified HAPVideoInput

```cpp
class HAPVideoInput : public InputSource {
public:
    bool open(const std::string& path) override {
        // Detect platform and configure
        auto config = HAPAsyncReader::detectOptimalConfig();
        
        // Warn if platform is limited
        if (isN100Platform()) {
            LOG_INFO << "Intel N100 detected: HAP limited to " 
                     << config.bufferFrames << " frames, "
                     << config.maxMemoryMB << "MB buffer";
            LOG_INFO << "Consider H.264/H.265 VAAPI for better performance";
        }
        
        asyncReader_ = std::make_unique<HAPAsyncReader>();
        asyncReader_->start(config);
        return true;
    }
    
    bool readFrame(int64_t frameNumber, FrameBuffer& buffer) override {
        // Check pre-buffer first (fast path)
        HAPFrameBuffer cached;
        if (asyncReader_->getPreBuffer().getFrame(frameNumber, cached)) {
            // Frame already in memory - just copy DXT data
            buffer.setDXTData(cached.dxtData, cached.format, 
                              cached.width, cached.height);
            
            // Request next frames to be loaded
            asyncReader_->prefetch(frameNumber + 1, getLookaheadCount());
            return true;
        }
        
        // Cache miss - synchronous read (fallback)
        LOG_WARNING << "HAP pre-buffer miss for frame " << frameNumber;
        return readFrameSync(frameNumber, buffer);
    }
    
private:
    int getLookaheadCount() const {
        // Platform-adaptive lookahead
        return config_.bufferFrames;
    }
    
    std::unique_ptr<HAPAsyncReader> asyncReader_;
    HAPPreBufferConfig config_;
};
```

---

## Implementation Plan

### Phase 1: Pre-buffer Infrastructure (4-6 hours)

1. Create `HAPPreBuffer` class with dynamic ring buffer
2. Add thread-safe get/push operations
3. Add frame invalidation on seek
4. Add memory limit enforcement
5. Add statistics (hit rate, read latency)

**Files:**
- `src/cuems_videocomposer/cpp/input/HAPPreBuffer.h`
- `src/cuems_videocomposer/cpp/input/HAPPreBuffer.cpp`

### Phase 2: Platform Detection (2-3 hours)

1. Detect CPU model (N100 vs i5 vs desktop)
2. Query available memory
3. Detect storage speed (NVMe vs SATA)
4. Select appropriate configuration preset

**Files:**
- `src/cuems_videocomposer/cpp/utils/PlatformDetection.h`
- `src/cuems_videocomposer/cpp/utils/PlatformDetection.cpp`

### Phase 3: Async I/O Thread (4-6 hours)

1. Create `HAPAsyncReader` class
2. Implement priority work queue
3. Implement I/O thread loop
4. Handle seek/jump invalidation
5. Add graceful shutdown
6. Add memory-mapped I/O option for N100

**Files:**
- `src/cuems_videocomposer/cpp/input/HAPAsyncReader.h`
- `src/cuems_videocomposer/cpp/input/HAPAsyncReader.cpp`

### Phase 4: Integrate with HAPVideoInput (2-3 hours)

1. Modify `HAPVideoInput::open()` to start async reader with platform config
2. Modify `HAPVideoInput::readFrame()` to use pre-buffer
3. Modify `HAPVideoInput::seek()` to invalidate buffer
4. Add fallback to sync read on cache miss
5. Add platform-specific warnings in logs

**Files:**
- `src/cuems_videocomposer/cpp/input/HAPVideoInput.cpp`
- `src/cuems_videocomposer/cpp/input/HAPVideoInput.h`

### Phase 5: Testing (3-4 hours)

1. **N100 Testing:**
   - Test 2-3 HAP layers @ 1080p
   - Verify memory usage stays under 100MB
   - Test with 8GB RAM system
   
2. **Mid-Range Testing:**
   - Test 4-5 HAP layers @ 1080p
   - Test 2-3 HAP layers @ 4K
   - Verify DDR5 vs DDR4 performance

3. **General Testing:**
   - Stress test with seeks/jumps
   - Measure cache hit rate
   - Verify no memory leaks
   - Test with different HAP variants (HAP, HAP Alpha, HAP Q)

---

## Expected Performance by Platform

### Intel N100/N101

| Metric | Before | After | Notes |
|--------|--------|-------|-------|
| 2 HAP layers | 4-6ms | ~2ms | ✅ Achievable |
| 3 HAP layers | 6-9ms | ~3ms | ⚠️ Tight |
| 4 HAP layers | 8-12ms | ~4ms | ❌ May drop |
| Pre-buffer memory | N/A | 50-75MB | Limited |

**N100 Recommendation:** Use 2-3 HAP layers max. For more layers, prefer H.264/H.265 VAAPI.

### Intel i5 / AMD Ryzen (iGPU)

| Metric | Before | After | Notes |
|--------|--------|-------|-------|
| 4 HAP layers | 6-10ms | ~3ms | ✅ Good |
| 5 HAP layers | 8-12ms | ~4ms | ✅ OK |
| 6 HAP layers | 10-15ms | ~5ms | ⚠️ Possible |
| Pre-buffer memory | N/A | 100-150MB | Comfortable |

### NVIDIA Discrete

| Metric | Before | After | Notes |
|--------|--------|-------|-------|
| 6 HAP layers | 8-12ms | ~3ms | ✅ Excellent |
| 8 HAP layers | 12-16ms | ~4ms | ✅ Good |
| Pre-buffer memory | N/A | 200-300MB | Plenty |

---

## Memory Requirements by Platform

| Platform | Max Layers | Frames Buffered | Memory/Layer | Total Memory |
|----------|------------|-----------------|--------------|--------------|
| N100 | 3 | 3 | ~25MB | ~75MB |
| i5/Ryzen | 5 | 5 | ~25MB | ~125MB |
| NVIDIA | 8 | 8 | ~25MB | ~200MB |

---

## Storage Requirements

| Platform | Recommended | Minimum | Notes |
|----------|-------------|---------|-------|
| N100 | NVMe Gen3 | SATA SSD | Gen4 overkill for PCIe lanes |
| i5/Ryzen | NVMe Gen4 | NVMe Gen3 | Full bandwidth |
| NVIDIA | NVMe Gen4 or RAID0 | NVMe Gen3 | For 6+ layers |

---

## Configuration Options

### Command-Line / Config File

```ini
[hap]
# Auto-detect (default) or manual override
platform = auto  # auto | low | mid | high

# Manual overrides
prebuffer_frames = 5
max_memory_mb = 150
use_mmap = true

# Debug
log_cache_hits = false
```

### OSC Commands

```
/videocomposer/hap/prebuffer/size <frames>   - Set buffer size
/videocomposer/hap/prebuffer/stats           - Get hit rate, latency
/videocomposer/hap/platform                  - Report detected platform
```

---

## Risk Assessment

| Risk | Platform | Impact | Mitigation |
|------|----------|--------|------------|
| Memory pressure | N100 | OOM possible | Hard memory limit, warnings |
| I/O too slow | All | Frame drops | Detect at startup, warn user |
| CPU too slow | N100 | Upload stalls | Recommend VAAPI instead |
| Seek invalidates buffer | All | Momentary drop | Pre-load target + neighbors |
| HAP-Q decode CPU | N100 | Adds latency | Decode in I/O thread, warn |

---

## Success Criteria by Platform

### Intel N100/N101
1. ✅ 2-3 HAP layers at 60fps with <2% frame drops
2. ✅ Memory usage <100MB
3. ✅ Clear warning if user exceeds recommended layers

### Intel i5 / AMD Ryzen
1. ✅ 4-5 HAP layers at 60fps with <1% frame drops
2. ✅ Pre-buffer hit rate >95%
3. ✅ Memory usage <200MB

### All Platforms
1. ✅ Seek response <100ms
2. ✅ No deadlocks or race conditions
3. ✅ Graceful degradation when limits exceeded

---

## Estimated Effort

| Phase | Hours | Priority |
|-------|-------|----------|
| Phase 1: Pre-buffer | 4-6 | ⭐⭐⭐⭐⭐ |
| Phase 2: Platform Detection | 2-3 | ⭐⭐⭐⭐⭐ |
| Phase 3: Async I/O | 4-6 | ⭐⭐⭐⭐⭐ |
| Phase 4: Integration | 2-3 | ⭐⭐⭐⭐⭐ |
| Phase 5: Testing | 3-4 | ⭐⭐⭐⭐ |
| **Total** | **15-22 hours** | - |

---

## Alternative Approaches

### For N100: Prefer VAAPI Over HAP

On Intel N100, VAAPI H.264/H.265 decode is significantly more efficient than HAP:

| Codec | CPU Load | Memory BW | Layers @ 1080p |
|-------|----------|-----------|----------------|
| HAP | High (memcpy) | High | 2-3 |
| H.264 VAAPI | Low | Low | 4-6 |
| H.265 VAAPI | Low | Low | 4-6 |

**Recommendation:** For N100 systems, recommend H.264/H.265 content. Reserve HAP for alpha channel requirements.

### io_uring (Linux 5.1+)

For even better I/O performance on Linux:

```cpp
// io_uring provides kernel-level async I/O
// Beneficial on all platforms, especially N100

// Pros:
// - Lower syscall overhead (important for weak N100 CPU)
// - True async (no thread pool needed)
// - Batched operations

// Cons:
// - Linux 5.1+ only
// - More complex API
```

Could be added as Phase 6 optimization, especially beneficial for N100.

---

## Conclusion

HAP async pre-buffering with platform-adaptive configuration is the right solution:

| Platform | HAP Viability | Recommendation |
|----------|---------------|----------------|
| N100 | ⚠️ Limited | 2-3 layers; prefer VAAPI |
| i5/Ryzen | ✅ Good | 4-5 layers HAP or VAAPI |
| NVIDIA | ✅ Excellent | 6+ layers HAP |

**Key insight for N100:** The CPU and memory bandwidth are the bottleneck, not disk I/O. Pre-buffering helps, but doesn't solve the fundamental limitation. For N100, VAAPI decode is the better path for most content.
