# Testing Shader-Based Corner Deformation

## What Was Implemented

✅ **Shader-based corner deformation** - Hardware-accelerated perspective warping  
✅ **Quality modes** - Standard and high-quality (anisotropic filtering)  
✅ **OSC commands** - Remote control for all warping features  
✅ **Multi-plane support** - Works with RGBA, NV12, YUV420P formats  

## Quick Manual Test

### 1. Start the Video Composer

```bash
cd /home/ion/src/cuems/cuems-videocomposer/build
./cuems-videocomposer --osc-port 7700
```

### 2. Load a Video via OSC

```bash
# In another terminal, use oscsend:
oscsend localhost 7700 /videocomposer/layer/load ss video_test_files/test_h264_24fps.mp4 0

# Make visible and play (NOTE: Video requires MTC timecode to be running)
oscsend localhost 7700 /videocomposer/layer/0/visible i 1
oscsend localhost 7700 /videocomposer/layer/0/play
```

### 3. Test Corner Deformation

#### Test 1: Subtle Keystone Correction (~10°)
```bash
# Enable corner deformation
oscsend localhost 7700 /videocomposer/layer/corner_deform_enable ii 0 1

# Apply subtle warp (8 floats: 4 corners x,y offsets)
oscsend localhost 7700 /videocomposer/layer/corner_deform iffffffff 0 \
    -0.1 0.0  `# Corner 0 (top-left): x,y offset` \
     0.1 0.0  `# Corner 1 (top-right)` \
     0.1 0.1  `# Corner 2 (bottom-right)` \
    -0.1 0.1  `# Corner 3 (bottom-left)`
```

**Expected**: Video should show subtle perspective correction, smooth rendering

#### Test 2: Moderate Warp (~20°)
```bash
oscsend localhost 7700 /videocomposer/layer/corner_deform iffffffff 0 \
    -0.2 -0.1 \
     0.2 -0.1 \
     0.2  0.2 \
    -0.2  0.2
```

**Expected**: More pronounced warp, should still look clean

#### Test 3: Extreme Warp with High-Quality Mode (>30°)
```bash
# Enable high-quality mode first
oscsend localhost 7700 /videocomposer/layer/corner_deform_hq ii 0 1

# Apply extreme warp
oscsend localhost 7700 /videocomposer/layer/corner_deform iffffffff 0 \
    -0.4  0.0 \
    -0.2 -0.4 \
     0.2  0.4 \
     0.4  0.2
```

**Expected**: Extreme warp with enhanced filtering, sharper than without HQ mode

#### Test 4: Trapezoid (Projector Mapping Scenario)
```bash
# Top narrower than bottom (projecting onto angled wall)
oscsend localhost 7700 /videocomposer/layer/corner_deform iffffffff 0 \
    -0.3 -0.2 \
     0.3 -0.2 \
     0.4  0.3 \
    -0.4  0.3
```

**Expected**: Trapezoid shape, simulating projection mapping

#### Disable Warping
```bash
oscsend localhost 7700 /videocomposer/layer/corner_deform_enable ii 0 0
oscsend localhost 7700 /videocomposer/layer/corner_deform_hq ii 0 0
```

## Automated Test Script

```bash
cd /home/ion/src/cuems/cuems-videocomposer/tests
python3 test_shader_warping.py --video ../video_test_files/test_h264_24fps.mp4 --duration 30
```

## Visual Inspection Checklist

When running tests, verify:

- [ ] **No corruption** - Image should be clean, no garbled pixels
- [ ] **Smooth warping** - Transitions should be smooth, no stuttering  
- [ ] **Correct perspective** - Warped image should follow expected geometry
- [ ] **No performance issues** - Playback should remain smooth
- [ ] **HQ mode difference** - Extreme warps should look sharper with HQ enabled
- [ ] **Shader usage** - Check console for "Shader-based rendering enabled" message

## Technical Verification

### Check Shader Compilation
Look for in console output:
```
[INFO] OpenGL version: 3.3...
[INFO] GLSL version: 3.30...
[INFO] Shader-based rendering enabled
[VERBOSE] All video shaders compiled successfully
```

### Check Shader Path Selection
When warping is enabled, fixed-function fallback should NOT be used:
```
# Should NOT see this when using shaders:
# "Fallback: Fixed-function pipeline (for corner deformation...)"
```

### Performance Comparison
- **Without warping**: ~60 FPS (vsync limited)
- **With shader warping**: ~60 FPS (no performance hit)
- **With fixed-function**: Would be ~30-40 FPS (if it were still fallback)

## Corner Coordinate System

```
Corner 0 (top-left)        Corner 1 (top-right)
    (-1, -1)                    (+1, -1)
        ├───────────────────────┤
        │                       │
        │     Video Frame       │
        │                       │
        ├───────────────────────┤
    (-1, +1)                    (+1, +1)
Corner 3 (bottom-left)     Corner 2 (bottom-right)
```

Offsets are in normalized coordinates [-1, 1]:
- Positive X = right
- Positive Y = down
- Typical values: -0.5 to +0.5 for reasonable warps
- Extreme values: -0.8 to +0.8 (requires HQ mode)

## Multi-Layer Test

Load 2 videos and warp them differently:

```bash
# Layer 0: Subtle warp
oscsend localhost 7700 /videocomposer/layer/load isi 0 video1.mp4
oscsend localhost 7700 /videocomposer/layer/set_visible ii 0 1
oscsend localhost 7700 /videocomposer/layer/play i 0
oscsend localhost 7700 /videocomposer/layer/corner_deform_enable ii 0 1
oscsend localhost 7700 /videocomposer/layer/corner_deform iffffffff 0 \
    -0.1 0.0  0.1 0.0  0.1 0.1  -0.1 0.1

# Layer 1: Moderate warp + HQ + opacity
oscsend localhost 7700 /videocomposer/layer/load isi 1 video2.mp4
oscsend localhost 7700 /videocomposer/layer/set_visible ii 1 1
oscsend localhost 7700 /videocomposer/layer/set_opacity if 1 0.7
oscsend localhost 7700 /videocomposer/layer/play i 1
oscsend localhost 7700 /videocomposer/layer/corner_deform_enable ii 1 1
oscsend localhost 7700 /videocomposer/layer/corner_deform_hq ii 1 1
oscsend localhost 7700 /videocomposer/layer/corner_deform iffffffff 1 \
    -0.25 -0.1  0.25 -0.1  0.2 0.2  -0.2 0.2
```

**Expected**: Both layers render warped simultaneously, smooth performance

## Troubleshooting

### "Shader compilation failed"
- Check OpenGL version >= 3.3
- Check GLEW is installed: `dpkg -l | grep libglew`
- Check GPU drivers

### "Using fixed-function pipeline"  
- This is OK if OpenGL 3.3 not available
- Warping will still work but slower

### Visual artifacts
- Try enabling HQ mode for that layer
- Check if warp values are too extreme (>0.5)
- Verify video codec is supported

### Performance issues
- Check GPU usage: `nvidia-smi` or `intel_gpu_top`
- Reduce number of warped layers
- Disable HQ mode if not needed

## Success Criteria

✅ Shaders compile and load  
✅ Video renders without warping  
✅ Warping applies correctly  
✅ HQ mode shows quality improvement  
✅ Multiple warped layers work  
✅ No performance degradation  
✅ No visual corruption  

If all criteria pass, shader-based warping is ready for production!

