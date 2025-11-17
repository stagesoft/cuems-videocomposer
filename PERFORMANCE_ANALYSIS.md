# Performance Analysis: Layer Architecture for Maximum Concurrent Playback

## Current Architecture Performance Bottlenecks

### CPU Bottlenecks
1. **Frame Decoding (FFmpeg)**: ~5-15ms per 1080p frame per layer
   - Sequential: 10 layers = 50-150ms total
   - Parallel: 10 layers = 5-15ms (if multi-threaded)

2. **FrameBuffer Memory Operations**: 
   - Allocate/copy: ~1-2ms per frame
   - CPU-side image processing (if added): ~10-50ms per frame

### GPU Bottlenecks
1. **Texture Upload (CPU→GPU)**: **MAJOR BOTTLENECK**
   - `glTexImage2D()`: ~2-5ms per 1080p frame (synchronous, blocks CPU)
   - Current: Sequential uploads per layer
   - 10 layers = 20-50ms just for uploads

2. **GPU Rendering**: Already efficient
   - Transform/crop/pan via texture coordinates: <0.1ms
   - Blend modes: <0.1ms per layer
   - Quad rendering: <0.1ms per layer

## Architecture Options Analysis

### Option A: CPU-Side Image Modifications (Modify FrameBuffer)
**Structure:**
```
VideoPlayback → modifies FrameBuffer → VideoDisplay → GPU upload
```

**Pros:**
- Simple architecture
- Modifications visible before GPU upload
- Easy to debug

**Cons:**
- **CRITICAL**: CPU pixel manipulation is SLOW (10-50ms per frame)
- Blocks CPU thread during modification
- Can't parallelize with frame decoding
- Memory bandwidth intensive
- **Limits to ~5-10 layers max** (CPU-bound)

**Performance Impact:**
- 10 layers × 30ms CPU processing = 300ms per frame
- **NOT VIABLE for many layers**

---

### Option B: GPU-Side Only (Current + Separation)
**Structure:**
```
VideoPlayback → FrameBuffer (raw) → VideoDisplay → GPU upload → GPU transforms
```

**Pros:**
- **GPU transforms are FAST** (<0.1ms per layer)
- CPU only does decoding (can be parallelized)
- No CPU pixel manipulation overhead
- **Can support 20-50+ layers** (GPU-bound, not CPU-bound)
- Transform/crop/pan via texture coordinates (current approach)
- Blend modes already GPU-accelerated

**Cons:**
- Modifications are render-time only (not in FrameBuffer)
- Slightly more complex render parameter passing

**Performance Impact:**
- 10 layers: ~50ms decoding (parallel) + ~25ms uploads + ~1ms GPU = ~76ms
- **VIABLE for many layers**

---

### Option C: Hybrid (Selective CPU + GPU)
**Structure:**
```
VideoPlayback → FrameBuffer → VideoDisplay (CPU for some, GPU for others)
```

**Pros:**
- Flexibility: CPU for complex ops, GPU for simple ones
- Can optimize per operation type

**Cons:**
- **Complexity**: Need to decide CPU vs GPU per operation
- **CPU operations still slow**: Any CPU-side work limits scalability
- Hard to optimize
- **Limits to ~10-15 layers max**

**Performance Impact:**
- Variable, but CPU operations become bottleneck
- **MODERATELY VIABLE**

---

## Recommended Architecture: Option B (GPU-Side Only)

### Component Structure

```
┌─────────────────────────────────────────────────────────┐
│                    VideoPlayback                        │
│  - SyncSource polling                                   │
│  - Frame synchronization                                │
│  - InputSource frame loading                            │
│  - FrameBuffer (raw decoded frames)                     │
│  - Time scaling/offset                                  │
│  - Playback state                                       │
└─────────────────────────────────────────────────────────┘
                          │
                          │ provides FrameBuffer
                          ▼
┌─────────────────────────────────────────────────────────┐
│                    VideoDisplay                         │
│  - LayerProperties (pan, crop, transform, blend)        │
│  - Render parameters calculation                        │
│  - GPU texture management                               │
│  - NO FrameBuffer modification                          │
│  - Prepares render instructions                        │
└─────────────────────────────────────────────────────────┘
                          │
                          │ render instructions
                          ▼
┌─────────────────────────────────────────────────────────┐
│                 OpenGLRenderer                          │
│  - Texture upload (optimized)                           │
│  - GPU transforms (texture coords)                      │
│  - GPU blend modes                                       │
│  - Parallel layer rendering (future)                    │
└─────────────────────────────────────────────────────────┘
```

### Key Design Decisions

1. **VideoPlayback Responsibilities:**
   - Frame synchronization (sync source polling)
   - Frame loading from InputSource
   - Time scaling/offset calculations
   - Playback state management
   - **Output: Raw FrameBuffer (never modified)**

2. **VideoDisplay Responsibilities:**
   - Store LayerProperties (pan, crop, transform, blend, opacity)
   - Calculate render parameters (texture coordinates, transforms)
   - Manage GPU texture resources
   - **NO FrameBuffer pixel manipulation**
   - **Output: Render instructions/parameters**

3. **OpenGLRenderer Responsibilities:**
   - Upload FrameBuffer to GPU texture (optimize this!)
   - Apply GPU transforms (via texture coordinates)
   - Apply GPU blend modes
   - Render quads

### Performance Optimizations

1. **Parallel Frame Decoding:**
   - VideoPlayback can decode frames in parallel threads
   - Each layer has independent decoding thread
   - **10x speedup for 10 layers**

2. **Optimize Texture Uploads:**
   - Use Pixel Buffer Objects (PBOs) for async uploads
   - Use `glTexSubImage2D()` when possible (faster than `glTexImage2D()`)
   - Batch uploads or use texture streaming
   - **2-3x speedup for uploads**

3. **Pre-calculate Render Parameters:**
   - VideoDisplay calculates texture coords/transforms once
   - Store in render parameters struct
   - Avoid recalculation in render loop
   - **Minimal CPU overhead**

4. **Future: GPU Compute Shaders:**
   - Move some operations to compute shaders
   - Even more parallelization
   - **Potential 5-10x speedup**

### Performance Estimates

**Current (10 layers):**
- Decoding: 150ms (sequential)
- Uploads: 50ms
- GPU render: 1ms
- **Total: ~201ms per frame = ~5 FPS**

**Optimized Option B (10 layers):**
- Decoding: 15ms (parallel, 10 threads)
- Uploads: 20ms (PBOs, optimized)
- GPU render: 1ms
- **Total: ~36ms per frame = ~28 FPS**

**Optimized Option B (20 layers):**
- Decoding: 30ms (parallel, 20 threads)
- Uploads: 40ms (PBOs, optimized)
- GPU render: 2ms
- **Total: ~72ms per frame = ~14 FPS**

**Optimized Option B (50 layers):**
- Decoding: 75ms (parallel, 50 threads)
- Uploads: 100ms (PBOs, optimized)
- GPU render: 5ms
- **Total: ~180ms per frame = ~5.5 FPS**

## Conclusion

**Option B (GPU-Side Only)** is the clear winner:
- ✅ Supports 20-50+ layers (vs 5-10 for CPU-side)
- ✅ GPU transforms are 100-500x faster than CPU
- ✅ Can parallelize frame decoding
- ✅ Scalable architecture
- ✅ Matches current efficient approach

**Critical Optimization:** Texture uploads are the bottleneck, not rendering.
Focus optimization efforts on:
1. Parallel frame decoding
2. Async texture uploads (PBOs)
3. Texture reuse when frames don't change

