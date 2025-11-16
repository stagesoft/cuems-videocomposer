# Refined Plan: Split VideoLayer with GPU Hardware Decoding, HAP Support, and Hybrid Processing

## HAP Codec Characteristics

**HAP (Hardware Accelerated Performance)** is a codec specifically designed for VJ/live performance applications:

1. **GPU-Optimized**: Designed for direct GPU decoding and OpenGL texture upload
2. **Zero-Copy**: Can be decoded directly to OpenGL textures (no CPU→GPU transfer)
3. **Variants**:
   - **HAP**: Standard version
   - **HAP Q**: Higher quality variant
   - **HAP Alpha**: With alpha channel support
4. **FFmpeg Support**: FFmpeg has HAP decoder support
5. **Performance**: Optimized for multiple concurrent streams
6. **Texture Format**: Decodes directly to DXT1/DXT5 texture formats (OpenGL compatible)

## Architecture Decision

**GPU-First with HAP Optimization**:
- HAP codec: Direct GPU texture decoding (zero-copy, optimal)
- Hardware-accelerated codecs (H.264, HEVC, AV1): GPU hardware decoding
- Software codecs: CPU decoding with GPU upload
- All image modifications: GPU-side preferred, CPU fallback when needed

## Implementation Plan

### Phase 1: Codec Detection and HAP Support

1. **Extend `InputSource` interface** for codec-specific handling
   - Add `enum class CodecType { HAP, HAP_Q, HAP_ALPHA, H264, HEVC, AV1, SOFTWARE }`
   - Add `CodecType detectCodec() const` - detect codec from file
   - Add `bool supportsDirectGPUTexture() const` - HAP can decode to texture directly
   - Add `DecodeBackend getOptimalBackend() const` - auto-select best backend

2. **Create `HAPVideoInput` class** (`src/cuems_videocomposer/cpp/input/HAPVideoInput.h/cpp`)
   - Specialized InputSource for HAP codec
   - Uses FFmpeg HAP decoder
   - **Key Feature**: Decodes directly to OpenGL texture (DXT1/DXT5)
   - Methods:
     - `open()` - detect HAP variant (HAP, HAP Q, HAP Alpha)
     - `readFrameToTexture()` - decode directly to OpenGL texture
     - `getTextureFormat()` - returns GL_COMPRESSED_RGB_S3TC_DXT1_EXT or GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
   - **No CPU FrameBuffer needed** - frame stays on GPU as texture

3. **Extend `VideoFileInput` for HAP detection**
   - Detect HAP codec in `openCodec()`
   - If HAP detected: delegate to `HAPVideoInput` or use HAP-specific path
   - If other codec: use existing hardware/software decoding

4. **Create `GPUTextureFrameBuffer` with HAP support**
   - Support DXT1/DXT5 compressed textures (HAP format)
   - Support uncompressed textures (hardware decoded formats)
   - Methods:
     - `bool isHAPTexture() const`
     - `GLenum getTextureFormat() const` - DXT1, DXT5, or uncompressed
     - `unsigned int getTextureId() const`

### Phase 2: GPU Hardware Decoding (Non-HAP Codecs)

5. **Implement hardware decoding in `VideoFileInput`**
   - Detect available hardware decoders (VAAPI, CUDA, VideoToolbox, DXVA2)
   - Use FFmpeg hardware acceleration APIs
   - When hardware decoding: return GPU texture (via `GPUTextureFrameBuffer`)
   - When software decoding: return CPU `FrameBuffer` (current behavior)

6. **Codec-specific optimization strategy**:
   ```
   HAP codec → Direct GPU texture (DXT1/DXT5) - ZERO COPY
   Hardware H.264/HEVC/AV1 → GPU hardware decoder → GPU texture - ZERO COPY
   Software codecs → CPU decode → GPU upload - ONE COPY
   ```

### Phase 3: Create LayerPlayback Component

7. **Create `LayerPlayback` class**
   - Handles sync and frame loading
   - Manages both CPU and GPU FrameBuffers
   - **Codec-aware**: Uses optimal InputSource (HAPVideoInput for HAP, VideoFileInput for others)
   - **Note**: Renamed from VideoPlayback to LayerPlayback for clarity
   - Methods:
     - `setInputSource()` - can accept HAPVideoInput or VideoFileInput
     - `getFrameBuffer()` - returns FrameBuffer (CPU or GPU)
     - `bool isFrameOnGPU() const`
     - `bool isHAPCodec() const` - check if current source is HAP

### Phase 4: Create LayerDisplay with Hybrid Processing

8. **Create `ImageProcessor` interface** (same as before)
   - GPU-first, CPU fallback architecture

9. **Create `GPUImageProcessor` with HAP optimization**
   - **HAP textures**: Use directly (already compressed, no processing needed)
   - **Other GPU textures**: Apply transforms via texture coordinates
   - **CPU buffers**: Upload to texture, then apply transforms
   - Special handling for DXT1/DXT5 textures (HAP format)

10. **Create `CPUImageProcessor`** (fallback only)
    - Only used when GPU cannot do operation
    - **Note**: For HAP, CPU processing should be avoided (defeats purpose)

11. **Create `LayerDisplay` class**
    - Orchestrates GPU-first processing
    - Special optimization: If HAP texture, skip most processing (already optimal)
    - **Note**: Renamed from VideoDisplay to LayerDisplay for clarity

### Phase 5: OpenGLRenderer HAP Support

12. **Update `OpenGLRenderer` for HAP textures**
    - Detect HAP texture format (DXT1/DXT5)
    - Use compressed texture directly (no upload needed)
    - Support both compressed (HAP) and uncompressed textures
    - Methods:
      - `bool uploadHAPTexture(const GPUTextureFrameBuffer& hapFrame)` - use existing texture
      - `bool uploadUncompressedTexture(const FrameBuffer& frame)` - upload CPU buffer

13. **Texture format handling**:
    ```cpp
    if (frameBuffer.isHAPTexture()) {
        // HAP: Use texture directly, already on GPU as DXT1/DXT5
        glBindTexture(GL_TEXTURE_2D, frameBuffer.getTextureId());
        // No upload needed!
    } else if (frameBuffer.isGPUTexture()) {
        // Hardware decoded: Use GPU texture directly
        glBindTexture(GL_TEXTURE_2D, frameBuffer.getTextureId());
    } else {
        // Software decoded: Upload CPU buffer to GPU
        uploadFrameToTexture(frameBuffer);
    }
    ```

### Phase 6: Integration and Optimization

14. **Update `VideoComposerApplication::createInitialLayer()`**
    - Detect codec type
    - If HAP: Create `HAPVideoInput`
    - If hardware-accelerated: Use hardware decoding
    - If software: Use software decoding
    - Wrap with appropriate components

15. **Performance optimization for HAP**:
    - HAP frames: Zero CPU→GPU transfer
    - HAP textures: Already compressed (DXT1/DXT5), efficient GPU memory
    - HAP decoding: GPU-accelerated via FFmpeg
    - **Result**: HAP is the fastest path for multiple concurrent streams

## HAP-Specific Implementation Details

### HAP Decoding Path

```
HAP File → FFmpeg HAP Decoder → DXT1/DXT5 Compressed Texture → OpenGL (direct use)
```

**Advantages**:
- No CPU decoding
- No CPU→GPU transfer
- Compressed texture format (memory efficient)
- Direct GPU rendering

### HAP Variant Detection

```cpp
enum class HAPVariant {
    HAP,        // Standard HAP (DXT1)
    HAP_Q,      // Higher quality (DXT5)
    HAP_ALPHA   // With alpha channel (DXT5)
};

HAPVariant detectHAPVariant(AVCodecContext* codecCtx);
```

### FFmpeg HAP Integration

- FFmpeg has HAP decoder: `avcodec_find_decoder(AV_CODEC_ID_HAP)`
- HAP frames decode to DXT1/DXT5 format
- Can extract texture data directly from decoded frame
- Map to OpenGL texture format:
  - DXT1 → `GL_COMPRESSED_RGB_S3TC_DXT1_EXT`
  - DXT5 → `GL_COMPRESSED_RGBA_S3TC_DXT5_EXT`

## Performance Comparison

### HAP Codec (Optimal Path)
- **Decoding**: GPU-accelerated via FFmpeg (~1-2ms per frame)
- **CPU→GPU Transfer**: **ZERO** (stays on GPU)
- **Memory**: Compressed texture (DXT1/DXT5) - efficient
- **50 layers**: ~50-100ms total = **10-20 FPS**

### Hardware H.264/HEVC/AV1
- **Decoding**: GPU hardware decoder (~2-5ms per frame)
- **CPU→GPU Transfer**: **ZERO** (stays on GPU)
- **Memory**: Uncompressed texture - larger
- **50 layers**: ~100-250ms total = **4-10 FPS**

### Software Codecs
- **Decoding**: CPU (~5-15ms per frame)
- **CPU→GPU Transfer**: ~2-5ms per frame
- **Memory**: CPU buffer + GPU texture
- **50 layers**: ~350-1000ms total = **1-3 FPS**

## File Changes

### New Files
- `src/cuems_videocomposer/cpp/input/HAPVideoInput.h/cpp` - HAP-specific input
- `src/cuems_videocomposer/cpp/layer/LayerPlayback.h/cpp`
- `src/cuems_videocomposer/cpp/layer/LayerDisplay.h/cpp`
- `src/cuems_videocomposer/cpp/layer/RenderParameters.h`
- `src/cuems_videocomposer/cpp/display/ImageProcessor.h`
- `src/cuems_videocomposer/cpp/display/GPUImageProcessor.h/cpp`
- `src/cuems_videocomposer/cpp/display/CPUImageProcessor.h/cpp`
- `src/cuems_videocomposer/cpp/video/GPUTextureFrameBuffer.h/cpp`

### Modified Files
- `src/cuems_videocomposer/cpp/input/InputSource.h` - add codec detection, HAP support
- `src/cuems_videocomposer/cpp/input/VideoFileInput.h/cpp` - HAP detection, hardware decoding
- `src/cuems_videocomposer/cpp/video/FrameBuffer.h/cpp` - support GPU textures, HAP
- `src/cuems_videocomposer/cpp/layer/VideoLayer.h/cpp` - compose components
- `src/cuems_videocomposer/cpp/display/OpenGLRenderer.h/cpp` - HAP texture support
- `CMakeLists.txt` - add new source files

## Key Design Principles

1. **HAP-First Optimization**: HAP gets the fastest path (zero-copy, direct texture)
2. **Codec-Aware Routing**: Different codecs use optimal decoding paths
3. **GPU-First Processing**: All modifications GPU-side when possible
4. **Zero-Copy When Possible**: HAP and hardware-decoded frames stay on GPU
5. **Hybrid Fallback**: CPU processing only when GPU cannot do operation
6. **Multiple Concurrent Streams**: Each layer decodes independently

## HAP Implementation Strategy

### Detection
```cpp
bool isHAPCodec(AVCodecContext* codecCtx) {
    return codecCtx->codec_id == AV_CODEC_ID_HAP ||
           codecCtx->codec_id == AV_CODEC_ID_HAPQ ||
           codecCtx->codec_id == AV_CODEC_ID_HAPALPHA;
}
```

### Decoding
```cpp
// HAP decodes to DXT1/DXT5 compressed texture data
// Extract from AVFrame and create OpenGL texture directly
GLuint createHAPTexture(AVFrame* frame, HAPVariant variant) {
    // frame->data[0] contains DXT1/DXT5 compressed data
    // Upload directly to OpenGL compressed texture
    glCompressedTexImage2D(GL_TEXTURE_2D, 0, 
                          variant == HAP_ALPHA ? GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 
                                               : GL_COMPRESSED_RGB_S3TC_DXT1_EXT,
                          width, height, 0, size, frame->data[0]);
}
```

### Integration
- HAP files automatically use `HAPVideoInput`
- Other files use `VideoFileInput` (with hardware decoding if available)
- Both return `FrameBuffer` (can be CPU or GPU)
- `LayerDisplay` handles both transparently

## Performance Targets

- **HAP (50 layers)**: 10-20 FPS (optimal)
- **Hardware H.264/HEVC (50 layers)**: 4-10 FPS
- **Software (50 layers)**: 1-3 FPS

HAP provides the best performance for multiple concurrent streams.

