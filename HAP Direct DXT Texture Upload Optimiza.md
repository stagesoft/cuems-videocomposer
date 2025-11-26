# HAP Direct DXT Texture Upload Optimization

## Problem

Current HAP flow is inefficient:

1. FFmpeg reads HAP packet (compressed DXT in Snappy container)
2. FFmpeg HAP decoder decompresses DXT -> RGBA on CPU
3. Copy RGBA to CPU FrameBuffer (4 bytes/pixel)
4. Upload RGBA to GPU texture
5. Render

**Issues:**

- 4-8x more CPU->GPU bandwidth than necessary
- CPU cycles wasted on DXT decompression
- Poor multi-layer scaling (critical for N100/N101)

## Solution

Direct DXT upload using Vidvox HAP SDK:

1. FFmpeg/MediaFileReader demuxes MOV container (keep existing)
2. Read raw packet bytes (skip FFmpeg HAP decoder)
3. Pass to HAP SDK: `HapDecode()` -> raw DXT data
4. Upload DXT directly via `glCompressedTexImage2D`
5. Render (GPU samples DXT natively)

**Benefits:**

- HAP: 0.5 bytes/pixel (DXT1) vs 4 bytes/pixel = 8x bandwidth reduction
- HAP-Q/Alpha: 1 byte/pixel (DXT5) vs 4 bytes/pixel = 4x reduction
- Zero CPU DXT decompression (only Snappy, which HAP SDK handles)
- Better multi-layer scalability for N100/N101

## Vidvox HAP SDK

**Source:** github.com/Vidvox/hap (MIT license)

**Files:** `hap.h`, `hap.c` (~500 lines of C)

**Dependencies:** Snappy (`libsnappy-dev`)

Key API:

```c
// Get DXT texture format from HAP frame
HapGetFrameTextureFormat(data, length, &format);

// Decode HAP -> raw DXT (handles Snappy internally)
HapDecode(data, length, output, outputSize, &outputFormat);
```

Output formats:

- `HapTextureFormat_RGB_DXT1` (0x83F0) - HAP standard
- `HapTextureFormat_RGBA_DXT5` (0x83F3) - HAP-Alpha
- `HapTextureFormat_YCoCg_DXT5` - HAP-Q (needs YCoCg shader, Phase 6)

## GPU Vendor Differences

| GPU | S3TC Support | Notes |

|-----|-------------|-------|

| NVIDIA (all) | Native | Always available |

| Intel N100/N101 | Native | Mesa 18.0+ built-in |

| Intel Iris/UHD | Native | Mesa 18.0+ built-in |

| AMD (Mesa) | Native | Mesa 18.0+ built-in |

Runtime check: `GL_EXT_texture_compression_s3tc`

## Implementation Phases

### Phase 1: Integrate Vidvox HAP SDK

- Add HAP SDK to `third_party/hap/` (hap.h, hap.c)
- Add Snappy dependency (`libsnappy-dev`)
- Update CMakeLists.txt

**Files:** `CMakeLists.txt`, `third_party/hap/`

### Phase 2: S3TC Runtime Detection

- Query `GL_EXT_texture_compression_s3tc` at init
- Store capability flag in `OpenGLDisplay`
- Log GPU vendor and S3TC status

**Files:** `OpenGLDisplay.h/cpp`

### Phase 3: Direct DXT Upload Path

- Modify `HAPVideoInput` to use HAP SDK
- Keep MediaFileReader for MOV demuxing
- Read raw packet -> `HapDecode()` -> DXT
- Upload via `GPUTextureFrameBuffer::uploadCompressedData()`

**Files:** `HAPVideoInput.h/cpp`

### Phase 4: Integration and Fallback

- Wire up in `VideoLayer` / `LayerPlayback`
- Fallback to FFmpeg RGBA if S3TC unavailable
- Performance logging

**Files:** `VideoLayer.cpp`, `LayerPlayback.cpp`

### Phase 5: Testing

- HAP, HAP-Q, HAP-Alpha variants
- Multi-layer stress (4+ layers on N100)
- Intel/AMD/NVIDIA compatibility

## Future Phases

### Phase 6: HAP-Q YCoCg Shader

HAP-Q uses YCoCg color space in DXT5. Needs fragment shader:

```glsl
vec4 ycocg = texture(tex, uv);
float Y = ycocg.a;
float Co = ycocg.r - 0.5;
float Cg = ycocg.g - 0.5;
vec3 rgb = vec3(Y + Co - Cg, Y + Cg, Y - Co - Cg);
```

### Phase 7: Multi-Chunk Parallel Decoding

HAP SDK supports chunked frames for 4K+ content:

- Iterate chunks with `HapDecodeFrame()`
- Parallel Snappy decompression via thread pool

## Key Files

| File | Changes |

|------|---------|

| `third_party/hap/` | NEW: Vidvox HAP SDK |

| `CMakeLists.txt` | HAP SDK + Snappy |

| `OpenGLDisplay.h/cpp` | S3TC detection |

| `HAPVideoInput.h/cpp` | HAP SDK integration |

| `GPUTextureFrameBuffer.cpp` | Already ready |

## Testing

1. HAP (DXT1) - standard
2. HAP-Alpha (DXT5) - transparency
3. HAP-Q (YCoCg-DXT5) - high quality (Phase 6)
4. Multi-layer: 4+ simultaneous HAP layers
5. Fallback: verify RGBA path works