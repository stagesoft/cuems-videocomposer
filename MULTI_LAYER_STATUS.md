# Multi-Layer Video Playback Status

## Date: 2025-11-18

## Issues Fixed ✅

### 1. Static Variable Sharing Between Layers
- **Status**: ✅ FIXED
- **Location**: `LayerPlayback.cpp`
- **Fix**: Converted static variables to instance members (`wasRolling_`, `lastLoggedFrame_`, `debugCounter_`)
- **Result**: Each layer now has independent MTC sync state

### 2. Shared Texture ID Causing Corruption
- **Status**: ✅ FIXED
- **Location**: `OpenGLRenderer.cpp`
- **Fix**: Each layer gets its own texture (initially temporary, now cached)
- **Result**: No more texture corruption between layers

### 3. Texture Deletion Timing
- **Status**: ✅ FIXED
- **Location**: `OpenGLRenderer.cpp`
- **Fix**: Deferred texture deletion until after `swapBuffers()`
- **Result**: Prevents corruption from deleting textures while GPU is using them

### 4. Texture Creation/Deletion Performance
- **Status**: ✅ FIXED
- **Location**: `OpenGLRenderer.cpp`
- **Fix**: Implemented texture caching per layer - textures are reused instead of created/deleted every frame
- **Result**: Much better performance, but...

## Current Issue ❌

### Multi-Layer Test Still Hangs
- **Status**: ❌ STILL HANGING
- **Test**: `test_dynamic_file_management.py` with 2 layers
- **Symptoms**:
  - Video starts playing and rendering correctly
  - Both layers display properly initially
  - After some time, application hangs
  - No texture corruption (that's fixed!)
  - Monitoring thread may or may not continue (needs verification)

## Possible Causes (To Investigate)

1. **Frame Loading/Decoding Blocking**
   - Multiple layers decoding simultaneously might cause blocking
   - Check if `readFrame()` or `readFrameToTexture()` can block
   - Hardware decoder might have limitations on concurrent sessions

2. **OpenGL Context Issues**
   - Multiple layers uploading textures might cause context conflicts
   - Check if `makeCurrent()`/`clearCurrent()` is properly synchronized

3. **Memory/Resource Exhaustion**
   - Cached textures might be accumulating
   - Frame buffers might not be released properly
   - Check for memory leaks in texture cache

4. **MTC Sync Source Blocking**
   - Multiple layers polling the same MTC sync source
   - `pollFrame()` might block or cause contention
   - Check if `FramerateConverterSyncSource` wrapper has issues

5. **File I/O Contention**
   - Two layers reading from same file might cause I/O blocking
   - MediaFileReader might not be thread-safe for concurrent access
   - Check if seeks/reads can block each other

## Next Steps

1. **Add Debug Logging**
   - Log when hang occurs (which operation)
   - Add timeout detection
   - Log texture cache state

2. **Profile/Strace**
   - Use `strace` to see what syscall is blocking
   - Use `gdb` to see where thread is stuck
   - Check if it's in OpenGL, FFmpeg, or file I/O

3. **Test Single Layer**
   - Verify single layer works indefinitely
   - This confirms it's multi-layer specific

4. **Test Different Scenarios**
   - Two different video files (not same file)
   - Hardware vs software decoding
   - Different codecs

5. **Check Thread Safety**
   - Verify all shared resources are properly protected
   - Check if OpenGL context operations are thread-safe

## Files Modified

- `src/cuems_videocomposer/cpp/layer/LayerPlayback.h` - Instance variables
- `src/cuems_videocomposer/cpp/layer/LayerPlayback.cpp` - Instance variables
- `src/cuems_videocomposer/cpp/display/OpenGLRenderer.h` - Texture caching
- `src/cuems_videocomposer/cpp/display/OpenGLRenderer.cpp` - Texture caching, deferred deletion
- `src/cuems_videocomposer/cpp/display/OpenGLDisplay.cpp` - Deferred cleanup call

## Test Command

```bash
cd /home/ion/src/cuems/cuems-videocomposer
python3 tests/test_dynamic_file_management.py \
  --videocomposer build/cuems-videocomposer \
  --video1 video_test_files/test_h264_mp4.mp4 \
  --video2 video_test_files/test_h264_720p.mp4
```

## Notes

- Texture corruption is completely fixed ✅
- Initial playback works correctly ✅
- Hang occurs after some time of playback ❌
- Need to identify the blocking operation causing the hang

