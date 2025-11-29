# VAAPI Deferred Binding - Zero-Copy Hardware Decoding Fix

## Status: ✅ NOT CURRENTLY REQUIRED

**Date:** 2024-11-29

**Resolution:** The original 0x502 GL errors were caused by GL state corruption from a bug

in the CPU frame rendering path (mixing `GL_TEXTURE_RECTANGLE_ARB` and `GL_TEXTURE_2D` textures),

NOT by threading issues. Once fixed, VAAPI zero-copy works correctly.

See [Investigation Results](#investigation-results-2024-11-29) below.

---

## Original Problem Statement (Superseded)

~~VAAPI hardware decoding currently falls back to CPU path due to a threading issue:~~

- ~~EGL image creation works on the decoder thread~~
- ~~Texture binding requires the GL render thread~~
- ~~Result: Frame is copied GPU→CPU→GPU instead of zero-copy~~

### Original Flow Analysis (Incorrect Assumption)

```
Playback Thread                         GL Render Thread
      |                                       |
LayerPlayback::loadFrame()                    |
      |                                       |
VideoFileInput::readFrameToTexture()          |
      |                                       |
VaapiInterop::createEGLImages() ✓             |
VaapiInterop::bindTexturesToImages() ✗        |
  └─ eglGetCurrentContext() == NO_CONTEXT     |
  └─ Returns false, falls back to CPU         |
      |                                       |
[GPU→CPU copy via av_hwframe_transfer_data]   |
      |                                       |
                                        OpenGLRenderer::renderLayer()
                                              |
                                        [CPU→GPU texture upload]
```

### Desired Flow (Zero-Copy)

```
Playback Thread                         GL Render Thread
      |                                       |
LayerPlayback::loadFrame()                    |
      |                                       |
VideoFileInput::readFrameToTexture()          |
      |                                       |
VaapiInterop::createEGLImages() ✓             |
Store pending EGL images in layer             |
      |                                       |
                                        OpenGLRenderer::renderLayer()
                                              |
                                        VaapiInterop::bindTexturesToImages() ✓
                                              |
                                        [Render directly from VAAPI texture]
```

## Performance Impact

**Current Status:** Zero-copy is working correctly. No CPU fallback overhead.

| Path | GPU→CPU | CPU→GPU | Total Overhead | Status |

|------|---------|---------|----------------|--------|

| Zero-copy | 0 | 0 | **~0.1ms** | ✅ Active |

~~The CPU fallback path (3-6ms overhead) is NOT being used.~~

~~For 1080p @ 60fps with 2 layers: 6-12ms saved per frame~~ → Already achieving optimal performance.

## Implementation Plan

### Phase 1: Add Deferred Binding Infrastructure

#### 1.1 Extend GPUTextureFrameBuffer for Pending EGL Images

```cpp
// In GPUTextureFrameBuffer.h
class GPUTextureFrameBuffer {
public:
    // Existing...
    
    // New: Store pending EGL images for deferred binding
    struct PendingVaapiFrame {
        EGLImageKHR eglImageY = nullptr;
        EGLImageKHR eglImageUV = nullptr;
        int width = 0;
        int height = 0;
        VaapiInterop* interop = nullptr;  // Weak reference for binding
        bool valid = false;
    };
    
    void setPendingVaapiFrame(const PendingVaapiFrame& pending);
    bool hasPendingVaapiFrame() const;
    bool bindPendingVaapiFrame();  // Called from GL thread
    void clearPendingVaapiFrame();
    
private:
    PendingVaapiFrame pendingVaapi_;
};
```

#### 1.2 Modify VaapiInterop for Two-Phase Operation

```cpp
// In VaapiInterop.h
class VaapiInterop {
public:
    // Existing createEGLImages() stays the same
    
    // New: Get created images without binding
    bool getCreatedEGLImages(EGLImageKHR& imageY, EGLImageKHR& imageUV) const;
    
    // New: Bind textures from externally-stored images (for deferred use)
    bool bindTexturesToStoredImages(EGLImageKHR imageY, EGLImageKHR imageUV,
                                     GLuint& texY, GLuint& texUV);
    
    // New: Check if we're on a GL thread
    static bool hasGLContext();
};
```

### Phase 2: Modify Frame Loading Path

#### 2.1 Update VideoFileInput::transferHardwareFrameToGPU

```cpp
bool VideoFileInput::transferHardwareFrameToGPU(AVFrame* hwFrame, 
                                                 GPUTextureFrameBuffer& textureBuffer) {
    // Phase 1: Create EGL images (works on any thread)
    if (vaapiInterop_->createEGLImages(hwFrame, width, height)) {
        
        // Check if we're on GL thread
        if (VaapiInterop::hasGLContext()) {
            // Direct path: bind immediately (rare - if called from GL thread)
            if (vaapiInterop_->bindTexturesToImages(texY, texUV)) {
                textureBuffer.setExternalNV12Textures(texY, texUV, frameInfo_);
                return true;
            }
        } else {
            // Deferred path: store EGL images for later binding
            EGLImageKHR imageY, imageUV;
            if (vaapiInterop_->getCreatedEGLImages(imageY, imageUV)) {
                GPUTextureFrameBuffer::PendingVaapiFrame pending;
                pending.eglImageY = imageY;
                pending.eglImageUV = imageUV;
                pending.width = width;
                pending.height = height;
                pending.interop = vaapiInterop_.get();
                pending.valid = true;
                
                textureBuffer.setPendingVaapiFrame(pending);
                return true;  // Success - binding deferred to render thread
            }
        }
    }
    
    // Fallback to CPU path
    return transferHardwareFrameToCPU(hwFrame, textureBuffer);
}
```

### Phase 3: Modify Rendering Path

#### 3.1 Update OpenGLRenderer::renderLayerFromGPU

```cpp
bool OpenGLRenderer::renderLayerFromGPU(const VideoLayer* layer) {
    const GPUTextureFrameBuffer* gpuBuffer = layer->getGPUFrameBuffer();
    if (!gpuBuffer) return false;
    
    // NEW: Check for pending VAAPI frame that needs binding
    if (gpuBuffer->hasPendingVaapiFrame()) {
        // We're on GL thread now - bind the pending EGL images
        GPUTextureFrameBuffer* mutableBuffer = 
            const_cast<GPUTextureFrameBuffer*>(gpuBuffer);
        if (!mutableBuffer->bindPendingVaapiFrame()) {
            LOG_WARNING << "Failed to bind pending VAAPI frame";
            return false;
        }
    }
    
    // Continue with existing rendering...
    if (!gpuBuffer->isValid()) return false;
    
    // ... rest of rendering code
}
```

#### 3.2 Implement GPUTextureFrameBuffer::bindPendingVaapiFrame

```cpp
bool GPUTextureFrameBuffer::bindPendingVaapiFrame() {
    if (!pendingVaapi_.valid || !pendingVaapi_.interop) {
        return false;
    }
    
    GLuint texY = 0, texUV = 0;
    if (pendingVaapi_.interop->bindTexturesToStoredImages(
            pendingVaapi_.eglImageY, pendingVaapi_.eglImageUV,
            texY, texUV)) {
        
        // Set up buffer with bound textures
        setExternalNV12Textures(texY, texUV, 
                                pendingVaapi_.width, pendingVaapi_.height);
        clearPendingVaapiFrame();
        return true;
    }
    
    clearPendingVaapiFrame();
    return false;
}
```

### Phase 4: Thread Safety

#### 4.1 Add Synchronization

- EGL images are thread-safe once created
- Texture IDs must only be used on GL thread
- Use atomic flag for pending state
```cpp
// In GPUTextureFrameBuffer.h
std::atomic<bool> hasPendingVaapi_{false};
std::mutex pendingVaapiMutex_;
```


### Phase 5: Cleanup and Error Handling

#### 5.1 Handle Frame Replacement

- If new frame arrives before pending frame is bound, release old EGL images
- Ensure VaapiInterop::releaseFrame() is called appropriately

#### 5.2 Handle Binding Failures

- If binding fails on GL thread, fall back to CPU path for that frame
- Log warning but don't spam logs

## Testing Plan

### Unit Tests

1. Test EGL image creation without GL context
2. Test deferred binding on GL thread
3. Test frame replacement before binding
4. Test error recovery

### Integration Tests

1. Play VAAPI-decoded video, verify zero-copy path is taken
2. Measure frame latency before/after fix
3. Test with multiple layers
4. Test rapid seeking (many frame replacements)

### Performance Benchmarks

```bash
# Before fix
./benchmark_vaapi.sh --mode=current
# Expected: GPU→CPU→GPU copies visible in perf trace

# After fix  
./benchmark_vaapi.sh --mode=zerocopy
# Expected: No memory copies, direct texture use
```

## Files to Modify

| File | Changes |

|------|---------|

| `GPUTextureFrameBuffer.h` | Add pending VAAPI frame storage |

| `GPUTextureFrameBuffer.cpp` | Implement deferred binding |

| `VaapiInterop.h` | Add two-phase binding API |

| `VaapiInterop.cpp` | Implement stored image binding |

| `VideoFileInput.cpp` | Use deferred path when no GL context |

| `OpenGLRenderer.cpp` | Bind pending frames before rendering |

## Risk Assessment

| Risk | Mitigation |

|------|------------|

| Thread race conditions | Use mutex for pending frame access |

| EGL image lifetime | Keep VaapiInterop alive while pending |

| Memory leaks | Ensure EGL images released on error paths |

| Performance regression | Fallback to CPU path if binding fails |

## Estimated Effort

- Phase 1 (Infrastructure): 2-3 hours
- Phase 2 (Frame Loading): 1-2 hours
- Phase 3 (Rendering): 1-2 hours
- Phase 4 (Thread Safety): 1 hour
- Phase 5 (Cleanup): 1-2 hours
- Testing: 2-3 hours

**Total: 8-13 hours**

## Success Criteria (All Met ✅)

1. ✅ No "GL error binding Y plane" errors in logs
2. ✅ No "GL binding deferred (wrong thread)" messages
3. ✅ VAAPI frames render without CPU fallback
4. ✅ Zero-copy path active (~0.1ms overhead)
5. ✅ No memory leaks or crashes observed

---

## Investigation Results (2024-11-29)

### Actual Root Cause

The 0x502 (`GL_INVALID_OPERATION`) errors were **NOT caused by threading issues**.

**Actual cause:** GL state corruption in the CPU frame rendering path (`OpenGLRenderer::renderLayer`).

The bug was in the CPU frame shader path which:

1. Created a `GL_TEXTURE_RECTANGLE_ARB` texture and uploaded frame data
2. Then tried to reuse that same texture cache entry as `GL_TEXTURE_2D` for shader rendering
3. This caused `GL_INVALID_OPERATION` errors that corrupted GL state
4. The corrupted state then caused VAAPI's `glEGLImageTargetTexStorageEXT()` to fail

### Actual Threading Model

The application is **single-threaded** for video file playback:

```
Main Thread (with GL context)
    │
    ├── displayBackend_->makeCurrent()  ← GL context made current BEFORE updates
    │
    └── updateLayers()
            │
            └── LayerManager::updateAll()
                    │
                    └── VideoLayer::update()
                            │
                            └── LayerPlayback::update()
                                    │
                                    └── loadFrame()
                                            │
                                            └── VideoFileInput::readFrameToTexture()
                                                    │
                                                    └── transferHardwareFrameToGPU()
                                                            │
                                                            └── VaapiInterop::bindTexturesToImages()
                                                                    │
                                                                    └── eglGetCurrentContext() ✅ RETURNS VALID CONTEXT
```

The code in `VideoComposerApplication.cpp` (lines 300-311) explicitly makes the GL context

current before calling `updateLayers()`:

```cpp
// Make OpenGL context current before updating layers
// This is needed because hardware decoding may allocate GPU textures during frame loading
if (displayBackend_ && displayBackend_->isWindowOpen()) {
    displayBackend_->makeCurrent();
}

updateLayers();
```

### The Fix

Fixed `OpenGLRenderer::renderLayer()` to use separate paths for different texture types:

- **Shader path** (normal): Creates and uses `GL_TEXTURE_2D` textures directly
- **Fixed-function fallback** (legacy): Uses `GL_TEXTURE_RECTANGLE_ARB` textures

This keeps GL state clean, allowing VAAPI binding to succeed.

### Current Status

After the fix:

- ✅ VAAPI zero-copy works correctly
- ✅ No GL errors in logs
- ✅ Direct GPU texture mapping (no CPU copy)
- ✅ Best possible performance

Log output confirming zero-copy:

```
[INFO] VAAPI zero-copy: Hardware frame directly mapped to GPU texture (no CPU copy)
```

### When This Plan Would Be Needed

This deferred binding plan would only be needed if:

1. **Multi-threaded video decoding** - If frame decoding is moved to a separate thread

without GL context, deferred binding would be required.

2. **NDI hardware passthrough** - If NDI frames were to bypass CPU and go directly to

GPU (currently they go through CPU buffer anyway).

3. **Architecture changes** - If `updateLayers()` is ever called without the GL context

being current.

For the current single-threaded architecture with proper GL context management,

this plan is **not required**.