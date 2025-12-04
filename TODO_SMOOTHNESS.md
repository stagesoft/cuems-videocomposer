# Playback Smoothness Improvements - TODO

## Current Status
- ✅ Single monitor: No dropped frames
- ✅ Buffer release timing fixed (release after flip, not before)
- ✅ `eglSwapInterval(0)` to disable EGL internal vsync
- ❌ Still not mpv-level smoothness (micro-jumps persist)
- ❌ Dual monitors: Dropped frames (~30fps instead of 60fps)

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

## Pending Changes (Priority Order)

### 1. HIGH: xjadeo-style Software Timer Loop
**Impact**: Should significantly improve single-monitor smoothness

**Concept**: Decouple update loop from vsync. Run at video framerate, use vsync only for tear prevention.

```cpp
// Current (vsync-driven):
while (running) {
    updateLayers();      // Poll MTC
    render();            // Render + wait for vsync (~16ms)
}                        // Loop runs at 60Hz

// Proposed (software timer like xjadeo):
while (running) {
    clock1 = getMonotonicTime();
    
    updateLayers();      // Poll MTC
    render();            // Render (vsync prevents tearing, but don't wait)
    
    elapsed = getMonotonicTime() - clock1;
    nominal_delay = 1.0 / videoFramerate;  // e.g., 40ms for 25fps
    if (elapsed < nominal_delay) {
        sleep(nominal_delay - elapsed);
    }
}                        // Loop runs at video fps (25fps)
```

**Files to modify**:
- `VideoComposerApplication.cpp`: Main loop timing
- `DRMBackend.cpp`: May need to adjust vsync waiting

**Risk**: Low - xjadeo proves this works for MTC sync

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

## References

- **xjadeo source**: `/home/ion/src/cuems/xjadeo/src/xjadeo/xjadeo.c` (main loop)
- **mpv source**: `video/out/drm_common.c`, `video/out/vo_drm.c`
- **GBM docs**: Mesa GBM API
- **DRM atomic**: `drmModeAtomicCommit()` documentation

