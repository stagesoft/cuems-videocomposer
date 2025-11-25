# Evaluation Resume (Concise): Media Decoding Architecture

## 1. Executive Summary

- **Primary video codec**: **HAP**  
  - GPU-optimized, zero-copy, no indexing, best for multiple layers (4–16+).  
  - Quality (HAP R @ 100%) ≈ ProRes 422 / DNxHD, but scales much better.

- **Other video codecs**  
  - **ProRes / DNxHD**: High quality, CPU-decoded (~30–40% CPU per 1080p@25fps layer). Good for 1–2 layers, not for many. No indexing needed (intra-frame).  
  - **H.264 / H.265 / AV1**: Hardware-decoded, good for 1–4 layers (decoder-limited). Usually GOP-based (indexing needed) unless encoded all-keyframe.  
  - **MJPEG / Uncompressed**: Intra-frame, no indexing, but CPU- and bandwidth-heavy, good for very few layers.

- **Audio codecs**  
  - All common codecs (PCM, FLAC, AAC, MP3, Opus, etc.) work with **timestamp-based seeking** → **no audio indexing needed**.  
  - **PCM (WAV/AIFF)** is recommended by default: 1–5% CPU for 10 streams, bandwidth is not an issue on SSD/HDD.  
  - **FLAC** is useful only when storage space or storage bandwidth is strongly constrained (USB 2.0, slow network).

- **Indexing**  
  - Only needed for GOP-based video codecs (H.264/H.265/AV1, VP9/VP8, Theora).  
  - Intra-frame codecs (HAP, ProRes, DNxHD, MJPEG, Uncompressed) should skip indexing entirely.

- **Performance model**  
  - HAP: best scaling; hardware-decoded H.264/H.265/AV1: good up to decoder limits; CPU decoding (ProRes/DNxHD) does not scale.  
  - Timecode-synchronized playback and OS file cache make concurrent access by video and audio players efficient.

---

## 2. Performance: Multiple Layers / Video Files

### 2.1 Decoding Methods (Conceptual)

1. **GPU-Optimized Codecs (HAP)**  
   - Direct compressed texture upload (DXT1/DXT5).  
   - Bypasses CPU and hardware decoder units.  
   - Minimal CPU load; scales in layer count.

2. **Hardware Decoding (H.264 / H.265 / AV1)**  
   - GPU decoder blocks (VAAPI, NVDEC, etc.).  
   - Limited by number of hardware decoder instances (typically 2–8).

3. **Software Decoding (ProRes, DNxHD, MJPEG, Uncompressed)**  
   - Pure CPU load.  
   - High per-layer cost, poor scaling.

### 2.2 Table 1 – Max Concurrent Layers (1080p @ 25fps)

``` 
Processor              HAP**   H.264   H.265   AV1      ProRes*   Notes
────────────────────────────────────────────────────────────────────────────────────
Intel N100/N101        4-6     2-3     2       1-2***   1         Limited GPU bandwidth, Gen12.2
Intel Celeron/Pentium  4-6     2-3     2       N/A      1         Entry-level iGPU
Intel Core i5          10-12   4-6     4       2-4***   2         Gen 11+ supports AV1
Intel Core i7          12-16   6-8     6       4-6***   3-4       Gen 11+ supports AV1
Intel Core i9          16+     8+      8       6-8***   4-5       Gen 12+ supports AV1
AMD Ryzen 3            4-6     2-4     2-3     N/A      1         Entry-level APU
AMD Ryzen 5            10-12   4-6     4       2-4***   2-3       RDNA 2+ supports AV1
AMD Ryzen 7            14-16   6-8     6       4-6***   4         RDNA 2+ supports AV1
AMD Ryzen 9            16+     8+      8       6-8***   5-6       RDNA 2+ supports AV1
NVIDIA GTX 1050/1650   12-16   2       2       N/A      N/A       Entry-level GPU
NVIDIA RTX 2060/3060   14-18   2-4     2-4     N/A      N/A       Mid-range GPU
NVIDIA RTX 3070/4070   16+     4       4       2-4***   N/A       High-end GPU
NVIDIA RTX A4000/5000  20+     4-8     4-8     4-6***   N/A       Professional GPU
```

*ProRes: Software decoding only (no hardware decoder support)  
**HAP: Direct compressed texture upload (DXT1/DXT5), not hardware decoding  
***AV1 support depends on GPU generation (Intel Gen 12+ including N100/N101 Gen12.2, AMD RDNA 2+, NVIDIA Ada Lovelace+)

### 2.3 Table 2 – CPU Utilization per Layer (1080p @ 25fps)

``` 
Processor              HAP**   H.264    H.265    AV1       ProRes*
───────────────────────────────────────────────────────────────────
Intel N100/N101        ~2%     ~5%      ~8%      ~10%***   ~40%
Intel Celeron/Pentium  ~2%     ~5%      ~8%      N/A       ~40%
Intel Core i5          ~3%     ~8%      ~10%     ~12%***   ~35%
Intel Core i7          ~2%     ~7%      ~9%      ~10%***   ~30%
Intel Core i9          ~2%     ~5%      ~7%      ~8%***    ~25%
AMD Ryzen 3            ~2%     ~6%      ~9%      N/A       ~35%
AMD Ryzen 5            ~3%     ~9%      ~11%     ~13%***   ~30%
AMD Ryzen 7            ~2%     ~7%      ~9%      ~10%***   ~28%
AMD Ryzen 9            ~2%     ~5%      ~7%      ~8%***    ~25%
```

*ProRes: Software decoding only (no hardware decoder support)  
**HAP: Direct compressed texture upload (minimal CPU, no decoding)  
***AV1 support depends on GPU generation

### 2.4 Table 3 – Max Concurrent Layers (4K @ 25fps)

``` 
Processor              HAP**   H.264   H.265   AV1      ProRes*
─────────────────────────────────────────────────────────────────
Intel Core i5          4-6     2-3     2       1-2***   1
Intel Core i7          6-8     3-4     3       2-3***   1-2
Intel Core i9          8-12    4-6     4       3-4***   2
AMD Ryzen 5            4-6     2-3     2       1-2***   1
AMD Ryzen 7            6-8     3-4     3       2-3***   1-2
AMD Ryzen 9            8-12    4-6     4       3-4***   2
NVIDIA RTX 3070/4070   8-12    2-4     2-4     1-2***   N/A
NVIDIA RTX A4000/5000  12-16   4-6     4-6     2-4***   N/A
```

*ProRes: Software decoding only (no hardware decoder support)  
**HAP: Direct compressed texture upload (DXT1/DXT5), not hardware decoding  
***AV1 support depends on GPU generation

### 2.5 Table 4 – Performance Rating by Layer Count

``` 
Layers   HAP                    Hardware                                    CPU
────────────────────────────────────────────────────────────────────────────────────────────
1-2      ✅ Excellent           ✅ Excellent                                 ✅ Good (high-end) / ⚠️ Acceptable (mid-range)
3-4      ✅ Excellent           ✅ Very Good                                 ⚠️ Acceptable (high-end) / ❌ Poor (mid-range)
5-8      ✅ Very Good           ⚠️ Good (within limits) / ⚠️ Limited        ❌ Poor
9-16     ✅ Good                ⚠️ Limited (decoder bottleneck)             ❌ Unusable
17+      ⚠️ Acceptable (GPU)    ❌ Unusable                                  ❌ Unusable
```

### 2.6 Key Performance Insights (Condensed)

- **HAP**  
  - Best scaling for layers (4–16+).  
  - Low CPU (~2–3% per layer).  
  - Not limited by hardware decoder count.

- **Hardware-decoded H.264/H.265/AV1**  
  - Excellent for 1–4 layers, limited by decoder instances (2–8).  
  - CPU usage moderate (~5–10% per 1080p layer).

- **ProRes / DNxHD (software)**  
  - ~30–40% CPU per 1080p layer → 1–2 layers max practical.  
  - Good for quality, poor for multi-layer performance.

- **Resolution**  
  - 4K roughly halves maximum layer counts vs 1080p.  
  - HAP less affected because compressed textures scale well.

---

## 3. Codec Selection & Indexing

### 3.1 Indexing Requirements

``` 
Codec Type          Codec                                    Indexing Need      Reason
────────────────────────────────────────────────────────────────────────────────────────────────────────────
Video - Intra-Frame HAP (all variants)                      ❌ No              All frames keyframes
Video - Intra-Frame ProRes, DNxHD/DNxHR                     ❌ No              Intra-frame only
Video - Intra-Frame MJPEG, Uncompressed                     ❌ No              Direct frame access
Video - GOP-Based   H.264, H.265/HEVC, AV1                  ⚠️ Usually Yes     Common: GOP structure. Suitable when all-keyframe (doesn't need indexing)
Video - GOP-Based   VP9/VP8, Theora                         ✅ Yes             Usually GOP structure
Audio - All         PCM, FLAC, ALAC, AAC, MP3, etc.         ❌ No              Frequent keyframes, accurate timestamps
```

### 3.2 Recommended Video Codecs (Concise)

- **Tier 1 – No Indexing (Preferred)**  
  - **HAP** – Primary format. GPU-optimized, zero-copy, best for 4–16+ layers, quality ≈ ProRes 422.  
  - **ProRes / DNxHD** – High quality but CPU-heavy; good for 1–2 layers; avoid for many layers.  
  - **MJPEG / Uncompressed** – Simple and frame-accurate; heavy CPU/bandwidth; use sparingly.

- **Tier 2 – With Indexing (Compatibility)**  
  - **H.264 / H.265 / AV1** – Good with hardware decoding (1–4 layers). Needs indexing if GOP-based; can be all-keyframe with specific encoder options.  
  - **VP9 / VP8 / Theora** – Usually GOP-based; CPU-decoded, suitable only for few layers.

### 3.3 Indexing Strategy

- **Skip indexing** for: HAP, ProRes, DNxHD, MJPEG, Uncompressed.  
- **Use 3-pass xjadeo-style indexing** for GOP-based codecs:  
  1. Pass 1 – Packet scan + keyframe detection.  
  2. Pass 2 – Verify keyframes by decoding.  
  3. Pass 3 – Build seek table.
- **All-keyframe detection**: If codec is intra-frame or GOP size == 1, indexing can be skipped.

---

## 4. Audio Codecs (Concise)

- **Indexing**  
  - All common audio codecs work with timestamp-based seeking.  
  - **No audio indexing is needed or recommended.**

- **PCM (WAV/AIFF)**  
  - 0.1–0.5% CPU per stereo stream.  
  - 10 streams ≈ 1–5% CPU total.  
  - 14 MB/s for 10 streams is only 2.8–9.3% of typical SSD/HDD bandwidth → **not a bottleneck**.  
  - Recommended default for simplicity and minimal CPU overhead.

- **FLAC**  
  - 1–3% CPU per stream (10–30% for 10 streams).  
  - ~50% storage and bandwidth savings vs PCM.  
  - Only justified if **storage space** or **storage bandwidth** is strongly constrained (USB 2.0, slow network).

- **AAC / MP3 / Opus**  
  - 1–2% CPU per stream, very small files.  
  - Good for archival/delivery or extreme storage limits; not necessary for local editing/playback.

**Summary**:  
Use **PCM** unless storage or I/O constraints are extreme; audio overhead is small compared to video decoding.

---

## 5. Architecture & Implementation (Short)

- **Timecode-synchronized playback**  
  - Both `cuems-videocomposer` and `cuems-audioplayer` use external timecode to drive seeking.  
  - Both can open the same media file concurrently; OS cache and sequential access make this efficient and safe.

- **Current behavior**  
  - Video: Uses codec detection; intra-frame codecs can skip indexing; GOP-based codecs are indexed with a 3-pass xjadeo-like system.  
  - Audio: Uses timestamp-based seeking only; no indexing or extra structures.

- **Planned / recommended improvements**  
  - **Codec-based indexing skip**: Immediately skip indexing for known intra-frame codecs.  
  - **Optional background indexing** for GOP-based codecs (improved UX, non-blocking).  
  - **Optional batch processor** (separate tool) to transcode media into preferred formats: HAP (primary) or ProRes/DNxHD / all-keyframe H.264/H.265/AV1.

---

## 6. Practical Recommendations (One Page)

- **Video**  
  - Use **HAP** for any project with multiple video layers.  
  - Use **ProRes/DNxHD** only when you need their ecosystem/quality and layer count is low (1–2).  
  - Use **hardware-decoded H.264/H.265/AV1** for compatibility and 1–4 layers.  
  - Avoid CPU-only codecs (ProRes/DNxHD) for many layers.

- **Audio**  
  - Store media audio as **PCM (WAV/AIFF)** by default.  
  - Consider **FLAC** only when storage or I/O is clearly the bottleneck.  
  - Ignore audio indexing; rely on timestamps.

- **Indexing**  
  - Only implement and pay indexing cost for GOP-based video codecs.  
  - Skip indexing for intra-frame codecs and all audio codecs.


