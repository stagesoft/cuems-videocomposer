<!-- 51f97d72-1bf8-4136-86f0-b344a47de3d6 a3f96aa9-fb08-4cdb-9720-4bdaa01d4fb3 -->
# Dynamic Video File Management System

## Overview

Implement a persistent video player that can dynamically load multiple video files, play them simultaneously, and optionally unload them when finished. Support looping (full file or region-based with loop count) and handle indexing implications when loading files during playback. cuems-engine controls the player via OSC.

## Key Architecture Decisions

### Layer ID = Cue ID (UUID)

- Use cue ID from cuems-engine as layer ID
- Cue ID is a standard UUID (string format)
- Stable layer IDs for reliable reference
- Layers are persistent containers for cues
- UUID format: "550e8400-e29b-41d4-a716-446655440000" (standard UUID string)

### Global Sync Source with Per-Layer Controls

- **One sync source for entire application** (shared across all layers)
- Created during `initializeGlobalSyncSource()` or `initializeMIDI()`
- Stored as `globalSyncSource_` in `VideoComposerApplication`
- All layers reference the same sync source instance
- Each layer has:
- `timeOffset_` - Frame offset (already exists, per-layer)
- `mtcFollow_` - Boolean flag to enable/disable MTC following (new, per-layer)
- When `mtcFollow_ = false`: Layer ignores sync source, manual control only
- When `mtcFollow_ = true`: Layer follows global sync source with offset applied
- Each layer wraps global sync source with `FramerateConverterSyncSource` (for FPS conversion)
- **Offset implementation like cuems-audioplayer**: Per-layer offset applied to sync frames, with per-layer MTC follow control

### Common Methods for Codec Detection and Layer Creation

- Extract codec detection into `createInputSourceFromFile()`
- Extract layer creation into `createEmptyLayer()`
- Both `createInitialLayer()` and new loading methods use these common methods

## OSC Command Interface

### File Management

- `/videocomposer/layer/load s s` - Create new layer with file (filepath, cueId)
- Parameters: filepath (string), cueId (UUID string)
- Creates new layer using cue ID (UUID) as layer ID
- Loads file into the new layer
- One-to-one pattern: file creates layer
- Example: `/videocomposer/layer/load "/path/to/video.mp4" "550e8400-e29b-41d4-a716-446655440000"`

- `/videocomposer/layer/<cueId>/file s` - Load/replace file in existing layer
- Loads file into layer identified by cue ID (UUID)
- Creates layer if it doesn't exist
- Many-to-one pattern: file loads into existing layer

- `/videocomposer/layer/unload s` - Unload file from layer by cue ID
- Unloads file from layer identified by cue ID (UUID string)
- Keeps layer (preserves properties: position, opacity, z-order, etc.)
- Clears InputSource, layer becomes empty

### Per-Layer Video/Image Controls (from jadeo)

All controls that affect video image are per-layer:

**Position & Display:**

- `/videocomposer/layer/<cueId>/position ii` - Set position (x, y)
- `/videocomposer/layer/<cueId>/opacity f` - Set opacity (0.0-1.0)
- `/videocomposer/layer/<cueId>/visible i` - Show/hide layer (0=off, 1=on)
- `/videocomposer/layer/<cueId>/zorder i` - Set z-order (stacking order)

**Transform:**

- `/videocomposer/layer/<cueId>/scale ff` - Set scale (scaleX, scaleY)
- `/videocomposer/layer/<cueId>/xscale f` - Set X scale
- `/videocomposer/layer/<cueId>/yscale f` - Set Y scale
- `/videocomposer/layer/<cueId>/rotation f` - Set rotation (degrees)

**Crop & Panorama:**

- `/videocomposer/layer/<cueId>/crop iiii` - Set crop rectangle (x, y, width, height)
- `/videocomposer/layer/<cueId>/crop/disable` - Disable crop
- `/videocomposer/layer/<cueId>/panorama i` - Enable/disable panorama mode (0=off, 1=on)
- `/videocomposer/layer/<cueId>/pan i` - Set pan offset (pixels, for panorama mode)

**Corner Deformation (Warping):**

- `/videocomposer/layer/<cueId>/corners ffffffff` - Set all 4 corners (8 floats: corner1x, corner1y, corner2x, corner2y, corner3x, corner3y, corner4x, corner4y)
- `/videocomposer/layer/<cueId>/corner1 ff` - Set corner 1 (x, y)
- `/videocomposer/layer/<cueId>/corner2 ff` - Set corner 2 (x, y)
- `/videocomposer/layer/<cueId>/corner3 ff` - Set corner 3 (x, y)
- `/videocomposer/layer/<cueId>/corner4 ff` - Set corner 4 (x, y)

**Time Controls:**

- `/videocomposer/layer/<cueId>/offset i` - Set frame offset (integer)
- `/videocomposer/layer/<cueId>/offset s` - Set frame offset (SMPTE timecode string)
- `/videocomposer/layer/<cueId>/timescale f` - Set time multiplier (default: 1.0)
- `/videocomposer/layer/<cueId>/timescale fi` - Set time multiplier and offset (float, int)
- `/videocomposer/layer/<cueId>/reverse` - Reverse playback (multiply timescale by -1.0)
- `/videocomposer/layer/<cueId>/mtcfollow i` - Enable/disable MTC following (0=off, 1=on)

**Blend Mode:**

- `/videocomposer/layer/<cueId>/blendmode i` - Set blend mode (0=NORMAL, 1=MULTIPLY, 2=SCREEN, 3=OVERLAY)

### Auto-unload (per-cue)

- `/videocomposer/layer/<cueId>/autounload i` - Set auto-unload (0=off, 1=on, default=0)

### Looping (per-file, from cuems-engine)

- `/videocomposer/layer/<cueId>/loop i [i]` - Enable/disable full file loop (0=off, 1=on, [loopCount=-1])
- `/videocomposer/layer/<cueId>/loop/region i i [i]` - Set loop region (startFrame, endFrame, [loopCount=-1])
- `/videocomposer/layer/<cueId>/loop/region/disable` - Disable region loop

**Loop Count Values:**

- `-1` or omitted: Loop infinitely
- `0`: No loop (play once)
- `>0`: Loop exactly N times, then continue to end

## Implementation Details

### Files to Modify

1. `src/cuems_videocomposer/cpp/layer/LayerProperties.h` - Add `autoUnload`, `loopRegion` struct with `loopCount` and `currentLoopCount`
2. `src/cuems_videocomposer/cpp/layer/LayerPlayback.h` - Add `mtcFollow_` member variable
3. `src/cuems_videocomposer/cpp/layer/LayerPlayback.cpp` - Add playback end detection, region loop handling, and mtcfollow support
4. `src/cuems_videocomposer/cpp/remote/RemoteCommandRouter.cpp` - Implement file loading/unloading commands
5. `src/cuems_videocomposer/cpp/VideoComposerApplication.h` - Add `globalSyncSource_` member, new public/private methods
6. `src/cuems_videocomposer/cpp/VideoComposerApplication.cpp` - Refactor to use global sync source, add common methods, refactor `createInitialLayer()`
7. `src/cuems_videocomposer/cpp/layer/LayerManager.cpp` - Add auto-unload cleanup in `updateAll()`

### New Methods in VideoComposerApplication

#### Private Helper Methods

1. **`std::unique_ptr<InputSource> createInputSourceFromFile(const std::string& filepath)`**

- Common method for codec detection and InputSource creation
- Detects codec (HAP vs other)
- Creates appropriate InputSource (HAPVideoInput or VideoFileInput)
- Handles `noIndex` configuration
- Returns created InputSource or nullptr on error
- Used by both `createInitialLayer()` and `loadFileIntoLayer()`

2. **`std::unique_ptr<VideoLayer> createEmptyLayer(int cueId)`**

- Common method for creating a new layer
- Creates VideoLayer with cue ID as layer ID
- Sets basic properties (visible, opacity, zOrder)
- Does NOT set input source or sync source
- Returns created layer or nullptr on error
- Used by both `createLayerWithFile()` and `loadFileIntoLayer()`

3. **`bool initializeGlobalSyncSource()`**

- Creates global sync source (MIDI/MTC) during initialization
- Called from `initializeMIDI()` or separate initialization step
- Stores in `globalSyncSource_` member
- All layers will reference this shared sync source
- Returns true on success, false on error

#### Public Methods (called from RemoteCommandRouter)

1. **`bool createLayerWithFile(int cueId, const std::string& filepath)`**

- Creates a new layer with cue ID as layer ID and loads file
- One-to-one pattern: file creates layer
- Calls `createInputSourceFromFile()` for codec detection
- Calls `createEmptyLayer()` to create layer structure
- Sets InputSource to layer
- Sets global sync source reference (shared, not per-layer)
- Wraps sync source with FramerateConverterSyncSource (per-layer, for FPS conversion)
- Returns true on success, false on error
- Used by `/videocomposer/layer/load s i` command

2. **`bool loadFileIntoLayer(int cueId, const std::string& filepath)`**

- Loads a video file into the layer identified by cue ID
- Many-to-one pattern: file loads into existing layer
- Creates layer if it doesn't exist (calls `createEmptyLayer()`)
- Calls `createInputSourceFromFile()` for codec detection
- Sets InputSource to layer
- Sets global sync source reference (shared, not per-layer)
- Wraps sync source with FramerateConverterSyncSource (per-layer, for FPS conversion)
- Replaces existing file if layer already has one
- Returns true on success, false on error
- Used by `/videocomposer/layer/<cueId>/file s` command

3. **`bool unloadFileFromLayer(int cueId)`**

- Unloads file from layer identified by cue ID
- Clears InputSource from layer
- Keeps layer (preserves properties: position, opacity, z-order, etc.)
- Layer becomes empty, ready for next file load
- Returns true on success, false if layer doesn't exist
- Used by `/videocomposer/layer/unload i` command

### New Methods in LayerPlayback

- `void checkPlaybackEnd()` - Check if playback reached end, handle region loop and loop count
- `void setMtcFollow(bool enabled)` - Enable/disable MTC following for this layer
- `bool getMtcFollow() const` - Get MTC follow state
- Modify `updateFromSyncSource()` to respect `mtcFollow_` flag (skip sync if false)

### New Methods in RemoteCommandRouter

- `handleLayerLoad()` - Create new layer with file (one-to-one: `/videocomposer/layer/load s i`)
- Takes filepath (string) and cueId (integer) as arguments
- `handleLayerFile()` - Load file into existing layer (many-to-one: `/videocomposer/layer/<cueId>/file s`)
- `handleLayerUnload()` - Unload file from layer by cue ID (`/videocomposer/layer/unload i`)
- `handleLayerAutoUnload()` - Set auto-unload flag per layer
- `handleLayerLoopRegion()` - Set loop region with loop count (from cuems-engine)
- `handleLayerLoopRegionDisable()` - Disable region loop
- `handleLayerLoop()` - Set full file loop with loop count
- `handleLayerOffset()` - Set frame offset for layer
- `handleLayerMtcFollow()` - Enable/disable MTC following for layer

## Testing Considerations

- Load multiple files simultaneously via OSC using cue IDs
- Test both one-to-one (`/layer/load s i`) and many-to-one (`/layer/<cueId>/file s`) patterns
- Test auto-unload when playback ends (per-cue)
- Test region looping with loop count (0, N, infinite)
- Test full file looping with loop count
- Test per-layer offset and mtcfollow controls
- Test global sync source shared across all layers
- Test indexing behavior when loading during playback
- Test layer persistence across file loads/unloads
- Verify no memory leaks on unload
- Test that loop region works: play from start, then loop only region
- Test that layer properties persist when replacing file in existing layer

## Implementation Status: ✅ COMPLETE

All planned functionality has been successfully implemented:

### Core Features ✅

- Dynamic file loading/unloading via OSC
- UUID-based layer management (cue IDs)
- Auto-unload on playback end (per-layer)
- Full file looping with loop count
- Region-based looping with loop count
- Global MTC sync source (shared across all layers)
- Per-layer sync controls (offset, mtcfollow)

### Per-Layer Controls ✅

- Position (x, y)
- Opacity (with proper blending)
- Scale (x, y, combined)
- Rotation
- Corner deformation (warping with homography)
- Blend modes (NORMAL, MULTIPLY, SCREEN, OVERLAY)
- Crop and panorama
- Visibility and z-order

### Architecture ✅

- Common codec detection method
- Common layer creation method
- Simplified sync source (global, always initialized)
- UUID to layer ID mapping
- Proper cleanup and memory management

### To-dos

- [x] Add autoUnload and loopRegion to LayerProperties.h ✅
- [x] Add playback end detection and region loop handling in LayerPlayback.cpp ✅
- [x] Implement createLayerWithFile() in VideoComposerApplication.cpp ✅
- [x] Implement handleLayerLoad() and handleLayerUnload() in RemoteCommandRouter.cpp ✅
- [x] Implement handleLayerLoopRegion() and handleLayerAutoUnload() in RemoteCommandRouter.cpp ✅
- [x] Add auto-unload cleanup logic in LayerManager::updateAll() ✅
- [x] Implement per-layer controls (position, scale, rotation, corner deformation, blend mode) ✅
- [x] Implement global sync source with per-layer FramerateConverterSyncSource wrappers ✅
- [x] Simplify sync source initialization (always create global, MIDI enabled by default) ✅