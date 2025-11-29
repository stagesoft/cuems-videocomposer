<!-- 35ed6311-08f8-42a9-8cbf-8221d814379f 51b0f556-233f-4aec-bc20-6f931dbd5245 -->
# NDI Input Support Implementation

## Overview

Implement NDI input support for cuems-videocomposer to receive video streams over NDI protocol. The architecture supports multiple simultaneous input sources on different layers (e.g., video file on layer 1, HAP on layer 2, NDI on layer 3, V4L2 webcam on layer 4).

**Key Design Decisions:**

- **NDI Library**: NDI SDK Direct (lower latency, full features, consistency with output plan)
- **Live Input Threading**: Async with frame buffer (dedicated thread per live input)
- **V4L2 Support**: Deferred (FFmpeg stopgap works today, add dedicated support later)
- **Live Stream Interface**: Common `LiveInputSource` base class

## License Compatibility

**Project License**: LGPL v3 (GNU Lesser General Public License version 3)

**NDI SDK License Compatibility**: ✅ **COMPATIBLE**

LGPL v3 is designed to allow linking with proprietary libraries, making it compatible with the NDI SDK's proprietary license. This means:

- ✅ **Can bundle NDI SDK binaries** with the application
- ✅ **Can statically or dynamically link** the NDI SDK
- ✅ **Can distribute** the combined work
- ✅ **Project code remains LGPL v3** (users can replace/modify the library)

**NDI SDK License Requirements** (must be complied with):

1. **Trademark Attribution**: Use "NDI®" with registered trademark symbol and include statement: "NDI® is a registered trademark of Vizrt NDI AB" near first usage or in footnotes
2. **Website Link**: Include link to [https://ndi.video/](https://ndi.video/) near all instances where NDI is used/selected in the product, on website, and in documentation
3. **EULA Terms**: Application's EULA must include:
   - Prohibition on modifying the NDI SDK
   - Prohibition on reverse engineering/disassembly
   - Warranty disclaimers on behalf of NewTek/Vizrt
   - Liability disclaimers for NewTek/Vizrt
   - Export compliance requirements
   - Copyright notice showing NewTek, Inc. as copyright owner
4. **DLL Management**: Include NDI DLLs in application directories (not system path)
5. **No Tool Distribution**: Do not distribute NDI tools; provide link to [ndi.video/tools](https://ndi.video/tools) instead

**Implementation Notes**:
- NDI SDK header files can be distributed under MIT license (already compatible)
- Binary libraries can be bundled with the application
- All NDI SDK license requirements must be met in the application's UI, documentation, and EULA

## Architecture

### Multi-Input Layer System

```
┌─────────────────────────────────────────────────────────────────┐
│                      LayerManager                                │
├─────────────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────┐│
│  │   Layer 1   │  │   Layer 2   │  │   Layer 3   │  │ Layer 4 ││
│  │ VideoFile   │  │ HAPVideo    │  │ NDIVideo    │  │ V4L2    ││
│  │ Input       │  │ Input       │  │ Input       │  │ (FFmpeg)││
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘  └────┬────┘│
│         │                │                │               │     │
│  ┌──────▼──────┐  ┌──────▼──────┐  ┌──────▼──────┐  ┌────▼────┐│
│  │ InputSource │  │ InputSource │  │LiveInput   │  │LiveInput││
│  │ (sync read) │  │ (sync read) │  │Source      │  │Source   ││
│  │             │  │             │  │(async buf) │  │(async)  ││
│  └─────────────┘  └─────────────┘  └─────────────┘  └─────────┘│
└─────────────────────────────────────────────────────────────────┘
```

### Input Source Class Hierarchy

```
InputSource (abstract base)
├── VideoFileInput        (existing - file-based, sync read)
├── HAPVideoInput         (existing - HAP codec, sync read)
└── LiveInputSource (new abstract base - async buffered)
    ├── NDIVideoInput     (new - NDI SDK direct)
    └── FFmpegLiveInput   (new - V4L2, RTSP via FFmpeg)
```

## Implementation Steps

### 1. Add isLiveStream() to InputSource Interface

**File**: `src/cuems_videocomposer/cpp/input/InputSource.h`

```cpp
class InputSource {
public:
    // ... existing methods ...
    
    /**
     * Check if this is a live stream (no seeking, continuous reading)
     * @return true for live streams (NDI, V4L2, RTSP), false for files
     */
    virtual bool isLiveStream() const { return false; }  // Default: not live
    
    /**
     * For live streams: get the latest available frame
     * Default implementation calls readFrame(0, buffer)
     */
    virtual bool readLatestFrame(FrameBuffer& buffer) {
        return readFrame(0, buffer);
    }
};
```

### 2. Create LiveInputSource Base Class

**File**: `src/cuems_videocomposer/cpp/input/LiveInputSource.h`

```cpp
class LiveInputSource : public InputSource {
public:
    LiveInputSource();
    virtual ~LiveInputSource();
    
    // InputSource interface
    bool isLiveStream() const override { return true; }
    bool seek(int64_t frameNumber) override { return false; }  // No seeking
    bool readFrame(int64_t frameNumber, FrameBuffer& buffer) override;
    bool readLatestFrame(FrameBuffer& buffer) override;
    
    // Live stream specific
    void setBufferSize(int frames) { bufferSize_ = frames; }
    int getBufferSize() const { return bufferSize_; }
    bool isBufferEmpty() const;
    int getBufferedFrameCount() const;
    
protected:
    // Subclasses implement this to capture frames
    virtual bool captureFrame(FrameBuffer& buffer) = 0;
    
    // Start/stop the capture thread
    void startCaptureThread();
    void stopCaptureThread();
    
private:
    void captureLoop();  // Runs in dedicated thread
    
    std::thread captureThread_;
    std::atomic<bool> running_;
    
    // Frame buffer (circular)
    std::vector<FrameBuffer> frameBuffer_;
    int bufferSize_ = 3;  // Default: 3 frames
    std::atomic<int> writeIndex_;
    std::atomic<int> readIndex_;
    std::mutex bufferMutex_;
    std::condition_variable frameAvailable_;
};
```

**File**: `src/cuems_videocomposer/cpp/input/LiveInputSource.cpp`

```cpp
void LiveInputSource::captureLoop() {
    FrameBuffer tempBuffer;
    
    while (running_) {
        // Capture frame from source (implemented by subclass)
        if (captureFrame(tempBuffer)) {
            // Add to circular buffer
            std::lock_guard<std::mutex> lock(bufferMutex_);
            int idx = writeIndex_ % bufferSize_;
            frameBuffer_[idx] = std::move(tempBuffer);
            writeIndex_++;
            frameAvailable_.notify_one();
        }
    }
}

bool LiveInputSource::readLatestFrame(FrameBuffer& buffer) {
    std::lock_guard<std::mutex> lock(bufferMutex_);
    if (writeIndex_ == 0) return false;  // No frames yet
    
    // Get latest frame (most recent write)
    int idx = (writeIndex_ - 1) % bufferSize_;
    buffer = frameBuffer_[idx];  // Copy
    return true;
}
```

### 3. Create NDIVideoInput Class (NDI SDK Direct)

**File**: `src/cuems_videocomposer/cpp/input/NDIVideoInput.h`

```cpp
#ifdef HAVE_NDI_SDK
#include <Processing.NDI.Lib.h>
#endif

class NDIVideoInput : public LiveInputSource {
public:
    NDIVideoInput();
    virtual ~NDIVideoInput();
    
    // InputSource interface
    bool open(const std::string& source) override;
    void close() override;
    bool isReady() const override;
    FrameInfo getFrameInfo() const override;
    int64_t getCurrentFrame() const override;
    CodecType detectCodec() const override;
    bool supportsDirectGPUTexture() const override { return false; }
    DecodeBackend getOptimalBackend() const override;
    
    // NDI-specific
    static std::vector<std::string> discoverSources(int timeoutMs = 5000);
    void setTallyState(bool onProgram, bool onPreview);
    
protected:
    // LiveInputSource interface
    bool captureFrame(FrameBuffer& buffer) override;
    
private:
    bool initializeNDI();
    void shutdownNDI();
    bool connectToSource(const std::string& sourceName);
    
#ifdef HAVE_NDI_SDK
    NDIlib_recv_instance_t ndiReceiver_;
    NDIlib_find_instance_t ndiFinder_;
#endif
    
    FrameInfo frameInfo_;
    std::string sourceName_;
    bool ready_;
    int64_t frameCount_;
};
```

**File**: `src/cuems_videocomposer/cpp/input/NDIVideoInput.cpp`

```cpp
bool NDIVideoInput::open(const std::string& source) {
    if (!initializeNDI()) return false;
    
    sourceName_ = source;
    
    // Create NDI receiver
    NDIlib_recv_create_v3_t recv_desc;
    recv_desc.source_to_connect_to.p_ndi_name = source.c_str();
    recv_desc.color_format = NDIlib_recv_color_format_RGBX_RGBA;  // RGBA format
    recv_desc.bandwidth = NDIlib_recv_bandwidth_highest;
    recv_desc.allow_video_fields = false;  // Progressive only
    
    ndiReceiver_ = NDIlib_recv_create_v3(&recv_desc);
    if (!ndiReceiver_) return false;
    
    // Wait for first frame to get format info
    NDIlib_video_frame_v2_t video_frame;
    if (NDIlib_recv_capture_v2(ndiReceiver_, &video_frame, nullptr, nullptr, 5000) 
        == NDIlib_frame_type_video) {
        frameInfo_.width = video_frame.xres;
        frameInfo_.height = video_frame.yres;
        frameInfo_.framerate = (double)video_frame.frame_rate_N / video_frame.frame_rate_D;
        NDIlib_recv_free_video_v2(ndiReceiver_, &video_frame);
    }
    
    ready_ = true;
    startCaptureThread();  // Start async capture
    return true;
}

bool NDIVideoInput::captureFrame(FrameBuffer& buffer) {
    NDIlib_video_frame_v2_t video_frame;
    
    // Non-blocking capture with short timeout
    auto type = NDIlib_recv_capture_v2(ndiReceiver_, &video_frame, nullptr, nullptr, 100);
    
    if (type == NDIlib_frame_type_video) {
        // Copy frame data to buffer
        buffer.allocate(video_frame.xres, video_frame.yres, PixelFormat::RGBA32);
        memcpy(buffer.data(), video_frame.p_data, buffer.size());
        
        NDIlib_recv_free_video_v2(ndiReceiver_, &video_frame);
        frameCount_++;
        return true;
    }
    
    return false;
}

std::vector<std::string> NDIVideoInput::discoverSources(int timeoutMs) {
    std::vector<std::string> sources;
    
    NDIlib_find_instance_t finder = NDIlib_find_create_v2();
    if (!finder) return sources;
    
    // Wait for sources
    std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
    
    uint32_t numSources = 0;
    const NDIlib_source_t* foundSources = NDIlib_find_get_current_sources(finder, &numSources);
    
    for (uint32_t i = 0; i < numSources; i++) {
        sources.push_back(foundSources[i].p_ndi_name);
    }
    
    NDIlib_find_destroy(finder);
    return sources;
}
```

### 4. Create FFmpegLiveInput for V4L2/RTSP (Stopgap)

**File**: `src/cuems_videocomposer/cpp/input/FFmpegLiveInput.h`

```cpp
/**
 * FFmpegLiveInput - Live input via FFmpeg (V4L2, RTSP, etc.)
 * 
 * Stopgap implementation using FFmpeg for live sources.
 * Later can be replaced with dedicated V4L2Input if needed.
 */
class FFmpegLiveInput : public LiveInputSource {
public:
    FFmpegLiveInput();
    virtual ~FFmpegLiveInput();
    
    // InputSource interface
    bool open(const std::string& source) override;
    void close() override;
    bool isReady() const override;
    FrameInfo getFrameInfo() const override;
    // ... etc
    
    // Set FFmpeg format (e.g., "v4l2", "rtsp")
    void setFormat(const std::string& format) { format_ = format; }
    
protected:
    bool captureFrame(FrameBuffer& buffer) override;
    
private:
    cuems_mediadecoder::MediaFileReader mediaReader_;
    cuems_mediadecoder::VideoDecoder videoDecoder_;
    std::string format_;
    // ... similar to VideoFileInput but simplified for live
};
```

### 5. Create createInputSource() Method

**File**: `src/cuems_videocomposer/cpp/VideoComposerApplication.cpp`

```cpp
std::unique_ptr<InputSource> VideoComposerApplication::createInputSource(
    const std::string& source) {
    
    // Detect source type and create appropriate input
    
    // NDI source (ndi://source_name or just source_name for NDI)
    if (source.find("ndi://") == 0 || isNDISource(source)) {
        auto ndiInput = std::make_unique<NDIVideoInput>();
        std::string ndiName = source;
        if (ndiName.find("ndi://") == 0) {
            ndiName = ndiName.substr(6);  // Remove prefix
        }
        if (ndiInput->open(ndiName)) {
            return ndiInput;
        }
        return nullptr;
    }
    
    // V4L2 device (/dev/video*)
    if (source.find("/dev/video") == 0) {
        auto v4l2Input = std::make_unique<FFmpegLiveInput>();
        v4l2Input->setFormat("v4l2");
        if (v4l2Input->open(source)) {
            return v4l2Input;
        }
        return nullptr;
    }
    
    // Network stream (rtsp://, http://, udp://)
    if (source.find("rtsp://") == 0 || 
        source.find("http://") == 0 ||
        source.find("udp://") == 0) {
        auto streamInput = std::make_unique<FFmpegLiveInput>();
        if (streamInput->open(source)) {
            return streamInput;
        }
        return nullptr;
    }
    
    // File path - use existing createInputSourceFromFile logic
    return createInputSourceFromFile(source);
}

bool VideoComposerApplication::isNDISource(const std::string& source) {
    // Check if source matches NDI naming pattern: "HOSTNAME (Source Name)"
    // Or query NDI SDK to see if it's a known source
    auto ndiSources = NDIVideoInput::discoverSources(1000);
    for (const auto& s : ndiSources) {
        if (s == source) return true;
    }
    return false;
}
```

### 6. Update LayerPlayback for Live Streams

**File**: `src/cuems_videocomposer/cpp/layer/LayerPlayback.cpp`

```cpp
bool LayerPlayback::loadFrame(int64_t frameNumber) {
    if (!inputSource_ || !inputSource_->isReady()) {
        return false;
    }
    
    // Check if this is a live stream
    if (inputSource_->isLiveStream()) {
        // Live streams: get latest available frame (ignore frameNumber)
        // The async buffer in LiveInputSource keeps frames ready
        if (inputSource_->readLatestFrame(cpuFrameBuffer_)) {
            frameOnGPU_ = false;
            return true;
        }
        return false;
    }
    
    // File-based sources: existing logic with frame number
    // ... existing code for HAP, VideoFileInput, etc.
}
```

### 7. Update CMakeLists.txt

```cmake
# NDI SDK detection
option(ENABLE_NDI "Enable NDI input/output support" ON)

if(ENABLE_NDI)
    # Find NDI SDK
    find_path(NDI_INCLUDE_DIR Processing.NDI.Lib.h
        PATHS 
            /usr/include
            /usr/local/include
            $ENV{NDI_SDK_DIR}/include
        PATH_SUFFIXES ndi)
    
    find_library(NDI_LIBRARY 
        NAMES ndi
        PATHS
            /usr/lib
            /usr/local/lib
            $ENV{NDI_SDK_DIR}/lib/x86_64-linux-gnu)
    
    if(NDI_INCLUDE_DIR AND NDI_LIBRARY)
        set(NDI_FOUND TRUE)
        add_definitions(-DHAVE_NDI_SDK)
        include_directories(${NDI_INCLUDE_DIR})
        message(STATUS "NDI SDK found: ${NDI_LIBRARY}")
    else()
        set(NDI_FOUND FALSE)
        message(WARNING "NDI SDK not found - NDI support disabled")
        message(STATUS "  Set NDI_SDK_DIR environment variable to NDI SDK location")
    endif()
endif()

# Live input sources
list(APPEND CPP_SOURCES
    src/cuems_videocomposer/cpp/input/LiveInputSource.cpp
    src/cuems_videocomposer/cpp/input/FFmpegLiveInput.cpp
)

if(NDI_FOUND)
    list(APPEND CPP_SOURCES
        src/cuems_videocomposer/cpp/input/NDIVideoInput.cpp
    )
    list(APPEND EXTRA_LIBS ${NDI_LIBRARY})
endif()
```

## Files to Create

| File | Purpose |

|------|---------|

| `cpp/input/LiveInputSource.h` | Abstract base class for all live inputs |

| `cpp/input/LiveInputSource.cpp` | Async capture thread and frame buffering |

| `cpp/input/NDIVideoInput.h` | NDI SDK direct input implementation |

| `cpp/input/NDIVideoInput.cpp` | NDI receive and frame capture |

| `cpp/input/FFmpegLiveInput.h` | FFmpeg-based live input (V4L2, RTSP) |

| `cpp/input/FFmpegLiveInput.cpp` | Live stream handling via FFmpeg |

## Files to Modify

| File | Changes |

|------|---------|

| `cpp/input/InputSource.h` | Add `isLiveStream()` and `readLatestFrame()` |

| `cpp/VideoComposerApplication.h/cpp` | Add `createInputSource()` method |

| `cpp/layer/LayerPlayback.cpp` | Handle live streams in `loadFrame()` |

| `CMakeLists.txt` | Add NDI SDK detection and live input sources |

## Testing Scenarios

1. **Single NDI input**: Layer with NDI source, verify sync
2. **Multiple inputs**: File + HAP + NDI + V4L2 on different layers
3. **NDI discovery**: List available NDI sources on network
4. **NDI reconnection**: Handle source disconnect/reconnect
5. **Buffer behavior**: Verify frame buffer prevents drops
6. **V4L2 webcam**: Basic webcam input via FFmpeg stopgap

## License Compliance Implementation

To comply with NDI SDK license requirements, the following must be implemented:

### 1. UI Attribution
- Add "NDI®" trademark symbol and attribution statement in:
  - About dialog
  - NDI source selection UI
  - Settings/preferences where NDI is mentioned
- Include link to [https://ndi.video/](https://ndi.video/) in:
  - NDI source selection dialog
  - Help/documentation menu
  - About dialog

### 2. Documentation
- Add NDI SDK license section to README
- Include NDI® trademark attribution
- Link to [ndi.video/tools](https://ndi.video/tools) for NDI tools
- Document NDI SDK requirements in build instructions

### 3. EULA/License File
- Include NDI SDK license terms in application's license file
- Add required disclaimers and restrictions
- Include copyright notice for NewTek, Inc.

### 4. Build System
- Ensure NDI SDK DLLs are copied to application directory (not system path)
- Include NDI SDK license file in distribution
- Document NDI SDK version and source in build output

## Future Enhancements

- Dedicated V4L2Input class (if FFmpeg latency is insufficient)
- NDI groups and metadata support
- Tally light integration (NDI → layer visibility indicator)
- Frame timestamp matching for better MTC sync on live inputs

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