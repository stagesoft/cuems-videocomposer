# VAAPI Zero-Copy Hardware Decoding Implementation Plan

## Implementation Status: ✅ COMPLETED (UNTESTED)

**Implemented:** 2024-11-26
**Status:** Code complete, compiles, but **NOT TESTED** on Intel/AMD hardware
**Reason:** Development machine has NVIDIA GPU only (GeForce GTX 970) - VAAPI requires Intel or AMD GPU

---

## Overview

Implement VAAPI (Video Acceleration API) zero-copy decoding for Intel/AMD GPUs, allowing decoded video frames to remain on the GPU without CPU memory transfers. This enables efficient multi-layer 4K playback on Intel N100/N101 and Core i5 systems.

## Implementation Summary

### What Was Implemented

#### Phase 1: EGL Context Setup ✅
- **Files Modified:**
  - `src/cuems_videocomposer/cpp/display/OpenGLDisplay.h`
  - `src/cuems_videocomposer/cpp/display/OpenGLDisplay.cpp`
- **Changes:**
  - Added EGL initialization alongside GLX context
  - Query for required extensions (`EGL_EXT_image_dma_buf_import`, `EGL_KHR_image_base`)
  - Store extension function pointers for DMA-BUF import
  - Added `hasVaapiSupport()`, `getEGLDisplay()`, and extension getter methods

#### Phase 2: VaapiInterop Class ✅
- **New Files:**
  - `src/cuems_videocomposer/cpp/hwdec/VaapiInterop.h`
  - `src/cuems_videocomposer/cpp/hwdec/VaapiInterop.cpp`
- **Features:**
  - VAAPI Surface → DRM PRIME FD → EGL Image → OpenGL Texture pipeline
  - `importFrame()` - imports VAAPI frame to OpenGL textures (zero-copy)
  - `releaseFrame()` - releases EGL image resources
  - Handles NV12 format (2 planes: Y + UV)

#### Phase 3: VideoFileInput Integration ✅
- **Files Modified:**
  - `src/cuems_videocomposer/cpp/input/VideoFileInput.h`
  - `src/cuems_videocomposer/cpp/input/VideoFileInput.cpp`
  - `src/cuems_videocomposer/cpp/video/GPUTextureFrameBuffer.h`
  - `src/cuems_videocomposer/cpp/video/GPUTextureFrameBuffer.cpp`
- **Changes:**
  - Added `setVaapiInterop()` method to VideoFileInput
  - Modified `transferHardwareFrameToGPU()` to use zero-copy when `AV_PIX_FMT_VAAPI`
  - Added `setExternalNV12Textures()` to GPUTextureFrameBuffer for external texture references
  - Automatic fallback to CPU copy path if zero-copy fails

#### Phase 4: Application Wiring ✅
- **Files Modified:**
  - `src/cuems_videocomposer/cpp/VideoComposerApplication.h`
  - `src/cuems_videocomposer/cpp/VideoComposerApplication.cpp`
  - `CMakeLists.txt`
- **Changes:**
  - VaapiInterop created and initialized in `initializeDisplay()`
  - Automatically passed to all VideoFileInput instances
  - CMake detects and links VAAPI/EGL/DRM libraries

### Build Configuration

Dependencies installed via:
```bash
sudo apt install libva-dev libva-drm2 libdrm-dev libegl-dev
```

CMake automatically enables VAAPI interop when dependencies are found:
```
-- VAAPI zero-copy interop enabled (EGL + VAAPI + DRM)
```

### Runtime Behavior

On startup, the application logs:
```
[INFO] EGL initialized: version 1.5
[INFO] EGL extensions available: EGL_EXT_image_dma_buf_import, EGL_KHR_image_base
[INFO] EGL extension functions loaded successfully
[INFO] VAAPI zero-copy interop enabled
[INFO] VaapiInterop initialized successfully
[INFO] VaapiInterop initialized - VAAPI zero-copy enabled for video playback
```

### Zero-Copy Activation Conditions

The VAAPI zero-copy path activates when ALL of these conditions are met:
1. Hardware decoder type is VAAPI (`hwDecoderType_ == HardwareDecoder::Type::VAAPI`)
2. Frame format is `AV_PIX_FMT_VAAPI`
3. VaapiInterop is initialized and available
4. System has Intel or AMD GPU with VAAPI support

### Fallback Behavior

If zero-copy fails or is unavailable:
1. Falls back to NV12 direct upload (skips sws_scale - still faster than old path)
2. If NV12 not available, falls back to RGBA via sws_scale (slowest path)

---

## Testing Required

### ⚠️ UNTESTED - Requires Intel/AMD Hardware

The implementation needs testing on:
- [ ] Intel integrated GPU (N100, N101, Core i5, etc.)
- [ ] AMD APU or discrete GPU with VAAPI support

### Test Commands (for Intel/AMD systems)

```bash
# Test VAAPI hardware decoding with zero-copy
./cuems-videocomposer --hw-decode vaapi path/to/video.mp4

# Verify zero-copy is working (look for these log messages):
# [VERBOSE] transferHardwareFrameToGPU: VAAPI zero-copy import successful 1920x1080
# [VERBOSE] VaapiInterop: Imported frame 1920x1080 (Y=X, UV=Y)
```

### Expected Performance (theoretical)

| Metric | Without Zero-Copy | With Zero-Copy |
|--------|------------------|----------------|
| Memory copies | 3 (decode→CPU→convert→GPU) | 0 |
| CPU usage (4K) | ~60% | ~10% |
| Memory bandwidth | ~3 GB/s | ~100 MB/s |
| Max 1080p layers | 2-3 | 8-10 |
| Latency | ~30ms | ~5ms |

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                     VAAPI Zero-Copy Pipeline                     │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  FFmpeg VAAPI Decoder                                           │
│         │                                                        │
│         ▼                                                        │
│  AVFrame (AV_PIX_FMT_VAAPI)                                     │
│         │                                                        │
│         ▼                                                        │
│  VASurface → vaExportSurfaceHandle() → DRM PRIME FD             │
│         │                                                        │
│         ▼                                                        │
│  DMA-BUF File Descriptor                                        │
│         │                                                        │
│         ▼                                                        │
│  eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)                       │
│         │                                                        │
│         ▼                                                        │
│  EGLImage (per plane: Y, UV for NV12)                           │
│         │                                                        │
│         ▼                                                        │
│  glEGLImageTargetTexture2DOES() → GL_TEXTURE_2D                 │
│         │                                                        │
│         ▼                                                        │
│  NV12 Shader (YUV→RGB conversion on GPU)                        │
│         │                                                        │
│         ▼                                                        │
│  Rendered Frame (with warping, blending, etc.)                  │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Files Created/Modified

### New Files
- `src/cuems_videocomposer/cpp/hwdec/VaapiInterop.h`
- `src/cuems_videocomposer/cpp/hwdec/VaapiInterop.cpp`

### Modified Files
- `CMakeLists.txt` - VAAPI/EGL/DRM detection and linking
- `src/cuems_videocomposer/cpp/display/OpenGLDisplay.h` - EGL context members
- `src/cuems_videocomposer/cpp/display/OpenGLDisplay.cpp` - EGL initialization
- `src/cuems_videocomposer/cpp/input/VideoFileInput.h` - VaapiInterop integration
- `src/cuems_videocomposer/cpp/input/VideoFileInput.cpp` - Zero-copy path in transferHardwareFrameToGPU()
- `src/cuems_videocomposer/cpp/video/GPUTextureFrameBuffer.h` - setExternalNV12Textures()
- `src/cuems_videocomposer/cpp/video/GPUTextureFrameBuffer.cpp` - External texture support
- `src/cuems_videocomposer/cpp/VideoComposerApplication.h` - VaapiInterop member
- `src/cuems_videocomposer/cpp/VideoComposerApplication.cpp` - VaapiInterop initialization and wiring

---

## Related Issues

### CUDA Hardware Decoding Performance (Deferred)
- CUDA hardware decoding (NVIDIA) still requires GPU→CPU→GPU copy
- True zero-copy for CUDA would require CUDA-OpenGL interop (cuGraphicsGLRegisterImage)
- This is a separate implementation effort from VAAPI

---

## References

- mpv VAAPI interop: `/home/ion/src/mpv/video/out/hwdec/dmabuf_interop_gl.c`
- FFmpeg VAAPI docs: https://trac.ffmpeg.org/wiki/Hardware/VAAPI
- EGL DMA-BUF extension: https://registry.khronos.org/EGL/extensions/EXT/EGL_EXT_image_dma_buf_import.txt
- Intel VA-API driver: https://github.com/intel/libva
