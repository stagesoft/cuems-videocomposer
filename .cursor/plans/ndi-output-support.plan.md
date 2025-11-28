<!-- 35ed6311-08f8-42a9-8cbf-8221d814379f 07b3cbda-0a6f-4480-8f71-9e7f53db1d49 -->
# NDI Video Output Support Implementation

## Overview

Implement NDI video output for cuems-videocomposer to send composited video over NDI protocol. The architecture will be extensible to support future output methods (hardware video output, streaming, recording) and includes headless rendering capability for server deployments.

**Key Design Decisions:**

- **NDI Library**: NDI SDK Direct (lower latency, full features for broadcast)
- **Headless Method**: Auto-detect (EGL+GBM → Hidden window fallback)
- **Frame Capture**: PBO Async with glReadPixels fallback
- **Threading**: Async encoding from start (separate thread with frame queue)

## Architecture

### Output System Components

```
┌─────────────────────────────────────────────────────────────┐
│                  VideoComposerApplication                    │
├─────────────────────────────────────────────────────────────┤
│  ┌──────────────┐    ┌──────────────┐    ┌───────────────┐ │
│  │ LayerManager │───▶│ OpenGL       │───▶│ FrameCapture  │ │
│  │              │    │ Renderer     │    │ (PBO Async)   │ │
│  └──────────────┘    └──────────────┘    └───────┬───────┘ │
│                                                   │         │
│  ┌──────────────────────────────────────────────▼───────┐ │
│  │                  OutputSinkManager                    │ │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐   │ │
│  │  │ NDIOutput   │  │ FileOutput  │  │ StreamOutput│   │ │
│  │  │ (SDK)       │  │ (Future)    │  │ (Future)    │   │ │
│  │  └─────────────┘  └─────────────┘  └─────────────┘   │ │
│  └──────────────────────────────────────────────────────┘ │
│                                                             │
│  ┌──────────────────────────────────────────────────────┐ │
│  │              DisplayBackend (Optional)                │ │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐   │ │
│  │  │ X11Display  │  │ Wayland     │  │ Headless    │   │ │
│  │  │             │  │ Display     │  │ (EGL+GBM)   │   │ │
│  │  └─────────────┘  └─────────────┘  └─────────────┘   │ │
│  └──────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

## Implementation Steps

### 1. Create OutputSink Interface

**File**: `src/cuems_videocomposer/cpp/output/OutputSink.h`

```cpp
class OutputSink {
public:
    enum class Type { NDI, HARDWARE, STREAMING, FILE };
    
    virtual ~OutputSink() = default;
    virtual bool open(const std::string& destination, const OutputConfig& config) = 0;
    virtual void close() = 0;
    virtual bool isReady() const = 0;
    virtual bool writeFrame(const FrameData& frame) = 0;  // Thread-safe
    virtual Type getType() const = 0;
    virtual std::string getId() const = 0;
};
```

### 2. Create Frame Capture System (PBO Async)

**File**: `src/cuems_videocomposer/cpp/output/FrameCapture.h`

**File**: `src/cuems_videocomposer/cpp/output/FrameCapture.cpp`

- **Primary**: Pixel Buffer Objects (PBO) for async GPU→CPU transfer
- **Fallback**: glReadPixels for compatibility
- Double-buffered PBOs for non-blocking capture
- Thread-safe frame queue for async encoding
```cpp
class FrameCapture {
public:
    bool initialize(int width, int height, PixelFormat format);
    void startCapture();           // Initiate async read (non-blocking)
    bool getCompletedFrame(FrameData& frame);  // Get previously captured frame
    bool hasPBOSupport() const;
private:
    GLuint pbo_[2];                // Double-buffered PBOs
    int currentPBO_;
    std::queue<FrameData> frameQueue_;
    std::mutex queueMutex_;
};
```


### 3. Create NDIVideoOutput (NDI SDK Direct)

**File**: `src/cuems_videocomposer/cpp/output/NDIVideoOutput.h`

**File**: `src/cuems_videocomposer/cpp/output/NDIVideoOutput.cpp`

Uses NewTek NDI SDK directly for:

- Lower latency than FFmpeg wrapper
- Full NDI feature access (tally, metadata, PTZ)
- Better NDI source discovery and management
```cpp
class NDIVideoOutput : public OutputSink {
public:
    bool open(const std::string& sourceName, const OutputConfig& config) override;
    void close() override;
    bool writeFrame(const FrameData& frame) override;
    
    // NDI-specific features
    void setTallyState(bool onProgram, bool onPreview);
    void sendMetadata(const std::string& metadata);
    
private:
    NDIlib_send_instance_t ndiSender_;
    NDIlib_video_frame_v2_t ndiFrame_;
    std::thread encodingThread_;
    ThreadSafeQueue<FrameData> frameQueue_;
    
    void encodingLoop();  // Async encoding thread
};
```


**NDI SDK Integration**:

- Link against `libndi` (NDI SDK library)
- Initialize with `NDIlib_initialize()`
- Create sender with `NDIlib_send_create()`
- Send frames with `NDIlib_send_send_video_v2()`

### 4. Create OutputSinkManager

**File**: `src/cuems_videocomposer/cpp/output/OutputSinkManager.h`

**File**: `src/cuems_videocomposer/cpp/output/OutputSinkManager.cpp`

```cpp
class OutputSinkManager {
public:
    bool addSink(std::unique_ptr<OutputSink> sink);
    bool removeSink(const std::string& id);
    void writeFrameToAll(const FrameData& frame);  // Distributes to all sinks
    bool hasActiveSinks() const;
    std::vector<std::string> getActiveSinkIds() const;
    
private:
    std::vector<std::unique_ptr<OutputSink>> sinks_;
    std::mutex sinksMutex_;
};
```

### 5. Create Headless Display Backend (EGL+GBM)

**File**: `src/cuems_videocomposer/cpp/display/HeadlessDisplay.h`

**File**: `src/cuems_videocomposer/cpp/display/HeadlessDisplay.cpp`

True headless rendering without X11/Wayland:

```cpp
class HeadlessDisplay : public DisplayBackend {
public:
    bool initialize() override;
    bool createWindow(int width, int height, const char* title) override;
    void makeCurrent() override;
    void swapBuffers() override;  // No-op for headless
    
private:
    int drmFd_;                    // DRM device file descriptor
    struct gbm_device* gbmDevice_; // GBM device
    EGLDisplay eglDisplay_;
    EGLContext eglContext_;
    EGLSurface eglSurface_;        // Offscreen surface
};
```

**Auto-detection logic** in `VideoComposerApplication::initializeDisplay()`:

1. If headless mode requested (no display output configured):

   - Try `HeadlessDisplay` (EGL+GBM)
   - Fallback to hidden window via X11Display/WaylandDisplay

2. If display output requested:

   - Use existing X11Display or WaylandDisplay

### 6. Async Encoding Thread Architecture

Each OutputSink that needs encoding runs its own thread:

```cpp
// In NDIVideoOutput
void NDIVideoOutput::encodingLoop() {
    while (running_) {
        FrameData frame;
        if (frameQueue_.waitAndPop(frame, 100ms)) {
            // Convert RGBA to NDI format (UYVY or NV12)
            convertFrame(frame, ndiFrame_);
            
            // Send via NDI (non-blocking)
            NDIlib_send_send_video_v2(ndiSender_, &ndiFrame_);
        }
    }
}
```

**Frame Queue Design**:

- Fixed-size queue (e.g., 3 frames) to limit memory
- Drop oldest frame if queue full (live output, can't fall behind)
- Condition variable for efficient waiting

### 7. Integration with VideoComposerApplication

**File**: `src/cuems_videocomposer/cpp/VideoComposerApplication.h`

```cpp
class VideoComposerApplication {
    // ... existing members ...
    
    std::unique_ptr<OutputSinkManager> outputSinkManager_;
    std::unique_ptr<FrameCapture> frameCapture_;
    bool headlessMode_;
    
    bool initializeOutputSinks();
    bool initializeHeadlessMode();
    void captureAndDistributeFrame();
};
```

**File**: `src/cuems_videocomposer/cpp/VideoComposerApplication.cpp`

```cpp
void VideoComposerApplication::render() {
    // Make context current
    displayBackend_->makeCurrent();
    
    // Render all layers
    if (layerManager_) {
        // ... existing layer rendering ...
    }
    
    // Capture frame for outputs (async PBO)
    if (outputSinkManager_ && outputSinkManager_->hasActiveSinks()) {
        // Start async capture of current frame
        frameCapture_->startCapture();
        
        // Get previously captured frame (from last render)
        FrameData completedFrame;
        if (frameCapture_->getCompletedFrame(completedFrame)) {
            outputSinkManager_->writeFrameToAll(completedFrame);
        }
    }
    
    // Swap buffers (or no-op for headless)
    displayBackend_->swapBuffers();
}
```

### 8. Configuration Support

**Command-line options**:

```
--ndi-output <source_name>    Enable NDI output with given source name
--headless                    Run without display window (outputs only)
--output-resolution WxH       Output resolution (default: match input)
--output-framerate FPS        Output frame rate (default: match input)
```

**Config file** (`~/.config/cuems-videocomposer/config`):

```ini
[output]
ndi_enabled = true
ndi_source_name = "CUEMS VideoComposer"
ndi_groups = ""
headless = false
```

### 9. Update CMakeLists.txt

```cmake
# NDI SDK detection
option(ENABLE_NDI_OUTPUT "Enable NDI output support" ON)

if(ENABLE_NDI_OUTPUT)
    find_path(NDI_INCLUDE_DIR Processing.NDI.Lib.h
        PATHS /usr/include /usr/local/include
        PATH_SUFFIXES ndi)
    find_library(NDI_LIBRARY NAMES ndi)
    
    if(NDI_INCLUDE_DIR AND NDI_LIBRARY)
        add_definitions(-DHAVE_NDI_OUTPUT)
        include_directories(${NDI_INCLUDE_DIR})
        list(APPEND OUTPUT_LIBS ${NDI_LIBRARY})
        message(STATUS "NDI output enabled")
    else()
        message(WARNING "NDI SDK not found - NDI output disabled")
    endif()
endif()

# Headless rendering (EGL + GBM)
pkg_check_modules(GBM gbm)
if(EGL_FOUND AND GBM_FOUND AND DRM_FOUND)
    add_definitions(-DHAVE_HEADLESS_DISPLAY)
    list(APPEND CPP_SOURCES src/cuems_videocomposer/cpp/display/HeadlessDisplay.cpp)
    message(STATUS "Headless display (EGL+GBM) enabled")
endif()

# Output sources
list(APPEND CPP_SOURCES
    src/cuems_videocomposer/cpp/output/OutputSink.cpp
    src/cuems_videocomposer/cpp/output/OutputSinkManager.cpp
    src/cuems_videocomposer/cpp/output/FrameCapture.cpp
)

if(HAVE_NDI_OUTPUT)
    list(APPEND CPP_SOURCES src/cuems_videocomposer/cpp/output/NDIVideoOutput.cpp)
endif()
```

## Files to Create

| File | Purpose |

|------|---------|

| `cpp/output/OutputSink.h` | Abstract interface for all outputs |

| `cpp/output/OutputSinkManager.h/cpp` | Manages multiple output sinks |

| `cpp/output/FrameCapture.h/cpp` | PBO-based async frame capture |

| `cpp/output/NDIVideoOutput.h/cpp` | NDI SDK output implementation |

| `cpp/display/HeadlessDisplay.h/cpp` | EGL+GBM headless backend |

## Files to Modify

| File | Changes |

|------|---------|

| `VideoComposerApplication.h/cpp` | Add OutputSinkManager, FrameCapture, headless mode |

| `ConfigurationManager.cpp` | Add NDI/output configuration options |

| `CMakeLists.txt` | Add NDI SDK detection, output sources, GBM |

## Future Extensions

- **HardwareVideoOutput**: DeckLink, DELTACAST capture cards
- **StreamingOutput**: RTSP server, WebRTC, HLS
- **FileOutput**: Record to MP4/MOV with background encoding
- **Multi-resolution output**: Different outputs at different resolutions

### To-dos

- [ ] Create OutputSink.h abstract interface for all output methods
- [ ] Create FrameCapture class with PBO async capture and glReadPixels fallback
- [ ] Create NDIVideoOutput using NDI SDK directly with async encoding thread
- [ ] Create OutputSinkManager to handle multiple simultaneous output sinks
- [ ] Create HeadlessDisplay backend using EGL+GBM for true headless rendering
- [ ] Integrate FrameCapture into render loop with double-buffered async capture
- [ ] Integrate OutputSinkManager and headless mode into VideoComposerApplication
- [ ] Add NDI output and headless mode configuration options
- [ ] Update CMakeLists.txt with NDI SDK detection, GBM, and output sources