# Zero-Copy GPU Interop Evaluation for QSV Hardware Decoding

## Executive Summary

**Recommendation: NOT CONVENIENT at this time**

The implementation complexity significantly outweighs the performance benefits for the current use case. The software fallback path works well, and zero-copy would require substantial platform-specific code with limited real-world performance gains.

---

## Current Situation

### What Works
- ✅ QSV hardware decoder detection and initialization
- ✅ Hardware decoder opens successfully for compatible formats
- ✅ Graceful fallback to software decoding when QSV transfer fails
- ✅ Software decoding path is fully functional

### What Doesn't Work
- ❌ `av_hwframe_transfer_data()` fails on Linux with QSV ("Function not implemented")
- ❌ This is a known QSV limitation on Linux - QSV doesn't support CPU frame transfer

### Current Workflow (Software Fallback)
```
QSV Hardware Decoder → [Transfer Fails] → Software Decoder → CPU Frame → GPU Upload
```

---

## Zero-Copy Implementation Options

### Option 1: VAAPI Interop (Recommended if implementing)
**Path**: QSV → VAAPI Surface → EGL/GLX Import → OpenGL Texture

**Requirements**:
- FFmpeg must be built with VAAPI support
- EGL or GLX extensions for VAAPI surface import
- Platform-specific interop code (Linux only)

**Complexity**: ⭐⭐⭐⭐ (High)
- Need to handle VAAPI surface creation/management
- EGL/GLX interop code (platform-specific)
- Surface format conversion (NV12 → RGBA)
- Synchronization between QSV decoder and OpenGL

**Performance Gain**: ⭐⭐⭐ (Moderate)
- Eliminates CPU transfer (~5-10ms per frame)
- Reduces CPU usage (~10-20% per stream)
- Lower latency for high-frame-rate content

**Code Changes Required**:
```cpp
// New dependencies
#include <va/va.h>
#include <va/va_glx.h>  // or va/va_egl.h
#include <GL/glx.h>     // or EGL headers

// New methods in VideoFileInput
bool createVAAPISurface();
bool importVAAPISurfaceToTexture(GPUTextureFrameBuffer& textureBuffer);
void releaseVAAPISurface();
```

**Estimated Implementation Time**: 2-3 weeks
- Research VAAPI interop APIs
- Implement surface management
- Handle format conversion
- Testing on different hardware/drivers
- Error handling and fallbacks

---

### Option 2: Direct EGL/GLX Texture Import
**Path**: QSV Hardware Frame → EGL/GLX Extension → OpenGL Texture

**Requirements**:
- EGL/GLX extensions for hardware surface import
- Direct access to QSV surface handles
- Platform-specific code

**Complexity**: ⭐⭐⭐⭐⭐ (Very High)
- Requires deep knowledge of QSV internals
- Platform-specific extensions (may not exist)
- Driver-dependent behavior
- Limited documentation

**Performance Gain**: ⭐⭐⭐⭐ (Good)
- True zero-copy (no intermediate transfers)
- Best possible performance

**Feasibility**: ⭐⭐ (Low)
- QSV on Linux has limited interop support
- May require Intel-specific extensions
- Not well-documented

**Estimated Implementation Time**: 4-6 weeks (if feasible)

---

### Option 3: Use VAAPI Directly Instead of QSV
**Path**: VAAPI Hardware Decoder → VAAPI Surface → EGL/GLX Import → OpenGL Texture

**Requirements**:
- FFmpeg built with VAAPI
- EGL/GLX VAAPI interop extensions
- VAAPI hardware support

**Complexity**: ⭐⭐⭐ (Moderate)
- VAAPI has better Linux support than QSV
- More documentation available
- Standard interop path

**Performance Gain**: ⭐⭐⭐⭐ (Good)
- Similar to Option 1, but more reliable

**Trade-offs**:
- Lose QSV-specific optimizations
- VAAPI may be slower than QSV on Intel hardware
- But VAAPI works more reliably on Linux

**Estimated Implementation Time**: 1-2 weeks

---

## Performance Analysis

### Current Software Fallback Performance
- **CPU Transfer Time**: ~2-5ms per frame (1920x1080)
- **GPU Upload Time**: ~1-3ms per frame
- **Total Overhead**: ~3-8ms per frame
- **CPU Usage**: ~15-25% per 1080p stream

### Zero-Copy Performance (Estimated)
- **Transfer Time**: 0ms (zero-copy)
- **GPU Upload Time**: 0ms (direct texture import)
- **Total Overhead**: ~0.5-1ms (synchronization only)
- **CPU Usage**: ~5-10% per 1080p stream

### Real-World Impact
- **Single Stream**: Minimal difference (software is fast enough)
- **Multiple Concurrent Streams**: Moderate benefit (~20-30% CPU reduction)
- **High Frame Rate (60fps+)**: Noticeable benefit (lower latency)
- **4K Content**: Moderate benefit (larger transfers)

---

## Complexity vs Benefit Matrix

| Option | Complexity | Performance Gain | Maintenance Burden | Recommendation |
|--------|-----------|------------------|-------------------|----------------|
| **Current (Software Fallback)** | ⭐ Low | ⭐ Baseline | ⭐ Low | ✅ **Keep** |
| **VAAPI Interop** | ⭐⭐⭐⭐ High | ⭐⭐⭐ Moderate | ⭐⭐⭐ Medium | ⚠️ Consider if needed |
| **Direct EGL/GLX Import** | ⭐⭐⭐⭐⭐ Very High | ⭐⭐⭐⭐ Good | ⭐⭐⭐⭐ High | ❌ Not recommended |
| **Use VAAPI Directly** | ⭐⭐⭐ Moderate | ⭐⭐⭐ Moderate | ⭐⭐ Low | ⚠️ Consider as alternative |

---

## Recommendations

### Short Term (Current)
✅ **Keep software fallback** - It works well and is maintainable

### Medium Term (If Performance Becomes Issue)
1. **Profile first**: Measure actual CPU usage with multiple concurrent streams
2. **If CPU is bottleneck**: Consider Option 3 (VAAPI directly) before Option 1
3. **If QSV is required**: Implement Option 1 (VAAPI interop) only if:
   - Multiple concurrent 4K streams are needed
   - CPU usage exceeds 80% with software decoding
   - Real-world profiling shows significant benefit

### Long Term (Future Considerations)
- Monitor FFmpeg/QSV development for better Linux support
- Consider Vulkan interop (if moving to Vulkan renderer)
- Evaluate Intel oneAPI Video Processing Library (oneVPL) as alternative

---

## Implementation Checklist (If Proceeding)

### Prerequisites
- [ ] FFmpeg built with VAAPI support
- [ ] EGL or GLX with VAAPI extensions available
- [ ] Test hardware with VAAPI support
- [ ] Performance profiling shows CPU bottleneck

### Development Tasks
- [ ] Research VAAPI interop APIs and examples
- [ ] Implement VAAPI surface creation/management
- [ ] Implement EGL/GLX surface import
- [ ] Handle format conversion (NV12 → RGBA)
- [ ] Add error handling and fallbacks
- [ ] Test on multiple hardware configurations
- [ ] Performance benchmarking vs software path
- [ ] Documentation and code comments

### Testing Requirements
- [ ] Test on Intel integrated graphics
- [ ] Test on Intel discrete graphics
- [ ] Test on AMD GPUs (VAAPI)
- [ ] Test with different video formats/resolutions
- [ ] Test with multiple concurrent streams
- [ ] Verify fallback to software on failure

---

## Conclusion

**For the current use case, zero-copy GPU interop is NOT CONVENIENT because:**

1. **Software fallback works well**: Current performance is acceptable for most scenarios
2. **High complexity**: Requires significant platform-specific code and maintenance
3. **Limited benefit**: Real-world performance gains are moderate, not transformative
4. **Better alternatives exist**: Using VAAPI directly (Option 3) is simpler and more reliable

**However, consider implementing if:**
- Profiling shows CPU is a bottleneck with multiple concurrent streams
- 4K content at high frame rates is a primary use case
- You have dedicated time for platform-specific interop development
- You can maintain and test on multiple hardware configurations

**Recommended approach**: Monitor performance in production, and implement zero-copy only if profiling demonstrates a clear need.

