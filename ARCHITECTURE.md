# cuems-videocomposer Architecture

## Input to Render Flow

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         VideoComposerApplication                         │
│                    (Main Event Loop & Orchestration)                     │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    │ creates & manages
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                            LayerManager                                  │
│                    (Manages Multiple VideoLayers)                       │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    │ contains
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                              VideoLayer                                  │
│  ┌──────────────────────────┐      ┌──────────────────────────────┐    │
│  │     InputSource          │      │     SyncSource                │    │
│  │  ┌────────────────────┐  │      │  ┌────────────────────────┐ │    │
│  │  │ VideoFileInput     │  │      │  │ MIDISyncSource         │ │    │
│  │  │ (video files)     │  │      │  │ (MTC timecode)         │ │    │
│  │  │                    │  │      │  │                        │ │    │
│  │  │ - open()           │  │      │  │ - connect()            │ │    │
│  │  │ - readFrame()      │  │      │  │ - pollFrame()          │ │    │
│  │  │ - seek()           │  │      │  │ - getFramerate()      │ │    │
│  │  │ - getFrameInfo()   │  │      │  │   (returns 25.0 fps)   │ │    │
│  │  │   (returns 24.0 fps)│  │      │  │                        │ │    │
│  │  └────────────────────┘  │      │  └────────────────────────┘ │    │
│  │                          │      │              │                │    │
│  └──────────────────────────┘      │              │ wrapped by    │    │
│                                     │              ▼                │    │
│                                     │  ┌────────────────────────┐  │    │
│                                     │  │FramerateConverter      │  │    │
│                                     │  │SyncSource              │  │    │
│                                     │  │                        │  │    │
│                                     │  │ - pollFrame()          │  │    │
│                                     │  │   converts:            │  │    │
│                                     │  │   inputFrame =         │  │    │
│                                     │  │   rint(syncFrame *     │  │    │
│                                     │  │    inputFps/syncFps)   │  │    │
│                                     │  │                        │  │    │
│                                     │  │ Example:               │  │    │
│                                     │  │ syncFrame=250 (25fps)  │  │    │
│                                     │  │ → inputFrame=240       │  │    │
│                                     │  │   (24fps)              │  │    │
│                                     │  └────────────────────────┘  │    │
│                                     └──────────────────────────────┘    │
│                                              │                           │
│                                              │ VideoLayer.update()      │
│                                              │ - updateFromSyncSource()  │
│                                              │ - loadFrame()             │
│                                              │ - render()                │
│                                              ▼                           │
│                                     ┌────────────────────┐               │
│                                     │   FrameBuffer      │               │
│                                     │   (decoded frame)  │               │
│                                     └────────────────────┘               │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    │ VideoLayer.render()
                                    │ returns FrameBuffer
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                          DisplayManager                                  │
│                    (Manages Display Windows)                             │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    │ delegates to
                                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                        DisplayBackend                                    │
│  ┌──────────────────────────────────────────────────────────────────┐  │
│  │                    OpenGLDisplay                                  │  │
│  │  ┌────────────────────────────────────────────────────────────┐  │  │
│  │  │              OpenGLRenderer                                 │  │  │
│  │  │                                                             │  │  │
│  │  │  - renderLayer()                                           │  │  │
│  │  │  - uploadTexture()                                         │  │  │
│  │  │  - drawQuad()                                               │  │  │
│  │  │                                                             │  │  │
│  │  └────────────────────────────────────────────────────────────┘  │  │
│  └──────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
                                    │ OpenGL rendering
                                    ▼
                            ┌───────────────┐
                            │  Display      │
                            │  (Screen)     │
                            └───────────────┘
```

## Component Details

### Input Sources
- **VideoFileInput**: Reads video files (MP4, MOV, etc.)
  - Provides: decoded frames, framerate, dimensions
  - Methods: `open()`, `readFrame()`, `seek()`, `getFrameInfo()`

### Sync Sources
- **MIDISyncSource**: Receives MTC (MIDI Time Code) at 25fps
  - Wrapped by **FramerateConverterSyncSource** to convert to input framerate
  - Other sync sources: LTCSyncSource (future), JACK sync (future)

### Framerate Conversion
- **FramerateConverterSyncSource**: Adapter that wraps any SyncSource
  - Converts frames from sync source framerate to input source framerate
  - Formula: `inputFrame = rint(syncFrame * inputFps / syncFps)`
  - Example: MTC at 25fps → Video at 24fps
    - syncFrame 250 → inputFrame 240
    - syncFrame 3000 → inputFrame 2880

### VideoLayer
- Combines InputSource and SyncSource
- Manages playback state (playing, paused)
- Handles frame loading and seeking
- Applies time scaling and offset
- Provides FrameBuffer for rendering

### Display Pipeline
- **LayerManager**: Manages multiple VideoLayers
- **DisplayManager**: Manages display windows
- **OpenGLDisplay**: OpenGL rendering backend
- **OpenGLRenderer**: Handles texture upload and quad drawing

## Data Flow Example

1. **MTC arrives** (25fps): `00:00:10:00` = frame 250
2. **FramerateConverterSyncSource** converts: 250 → 240 (24fps video)
3. **VideoLayer.updateFromSyncSource()** receives frame 240
4. **VideoLayer.loadFrame(240)** calls `inputSource->readFrame(240)`
5. **VideoFileInput.readFrame()** decodes frame 240 from video file
6. **FrameBuffer** contains decoded frame data
7. **VideoLayer.render()** returns FrameBuffer
8. **OpenGLDisplay.renderLayer()** uploads texture and draws to screen

## Key Design Principles

1. **Separation of Concerns**: Each component has a single responsibility
2. **Timecode-Agnostic**: Framerate conversion works with any sync source
3. **Input-Agnostic**: Works with video files, live feeds, streams
4. **Adapter Pattern**: FramerateConverterSyncSource wraps any SyncSource
5. **Future-Proof**: Easy to add new input/sync sources

