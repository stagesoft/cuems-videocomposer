# Rendering Optimizations Master Plan

## Target Goals

- 2-4 VAAPI hardware-decoded layers @ 60fps
- 4-6 HAP layers @ 60fps
- Smooth operation with headroom for effects

## Current Performance Baseline

| Component | Current Time | Target | Status |

|-----------|--------------|--------|--------|

| VAAPI decode (per layer) | ~0.3ms | - | ✅ Optimal |

| HAP read + upload (per layer) | ~1-2ms | <0.5ms | ⚠️ Needs work |

| CPU frame upload (per layer) | ~0.5-1ms | <0.3ms | ⚠️ Can improve |

| Layer render (per layer) | ~0.3-0.5ms | - | ✅ OK |

| Compositing | ~1-2ms | <1ms | ⚠️ Can improve |

| **Total 6 layers** | ~10-15ms | <8ms | Target |

---

## Optimization Summary

| # | Optimization | Performance Gain | Complexity | Priority | Dependencies |

|---|--------------|------------------|------------|----------|--------------|

| 1 | HAP Async I/O Pre-buffer | 40-60% for HAP | Medium | ⭐⭐⭐⭐⭐ | None |

| 2 | PBO Double-Buffer | 20-30% upload | Medium | ⭐⭐⭐⭐ | None |

| 3 | Persistent Mapped Buffers | 10-15% upload | Low | ⭐⭐⭐ | #2 |

| 4 | Instanced Rendering | 15-25% render | Medium | ⭐⭐⭐ | None |

| 5 | Texture Array Layers | 10-20% render | Medium | ⭐⭐ | #4 |

| 6 | Uniform Buffer Objects | 5-10% render | Low | ⭐⭐ | None |

| 7 | Compute Shader Composite | 20-40% composite | High | ⭐⭐ | Refactor |

| 8 | Multi-thread SW Decode | 50-70% SW decode | High | ⭐ | Only if needed |

| 9 | Frame Interpolation | Smoothness | Very High | ⭐ | Future |

| 10 | Vulkan Backend | 30-50% overall | Very High | ⭐ | Major refactor |

---

## Optimization 1: HAP Async I/O Pre-buffer

**Status:** Separate plan created (`.cursor/plans/hap-async-prebuffer.plan.md`)

### Summary

- Read upcoming HAP frames in background thread
- Eliminates disk I/O latency from render path
- Critical for 4-6 HAP layer goal

### Metrics

| Metric | Before | After |

|--------|--------|-------|

| HAP frame time | 1-2ms | ~0.1ms (cached) |

| 6-layer HAP total | 6-12ms | ~0.6ms |

| **Improvement** | - | **90%** |

### Complexity: Medium (14-21 hours)

---

## Optimization 2: PBO Double-Buffer

### Problem

Current texture upload is synchronous:

```cpp
// Current: CPU waits for GPU to finish before upload completes
glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, data);
// ↑ Blocks until GPU copies data
```

### Solution

Use Pixel Buffer Objects for async upload:

```cpp
// Frame N: Upload to PBO (CPU side, non-blocking)
glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[writeIndex]);
void* ptr = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, size,
    GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
memcpy(ptr, frameData, size);
glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

// Frame N: GPU reads from OTHER PBO (async DMA)
glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo[readIndex]);
glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
// ↑ Returns immediately, GPU does DMA transfer

// Swap indices for next frame
std::swap(writeIndex, readIndex);
```

### Architecture

```
Frame N:
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│ CPU writes  │────▶│   PBO A     │     │  Texture    │
│ frame N+1   │     │  (staging)  │     │             │
└─────────────┘     └─────────────┘     └─────────────┘
                                              ▲
┌─────────────┐     ┌─────────────┐           │
│ GPU reads   │◀────│   PBO B     │───────────┘
│ frame N     │     │  (ready)    │    DMA transfer
└─────────────┘     └─────────────┘    (async)
```

### Implementation

```cpp
class PBOTextureUploader {
public:
    static constexpr int BUFFER_COUNT = 2;  // Double buffer
    
    void initialize(int width, int height, GLenum format) {
        size_ = width * height * 4;  // BGRA
        glGenBuffers(BUFFER_COUNT, pbos_);
        
        for (int i = 0; i < BUFFER_COUNT; i++) {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos_[i]);
            glBufferData(GL_PIXEL_UNPACK_BUFFER, size_, nullptr, GL_STREAM_DRAW);
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    }
    
    void* mapForWrite() {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos_[writeIndex_]);
        return glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, size_,
            GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    }
    
    void unmapAndUpload(GLuint texture) {
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        
        // Upload from read PBO (previous frame's data)
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbos_[readIndex_]);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_,
                        GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
        
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        
        // Swap for next frame
        std::swap(writeIndex_, readIndex_);
    }
    
private:
    GLuint pbos_[BUFFER_COUNT];
    int writeIndex_ = 0;
    int readIndex_ = 1;
    size_t size_;
    int width_, height_;
};
```

### Metrics

| Metric | Before | After |

|--------|--------|-------|

| CPU→GPU upload time | 0.5-1ms | ~0.1ms (async) |

| CPU blocked time | 0.5-1ms | ~0.05ms |

| **Improvement** | - | **80%** |

### Complexity: Medium (6-8 hours)

### Files to Modify

- `OpenGLRenderer.h/cpp` - Add PBO management
- `LayerTextureCache` - Store PBO pairs per layer

---

## Optimization 3: Persistent Mapped Buffers

### Problem

`glMapBufferRange` still has overhead from map/unmap cycle.

### Solution

Use `GL_MAP_PERSISTENT_BIT` (OpenGL 4.4+):

```cpp
// One-time setup
glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
glBufferStorage(GL_PIXEL_UNPACK_BUFFER, size, nullptr,
    GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
persistentPtr_ = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, size,
    GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);

// Every frame (no map/unmap!)
memcpy(persistentPtr_, frameData, size);
// Use fence to ensure previous upload complete before overwriting
glClientWaitSync(fence_, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
glTexSubImage2D(..., nullptr);
fence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
```

### Metrics

| Metric | Before (PBO) | After (Persistent) |

|--------|--------------|---------------------|

| Map overhead | ~0.05ms | 0 |

| **Additional improvement** | - | **10-15%** |

### Complexity: Low (2-3 hours, builds on #2)

### Requirements

- OpenGL 4.4+ or `GL_ARB_buffer_storage`
- Fallback to regular PBO for older drivers

---

## Optimization 4: Instanced Rendering

### Problem

Current: One draw call per layer

```cpp
for (layer : layers) {
    setUniforms(layer);      // CPU overhead
    glDrawArrays(...);       // Draw call overhead
}
```

### Solution

Single instanced draw for all layers:

```cpp
// Upload all layer transforms to SSBO/UBO
glBindBuffer(GL_SHADER_STORAGE_BUFFER, layerDataSSBO);
glBufferSubData(..., layerTransforms, sizeof(LayerData) * layerCount);

// Single draw call for all layers
glDrawArraysInstanced(GL_TRIANGLE_FAN, 0, 4, layerCount);
```

### Shader Changes

```glsl
// Vertex shader
layout(std430, binding = 0) buffer LayerData {
    mat4 transforms[];
    vec4 params[];  // opacity, blend mode, etc.
};

void main() {
    int layerId = gl_InstanceID;
    mat4 mvp = transforms[layerId];
    // ...
}

// Fragment shader
void main() {
    int layerId = gl_InstanceID;
    float opacity = params[layerId].x;
    // ...
}
```

### Metrics

| Metric | Before | After |

|--------|--------|-------|

| Draw calls (6 layers) | 6 | 1 |

| Uniform updates | 6 | 1 (buffer) |

| CPU overhead | ~0.3ms | ~0.05ms |

| **Improvement** | - | **80% CPU** |

### Complexity: Medium (8-10 hours)

### Challenges

- Different blend modes per layer (need shader branching or multi-pass)
- Different textures per layer (texture arrays or bindless)
- Ordering for alpha blending

---

## Optimization 5: Texture Array Layers

### Problem

With instanced rendering, each layer needs a different texture.

Binding textures per-instance breaks batching.

### Solution

Use `GL_TEXTURE_2D_ARRAY`:

```cpp
// Create texture array (all layers same size)
glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, 
             width, height, MAX_LAYERS, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

// Upload layer N's frame to array slice N
glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layerIndex,
                width, height, 1, GL_BGRA, GL_UNSIGNED_BYTE, data);
```
```glsl
// Shader
uniform sampler2DArray uLayerTextures;

void main() {
    int layerId = gl_InstanceID;
    vec4 color = texture(uLayerTextures, vec3(vTexCoord, float(layerId)));
}
```

### Metrics

| Metric | Before | After |

|--------|--------|-------|

| Texture binds (6 layers) | 6 | 1 |

| **Additional improvement** | - | **10-20%** |

### Complexity: Medium (4-6 hours, builds on #4)

### Limitations

- All layers must be same resolution (or use max + UV scaling)
- Different formats need separate arrays

---

## Optimization 6: Uniform Buffer Objects (UBO)

### Problem

Setting uniforms individually has overhead:

```cpp
glUniform1f(loc1, opacity);
glUniform1i(loc2, blendMode);
glUniformMatrix4fv(loc3, 1, GL_FALSE, mvp);
// ... 10+ uniform calls per layer
```

### Solution

Pack uniforms into UBO:

```cpp
struct LayerUniforms {
    float mvp[16];
    float opacity;
    int blendMode;
    float colorCorrection[5];
    float padding[2];  // Align to 16 bytes
};

// One buffer update instead of many uniform calls
glBindBuffer(GL_UNIFORM_BUFFER, ubo);
glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(LayerUniforms), &uniforms);
```

### Metrics

| Metric | Before | After |

|--------|--------|-------|

| API calls per layer | ~15 | 1-2 |

| **Improvement** | - | **5-10%** |

### Complexity: Low (3-4 hours)

---

## Optimization 7: Compute Shader Composite

### Problem

Current fragment shader compositing has fixed pipeline overhead.

### Solution

Use compute shader for compositing:

```glsl
#version 430 core
layout(local_size_x = 16, local_size_y = 16) in;

layout(binding = 0, rgba8) uniform image2D outputImage;
layout(binding = 1) uniform sampler2DArray layerTextures;

layout(std430, binding = 0) buffer LayerData {
    LayerParams layers[];
};

uniform int layerCount;

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    vec2 uv = vec2(pixel) / vec2(imageSize(outputImage));
    
    vec4 result = vec4(0.0);
    
    for (int i = 0; i < layerCount; i++) {
        vec2 layerUV = transformUV(uv, layers[i]);
        vec4 layerColor = texture(layerTextures, vec3(layerUV, float(i)));
        
        // Apply blend mode
        result = blend(result, layerColor, layers[i].blendMode, layers[i].opacity);
    }
    
    imageStore(outputImage, pixel, result);
}
```

### Metrics

| Metric | Before | After |

|--------|--------|-------|

| Composite time | 1-2ms | 0.5-1ms |

| Memory bandwidth | Higher (multiple passes) | Lower (single pass) |

| **Improvement** | - | **30-50%** |

### Complexity: High (15-20 hours)

### Challenges

- Complete shader rewrite
- Blend mode complexity
- Corner deformation in compute

---

## Optimization 8: Multi-threaded Software Decode

### Problem

Software decoding is CPU-bound and blocks main thread.

### Solution

Decode pool with frame queue:

```cpp
class DecodeThreadPool {
    std::vector<std::thread> workers_;
    std::queue<DecodeTask> taskQueue_;
    std::unordered_map<int64_t, DecodedFrame> frameCache_;
    
    void workerLoop() {
        while (running_) {
            DecodeTask task = dequeue();
            
            // Each worker has its own FFmpeg decoder context
            AVFrame* frame = decoder_->decode(task.frameNumber);
            
            // Convert to RGBA
            FrameBuffer buffer = convertToRGBA(frame);
            
            // Store in cache
            frameCache_[task.frameNumber] = std::move(buffer);
        }
    }
};
```

### Metrics

| Metric | Before | After |

|--------|--------|-------|

| SW decode time (1080p H.264) | 8-12ms | <2ms (parallel) |

| CPU utilization | 1 core | 4+ cores |

| **Improvement** | - | **70-80%** |

### Complexity: High (20-30 hours)

### When Needed

- Only if VAAPI unavailable for target codec
- ProRes/DNxHD without hardware support
- Fallback scenarios

---

## Optimization 9: Frame Interpolation

### Problem

30fps source on 60fps display shows duplicate frames.

### Solution

Motion-compensated frame interpolation:

```
Source:    [F0]----[F1]----[F2]----[F3]
Display:   [F0][I0][F1][I1][F2][I2][F3]
                ↑      ↑      ↑
            Interpolated frames
```

### Approaches

1. **Simple blend** - Mix adjacent frames (ghosting)
2. **Motion vectors** - Use codec MVs for warping
3. **Optical flow** - GPU-computed flow (best quality, expensive)

### Complexity: Very High (40+ hours)

### Priority: Low (nice-to-have, not critical)

---

## Optimization 10: Vulkan Backend

### Problem

OpenGL has driver overhead and limited parallelism.

### Solution

Vulkan backend for maximum performance:

- Explicit multi-threading
- Pre-recorded command buffers
- Better memory control
- Lower driver overhead

### Metrics

| Metric | OpenGL | Vulkan |

|--------|--------|--------|

| Driver overhead | Higher | Lower |

| CPU parallelism | Limited | Full |

| **Improvement** | - | **30-50%** |

### Complexity: Very High (100+ hours - major refactor)

### Priority: Low (diminishing returns vs effort)

---

## Implementation Roadmap

### Phase 1: Critical Path (Week 1-2)

| Optimization | Hours | Impact |

|--------------|-------|--------|

| #1 HAP Async I/O | 14-21 | High |

| **Total** | 14-21 | - |

### Phase 2: Quick Wins (Week 3)

| Optimization | Hours | Impact |

|--------------|-------|--------|

| #2 PBO Double-Buffer | 6-8 | Medium |

| #6 UBO | 3-4 | Low |

| **Total** | 9-12 | - |

### Phase 3: Advanced (Week 4-5)

| Optimization | Hours | Impact |

|--------------|-------|--------|

| #3 Persistent Mapped | 2-3 | Low |

| #4 Instanced Rendering | 8-10 | Medium |

| **Total** | 10-13 | - |

### Phase 4: Optional (Future)

| Optimization | Hours | Impact |

|--------------|-------|--------|

| #5 Texture Arrays | 4-6 | Low |

| #7 Compute Composite | 15-20 | Medium |

| #8 MT SW Decode | 20-30 | Situational |

---

## Cumulative Performance Projection

### 6 HAP Layers @ 1080p

| Stage | Frame Time | Improvement |

|-------|------------|-------------|

| Current | 10-15ms | Baseline |

| + HAP Async I/O (#1) | 4-6ms | **60%** |

| + PBO (#2) | 3-5ms | **70%** |

| + Instanced (#4) | 2.5-4ms | **75%** |

| + Compute (#7) | 2-3ms | **80%** |

### 4 VAAPI Layers @ 1080p

| Stage | Frame Time | Improvement |

|-------|------------|-------------|

| Current | 3-4ms | Baseline (already good) |

| + PBO (#2) | 2.5-3.5ms | **15%** |

| + Instanced (#4) | 2-3ms | **25%** |

---

## Hardware Requirements

| Optimization | Minimum OpenGL | GPU Requirement |

|--------------|----------------|-----------------|

| #1 HAP Async | Any | None (CPU/IO) |

| #2 PBO | 2.1 | Any |

| #3 Persistent | 4.4 | Modern |

| #4 Instanced | 3.1 | Any |

| #5 Tex Arrays | 3.0 | Any |

| #6 UBO | 3.1 | Any |

| #7 Compute | 4.3 | Modern |

---

## Recommendation

### Must Have (for your goals)

1. ✅ **HAP Async I/O** - Critical for 6 HAP layers
2. ✅ **PBO Double-Buffer** - General improvement, easy win

### Should Have

3. ⚠️ **Instanced Rendering** - Good for 4+ layers
4. ⚠️ **UBO** - Easy, small gain

### Nice to Have

5. ❓ **Persistent Mapped** - Small gain, needs GL 4.4
6. ❓ **Texture Arrays** - Only with instancing

### Probably Not Needed

7. ❌ **Compute Composite** - High effort, VAAPI already fast
8. ❌ **MT SW Decode** - Only if SW fallback common
9. ❌ **Vulkan** - Massive effort, overkill

---

## Quick Start

Implement in this order for best ROI:

```
Week 1-2:  HAP Async I/O     [14-21h] → 60% improvement for HAP
Week 3:    PBO Double-Buffer [6-8h]  → 20% improvement general
Week 4:    Instanced Render  [8-10h] → 15% improvement render
           ─────────────────────────────────────────────────
           Total: 28-39 hours → 75%+ improvement
```