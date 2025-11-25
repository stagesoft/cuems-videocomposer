# Multi-Layer Video Playback Fixes

## Issues Found and Fixed

### Issue 1: Static Variable Sharing Between Layers (CRITICAL)
**Location**: `src/cuems_videocomposer/cpp/layer/LayerPlayback.cpp:95-102`

**Problem**: 
- Static variables `wasRolling`, `lastLoggedFrame`, and `debugCounter` were shared across ALL layer instances
- Each layer overwrote the other's MTC sync state
- Caused race conditions and undefined behavior with multiple layers

**Fix**:
- Converted to instance member variables in `LayerPlayback` class
- Added to header: `wasRolling_`, `lastLoggedFrame_`, `debugCounter_`
- Initialized in constructor
- Each layer now has independent MTC sync state

**Files Modified**:
- `src/cuems_videocomposer/cpp/layer/LayerPlayback.h` (lines 97-100)
- `src/cuems_videocomposer/cpp/layer/LayerPlayback.cpp` (lines 12-22, 95-171)

---

### Issue 2: Shared Texture ID Causing Corruption (CRITICAL)
**Location**: `src/cuems_videocomposer/cpp/display/OpenGLRenderer.cpp:372`

**Problem**:
- OpenGLRenderer had a single shared `textureId_` member variable
- When multiple layers used CPU frames (software decoding), they ALL uploaded to the same texture
- Layer 2 would overwrite Layer 1's texture → texture corruption and video stops
- User reported seeing texture corruption before playback stopped

**Symptoms**:
- Texture corruption visible on screen
- Video frames stop updating
- Only works with single layer, fails with multiple layers

**Fix**:
- Create a temporary texture per-layer for CPU-decoded frames
- Use `glGenTextures()` for each layer during rendering
- Delete texture with `glDeleteTextures()` immediately after rendering that layer
- GPU-decoded frames already had separate textures (no issue there)

**Files Modified**:
- `src/cuems_videocomposer/cpp/display/OpenGLRenderer.cpp` (lines 369-456)

**Code Changes**:
```cpp
// OLD CODE (BROKEN):
if (!isOnGPU && cpuBuffer.isValid()) {
    if (!uploadFrameToTexture(cpuBuffer)) {  // Uses shared textureId_!
        return false;
    }
}

// NEW CODE (FIXED):
if (!isOnGPU && cpuBuffer.isValid()) {
    // Create temporary per-layer texture
    GLuint layerTextureId = 0;
    glGenTextures(1, &layerTextureId);
    
    // Upload this layer's frame
    glBindTexture(GL_TEXTURE_RECTANGLE_ARB, layerTextureId);
    // ... texture setup and upload ...
    
    // Render with this layer's texture
    renderQuad(x, y, w, h);
    
    // Cleanup immediately after rendering
    glDeleteTextures(1, &layerTextureId);
}
```

---

## Testing

### Before Fixes:
- ❌ Single layer: OK
- ❌ Multiple layers: Texture corruption → video stops

### After Fixes:
- ✅ Static variable isolation: Each layer has independent state
- ✅ Texture isolation: Each layer gets its own texture
- ✅ Ready for testing with multiple layers

### Test Command:
```bash
cd /home/ion/src/cuems/cuems-videocomposer

# Test with two DIFFERENT video files
python3 tests/test_dynamic_file_management.py \
  --videocomposer build/cuems-videocomposer \
  --video1 video_test_files/test_h264_mp4.mp4 \
  --video2 video_test_files/test_h264_720p.mp4
```

---

## Technical Details

### Why CPU Frames Need Per-Layer Textures

1. **GPU Frames (Hardware Decoded)**: Each layer's `GPUTextureFrameBuffer` already allocates its own texture ID via `glGenTextures()` - no sharing issue

2. **CPU Frames (Software Decoded)**: The old code used a single shared texture in `OpenGLRenderer::textureId_` - this caused the corruption

### Performance Impact

- **Memory**: Minimal - textures are created and destroyed each frame only for CPU-decoded layers
- **GPU**: Minimal - texture creation/deletion is fast compared to decoding
- **Optimization Possible**: Could cache textures per-layer if needed, but current solution is simpler and reliable

---

## Related Code

- `LayerManager::updateAll()`: Calls `update()` on each layer sequentially
- `OpenGLRenderer::compositeLayers()`: Renders layers in z-order
- `OpenGLDisplay::render()`: Makes GL context current, renders all layers, swaps buffers

---

## Date
2025-11-18

## Status
✅ Fixed and compiled successfully
🧪 Ready for user testing

