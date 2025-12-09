# Playback Smoothness Improvements - TODO

## Current Status
- ✅ Single monitor: No dropped frames
- ✅ Buffer release timing fixed (release after flip, not before)
- ✅ `eglSwapInterval(0)` to disable EGL internal vsync
- ✅ **Quick Win #1**: Skip decode for same frame (readFrame/readFrameToTexture)
- ✅ **Quick Win #2**: Skip same MTC frame in LayerPlayback (already existed)
- ✅ **xjadeo-style software timer**: Run loop at MTC fps, not display rate
- ❌ Dual monitors: Dropped frames (~30fps instead of 60fps) - needs atomic modesetting

---

## Analysis Summary

### Comparison: xjadeo vs cuems-videocomposer vs mpv

| Aspect | xjadeo | cuems-videocomposer | mpv |
|--------|--------|---------------------|-----|
| Loop rate | Video fps (25fps) | Display fps (60Hz) | Display fps |
| Timing driver | Software timer | VSync/page flip | VSync + presentation timing |
| Frame selection | Direct MTC frame | Direct MTC frame | PTS-based with queue |
| Buffer management | Single buffer | Double buffer | Triple buffer + queue |

### Root Cause of Micro-jumps
Our loop runs at 60Hz (vsync-driven), but MTC updates at 25fps. This creates:
1. Variable phase between MTC update and vsync
2. Same frame displayed 2-3 times, but timing of "new frame" varies relative to vsync
3. Perceived as micro-jumps even though no frames are dropped

---

## xjadeo's PTS Features (Reference)

xjadeo has sophisticated PTS handling that contributes to its smoothness. Key features:

### 1. Frame Index with PTS Mapping
Pre-builds a complete index mapping frame numbers to PTS:

```c
struct FrameIndex {
    int64_t pkt_pts;    // PTS from packet
    int64_t pkt_pos;    // Byte position in file
    int64_t frame_pts;  // PTS from decoded frame
    int64_t frame_pos;  // Byte position of frame
    int64_t timestamp;  // Expected PTS for frame[i]
    int64_t seekpts;    // PTS of keyframe to seek to
    int64_t seekpos;    // Byte position to seek to
    uint8_t key;        // Is this a keyframe?
};
```

**Benefit**: O(1) lookup from MTC frame → seek target. No seeking through file to find keyframes at runtime.

### 2. Best-Effort PTS Parsing
Tries multiple PTS sources in priority order:

```c
// In parse_pts_from_frame():
pts = f->best_effort_timestamp;  // First choice (FFmpeg's best guess)
if (pts == AV_NOPTS_VALUE)
    pts = f->pts;                // Frame's presentation timestamp
if (pts == AV_NOPTS_VALUE)
    pts = f->pkt_dts;            // Decode timestamp from packet
```

**Benefit**: Works with poorly-muxed files where PTS may be missing.

### 3. Smart Seek vs. Decode Decision
Tracks last decoded position to minimize seeks:

```c
static int64_t last_decoded_pts = -1;
static int64_t last_decoded_frameno = -1;

// In seek_frame():
if (last_decoded_pts == timestamp) return 0;  // Already have it!

int need_seek = 0;
if (last_decoded_pts < 0)                      need_seek = 1;  // First frame
else if (last_decoded_pts > timestamp)         need_seek = 1;  // Going backwards
else if ((framenumber - last_decoded_frameno) == 1) ;          // Next frame - no seek!
else if (fidx[framenumber].seekpts != fidx[last_decoded_frameno].seekpts)
    need_seek = 1;  // Different GOP
```

**Benefit**: For sequential playback, decodes without seeking. Only seeks on jumps or reverse.

### 4. Fuzzy PTS Matching
Accepts frames within one frame duration:

```c
const int64_t prefuzz = one_frame > 10 ? 1 : 0;
if (pts + prefuzz >= timestamp) {
    if (pts - timestamp < one_frame) {
        last_decoded_pts = timestamp;
        return 0;  // OK - close enough!
    }
}
```

**Benefit**: Tolerates PTS rounding errors, VFR content, or timestamp discontinuities.

### 5. Skip Redundant Display
```c
if (!force_update && dispFrame == timestamp) return;  // Already showing this frame
```

**Benefit**: Running at video fps, this rarely triggers. But if MTC stalls, avoids redundant work.

### What We Can Learn

1. **Frame index with seek targets** - We already have indexing, but could add `seekpts` to directly map frame → seek target
2. **Track last decoded frame** - Avoid seeking for sequential frames
3. **Fuzzy PTS matching** - Be tolerant of off-by-one PTS mismatches
4. **Run at video fps** - The main lesson: don't poll MTC faster than video framerate

---

## Pending Changes (Priority Order)

### 1. ✅ IMPLEMENTED: xjadeo-style Software Timer Loop
**Impact**: Should significantly improve single-monitor smoothness

**Concept**: Decouple update loop from vsync. Run at MTC framerate, use vsync only for tear prevention.

**Implementation** (`VideoComposerApplication.cpp`):
```cpp
// Get MTC framerate for timing (default 25fps if not available)
double mtcFps = globalSyncSource_->getFramerate();  // e.g., 25.0
double nominalDelaySec = 1.0 / mtcFps;  // 40ms for 25fps

while (running) {
    auto clock1 = std::chrono::steady_clock::now();
    
    updateLayers();      // Poll MTC
    render();            // Render + vsync for tear prevention
    
    auto clock2 = std::chrono::steady_clock::now();
    auto elapsed = clock2 - clock1;
    if (elapsed < nominalDelay) {
        std::this_thread::sleep_for(nominalDelay - elapsed);
    }
    clock1 = std::chrono::steady_clock::now();
}                        // Loop runs at MTC fps (25fps)
```

**Key features**:
- Uses MTC framerate dynamically (adapts if MTC fps changes)
- Vsync still provides tear prevention and buffer management
- Software timer ensures consistent MTC rate regardless of display refresh

---

### 2. MEDIUM: Triple Buffering for Render-Ahead
**Impact**: Enables render-ahead, may reduce jitter

**Concept**: Request 3 GBM buffers instead of 2, allowing render-ahead without dropped frames.

```cpp
// In DRMSurface::init() or resize():
// Try to create GBM surface with 3 buffers for triple buffering
gbmSurface_ = gbm_surface_create(gbmDevice_, width_, height_, format,
    GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
// Note: GBM may still only provide 2 buffers - need to verify
```

**With triple buffering, render-ahead becomes possible**:
```cpp
for (auto& surface : surfaces_) {
    surface->processFlipEvents();
    // With 3 buffers: can render even if flip pending (if free buffer exists)
    if (surface->isFlipPending() && !surface->hasFreeBuffers()) {
        surface->waitForFlip();
    }
}
```

**Files to modify**:
- `DRMSurface.cpp`: GBM surface creation
- `DRMBackend.cpp`: Re-enable render-ahead logic

**Risk**: Medium - GBM may not guarantee 3 buffers

---

### 3. MEDIUM: Atomic Modesetting for Dual Output
**Impact**: Fixes dual-monitor dropped frames (30fps → 60fps)

**Concept**: Submit both page flips in a single atomic commit.

```cpp
// Current (sequential, 30fps max):
for (auto& surface : surfaces_) {
    surface->swapBuffers();
    surface->schedulePageFlip();  // Each waits for its own vsync
}

// Proposed (atomic, 60fps):
drmModeAtomicReqPtr req = drmModeAtomicAlloc();
for (auto& surface : surfaces_) {
    surface->swapBuffers();
    // Add FB_ID property to atomic request instead of individual flip
    drmModeAtomicAddProperty(req, surface->getCrtcId(), 
                             FB_ID_prop, surface->getFbId());
}
// Single atomic commit for all outputs
drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK | 
                    DRM_MODE_PAGE_FLIP_EVENT, this);
```

**Files to modify**:
- `DRMSurface.cpp`: Add atomic property helpers
- `DRMBackend.cpp`: Implement atomic commit path
- `DRMOutputManager.cpp`: Query atomic capabilities

**Risk**: Medium - Requires atomic modesetting support (most modern GPUs have it)

---

### 4. LOW: Async Decode with Frame Queue (Complex)
**Impact**: Would match mpv architecture

**Concept**: Decode frames ahead of time in separate thread, pick best frame at vsync.

```cpp
// Decode thread:
while (running) {
    AVFrame* frame = decode_next_frame();
    frameQueue.push(frame);  // Thread-safe queue
}

// Render thread (at vsync):
int64_t targetPTS = getCurrentMTCTime();
AVFrame* bestFrame = frameQueue.findClosest(targetPTS);
render(bestFrame);
```

**Why this is complex**:
- Previous async decode attempt caused deadlocks
- FFmpeg decoder state is not trivially thread-safe
- Need separate decoder instance per thread, or careful mutex usage
- VAAPI zero-copy complicates frame ownership

**Files to modify**:
- `VideoFileInput.cpp`: Async decode thread
- `LayerPlayback.cpp`: Frame queue management
- New file: `FrameQueue.h/cpp`

**Risk**: High - Previous attempt failed, needs architectural rethink

---

### 5. LOW: Presentation Timing Feedback
**Impact**: Fine-tuning, marginal improvement

**Concept**: Use presentation timestamps from page flip handler to adjust frame selection.

```cpp
// In pageFlipHandler:
presentationTiming_.recordFlip(sec, usec, msc);

// In frame selection:
int64_t nextVsyncTime = presentationTiming_.predictNextVsync();
int64_t targetMTC = mtcTimeAtVsync(nextVsyncTime);
selectFrame(targetMTC);  // Pick frame for NEXT vsync, not current
```

**Files to modify**:
- `PresentationTiming.cpp`: Add prediction
- `LayerPlayback.cpp`: Use prediction for frame selection

**Risk**: Low - Incremental improvement

---

## Recommended Implementation Order

1. **xjadeo-style software timer** - Highest impact, lowest risk
2. **Triple buffering** - Enables render-ahead
3. **Atomic modesetting** - Fixes dual-monitor
4. **Presentation timing feedback** - Polish
5. **Async decode** - Only if still needed after above

---

## Testing Methodology

For each change:
1. Test with HAP codec (GPU decode - isolates display timing)
2. Test with VAAPI (hardware decode)
3. Test with software decode (stress test)
4. Compare visually with mpv and xjadeo
5. Check `PresentationTiming` dropped frame count
6. Test single and dual monitor configurations

---

## Current vs. xjadeo Comparison

| Feature | xjadeo | cuems-videocomposer | Action |
|---------|--------|---------------------|--------|
| Frame index | ✅ Full (pkt_pts, seekpts, etc.) | ✅ Basic (frame → byte offset) | Could add seekpts |
| Best-effort PTS | ✅ Multiple fallbacks | ✅ Uses `best_effort_timestamp` | OK |
| Track last decoded | ✅ `last_decoded_pts/frameno` | ❌ Seeks every frame | **Add this** |
| Skip same frame | ✅ `if (dispFrame == timestamp)` | ❌ Renders every vsync | **Add this** |
| Fuzzy PTS match | ✅ Within one_frame tolerance | ❌ Exact match only | Consider |
| Loop rate | ✅ Video fps (25fps) | ❌ Display fps (60Hz) | **Priority #1** |
| Vsync usage | ✅ Tear prevention only | ❌ Timing driver | **Priority #1** |

### Quick Wins (Low Effort, Medium Impact)

1. **Track last decoded frame** - Skip seeking if next frame is sequential
2. **Skip render if same MTC frame** - Don't re-render if MTC hasn't changed

### Major Change (High Effort, High Impact)

1. **Software timer loop** - Run at video fps instead of display fps

---

## References

- **xjadeo source**: `/home/ion/src/cuems/xjadeo/src/xjadeo/xjadeo.c` (main loop)
- **mpv source**: `video/out/drm_common.c`, `video/out/vo_drm.c`
- **GBM docs**: Mesa GBM API
- **DRM atomic**: `drmModeAtomicCommit()` documentation

