# Multi-Display Implementation Plan

## DRM/KMS Multi-Monitor Backend (Primary) + Wayland (Future)

**Version:** 1.1

**Created:** November 2024

**Status:** Planning

**Estimated Total Effort:** 60-80 hours (DRM/KMS only)

---

## Implementation Priority

| Priority | Backend | Status | Notes |

|----------|---------|--------|-------|

| **1st** | DRM/KMS Direct | 🔴 To Implement | Lowest latency, production target |

| **2nd** | X11 Xinerama | ✅ Implemented | Legacy fallback |

| **Future** | Wayland layer-shell | ⏳ Deferred | For wlroots compositors |

| **Future** | Wayland XDG | ⏳ Deferred | Fallback for GNOME/KDE |

---

## Table of Contents

1. [Goals](#goals)
2. [Current State](#current-state)
3. [Architecture Overview](#architecture-overview)
4. [Phase 1: Display Configuration Manager](#phase-1-display-configuration-manager)
5. [Phase 2: DRM/KMS Backend](#phase-2-drmkms-backend)
6. [Phase 3: Multi-Output Rendering](#phase-3-multi-output-rendering)
7. [Phase 4: Configuration Persistence](#phase-4-configuration-persistence)
8. [Testing Matrix](#testing-matrix)
9. [Risk Assessment](#risk-assessment)
10. [Shared Components with NDI Plan](#shared-components-with-ndi-plan)
11. [Future: Wayland Support](#future-wayland-support)

---

## Goals

### Primary Goals

1. **Enumerate all connected displays** with full metadata:

   - Connector name (HDMI-A-1, DP-2, etc.)
   - Physical position (x, y in global coordinate space)
   - Resolution and refresh rate
   - Physical dimensions (for DPI calculation)
   - Make/model (from EDID)

2. **Support multiple display backends:**

   - **DRM/KMS direct** (production, lowest latency) ← **PRIMARY TARGET**
   - X11 Xinerama (legacy, already implemented)
   - Wayland layer-shell (future, for wlroots compositors)
   - Wayland XDG (future, fallback for GNOME/KDE)

3. **Per-output rendering:**

   - Route video layers to specific outputs
   - Independent resolution per output
   - Synchronized frame presentation

4. **Display configuration management:**

   - Set resolution/refresh per output
   - Configure output positions/arrangement
   - Save/load display configurations
   - Runtime reconfiguration

### Target Hardware

| Platform | Priority | Notes |

|----------|----------|-------|

| Intel N100/N101 | ⭐⭐⭐⭐⭐ | Primary development target |

| Intel i5/Ryzen iGPU | ⭐⭐⭐⭐⭐ | Primary development target |

| NVIDIA discrete | ⭐⭐⭐ | Secondary target |

---

## Current State

### What Exists

```
src/cuems_videocomposer/cpp/display/
├── DisplayBackend.h          # Abstract interface
├── DisplayManager.h/cpp      # Multi-display manager (X11 only)
├── XineramaHelper.h/cpp      # X11 Xinerama enumeration
├── X11Display.h/cpp          # X11 backend
├── WaylandDisplay.h/cpp      # Wayland backend (single window only)
└── OpenGLRenderer.h/cpp      # OpenGL rendering
```

### Current Limitations

| Component | Limitation |

|-----------|------------|

| `WaylandDisplay` | Single window, no multi-output |

| `WaylandDisplay` | `supportsMultiDisplay()` returns `false` |

| `WaylandDisplay` | No `wl_output` enumeration |

| `WaylandDisplay` | No layer-shell support |

| `DisplayManager` | X11/Xinerama only |

| No DRM backend | Not implemented |

### What Works

- X11 Xinerama multi-monitor detection ✅
- X11 window positioning per-screen ✅
- VAAPI zero-copy (needs testing) ✅
- HAP playback ✅
- OpenGL rendering ✅

### Related Plans

| Plan | Status | Integration |

|------|--------|-------------|

| `ndi-output-support.plan.md` | Planning | OutputSink, FrameCapture, HeadlessDisplay |

| `rendering-optimizations.plan.md` | In Progress | PBO, VAAPI zero-copy |

| `vaapi.plan.md` | Completed | Hardware decoding |

---

## Architecture Overview

### Unified Output System

The architecture unifies **physical display outputs** (monitors, projectors) with **virtual outputs** (NDI, streaming, recording) under a common framework. This enables:

- Rendering to multiple physical displays simultaneously
- Capturing rendered frames for NDI/streaming output
- Headless operation (no physical display, NDI only)
- Mixed mode (physical displays + NDI capture)

### Complete System Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        VideoComposerApplication                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌──────────────┐    ┌───────────────────┐    ┌─────────────────────────┐  │
│  │ LayerManager │───▶│ MultiOutputRenderer│───▶│ Physical Display Output │  │
│  │              │    │                   │    │  (per-output rendering)  │  │
│  └──────────────┘    └─────────┬─────────┘    └─────────────────────────┘  │
│                                │                                             │
│                                │ (frame capture from primary/composite)      │
│                                ▼                                             │
│                      ┌─────────────────┐                                    │
│                      │  FrameCapture   │                                    │
│                      │  (PBO Async)    │                                    │
│                      └────────┬────────┘                                    │
│                               │                                              │
│                               ▼                                              │
│                    ┌──────────────────────┐                                 │
│                    │  OutputSinkManager   │                                 │
│                    │  (virtual outputs)   │                                 │
│                    ├──────────────────────┤                                 │
│                    │ ┌────────┐ ┌───────┐ │                                 │
│                    │ │NDIOut  │ │Stream │ │  (Future: File, HW capture)    │
│                    │ └────────┘ └───────┘ │                                 │
│                    └──────────────────────┘                                 │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Display Backend Hierarchy

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         DisplayConfiguration                                 │
│  Global display settings, output arrangement, persistence                    │
├─────────────────────────────────────────────────────────────────────────────┤
│  + loadConfig(path)                                                          │
│  + saveConfig(path)                                                          │
│  + getOutputConfig(name) → OutputConfig                                      │
│  + setOutputConfig(name, OutputConfig)                                       │
│  + getOutputArrangement() → vector<OutputPlacement>                          │
└─────────────────────────────────────────────────────────────────────────────┘
                                       │
                                       ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                            OutputManager                                     │
│  Enumerates outputs, creates surfaces, manages lifecycle                     │
├─────────────────────────────────────────────────────────────────────────────┤
│  + detectOutputs() → vector<OutputInfo>                                      │
│  + getOutput(index) → OutputInfo                                             │
│  + getOutputByName(name) → OutputInfo                                        │
│  + createSurface(outputIndex) → OutputSurface*                               │
│  + setOutputMode(index, width, height, refresh)                              │
│  + onHotplug(callback)                                                       │
└────────────────────────────────┬────────────────────────────────────────────┘
                                 │
       ┌─────────────────────────┼─────────────────────────┐
       │                         │                         │
       ▼                         ▼                         ▼
┌──────────────────┐   ┌──────────────────┐   ┌──────────────────┐
│  DRMOutputMgr    │   │ WaylandOutputMgr │   │  X11OutputMgr    │
├──────────────────┤   ├──────────────────┤   ├──────────────────┤
│ - drm_fd         │   │ - wl_outputs[]   │   │ - xinerama       │
│ - connectors[]   │   │ - xdg_outputs[]  │   │ - display        │
│ - crtcs[]        │   │ - layer_shell    │   │                  │
│ - gbm_device     │   │ - xdg_wm_base    │   │                  │
└──────────────────┘   └──────────────────┘   └──────────────────┘
       │                         │                         │
       ▼                         ▼                         ▼
┌──────────────────┐   ┌──────────────────┐   ┌──────────────────┐
│ DRMSurface       │   │ WaylandSurface   │   │ X11Surface       │
│ (per-output)     │   │ (per-output)     │   │ (per-output)     │
├──────────────────┤   ├──────────────────┤   ├──────────────────┤
│ - gbm_surface    │   │ - wl_surface     │   │ - window         │
│ - egl_surface    │   │ - layer_surface  │   │ - glx_ctx        │
│ - framebuffers   │   │ - egl_window     │   │                  │
│ - page_flip      │   │ - xdg_toplevel   │   │                  │
└──────────────────┘   └──────────────────┘   └──────────────────┘
```

### Virtual Output System (from NDI Plan)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                            OutputSinkManager                                 │
│  Manages all virtual outputs (NDI, streaming, recording)                     │
├─────────────────────────────────────────────────────────────────────────────┤
│  + addSink(OutputSink*) → bool                                               │
│  + removeSink(id) → bool                                                     │
│  + writeFrameToAll(FrameData&)                                               │
│  + hasActiveSinks() → bool                                                   │
│  + getActiveSinkIds() → vector<string>                                       │
└────────────────────────────────┬────────────────────────────────────────────┘
                                 │
       ┌─────────────────────────┼─────────────────────────┐
       │                         │                         │
       ▼                         ▼                         ▼
┌──────────────────┐   ┌──────────────────┐   ┌──────────────────┐
│  NDIVideoOutput  │   │  StreamOutput    │   │  FileOutput      │
│  (NDI SDK)       │   │  (Future)        │   │  (Future)        │
├──────────────────┤   ├──────────────────┤   ├──────────────────┤
│ - ndiSender_     │   │ - rtspServer     │   │ - muxer          │
│ - frameQueue_    │   │ - webrtc         │   │ - encoder        │
│ - encodingThread │   │                  │   │                  │
└──────────────────┘   └──────────────────┘   └──────────────────┘
```

### Headless Display Backend

For server deployments with no physical display:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           HeadlessDisplay                                    │
│  EGL+GBM rendering without compositor                                        │
├─────────────────────────────────────────────────────────────────────────────┤
│  + init() → bool                                                             │
│  + createSurface(width, height) → HeadlessSurface*                           │
│  + makeCurrent()                                                             │
│  + render(LayerManager*)                                                     │
│  + getFrameCapture() → FrameCapture*                                         │
├─────────────────────────────────────────────────────────────────────────────┤
│  - drmFd_          // DRM device (for GBM)                                   │
│  - gbmDevice_      // GBM buffer allocator                                   │
│  - eglDisplay_     // EGL display on GBM                                     │
│  - eglContext_     // OpenGL context                                         │
│  - offscreenFBO_   // Render target                                          │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Frame Capture Integration

```cpp
// FrameCapture integrates with MultiOutputRenderer
class FrameCapture {
public:
    enum class CaptureSource {
        PRIMARY_OUTPUT,      // Capture from primary display output
        COMPOSITE_FBO,       // Capture from internal composited framebuffer
        SPECIFIC_OUTPUT      // Capture from a specific output index
    };
    
    bool initialize(int width, int height, PixelFormat format);
    void setCaptureSource(CaptureSource source, int outputIndex = 0);
    
    // Called after render, before swap
    void startCapture();                           // Initiate async PBO read
    bool getCompletedFrame(FrameData& frame);      // Get previous frame
    
    bool hasPBOSupport() const;
    
private:
    GLuint pbo_[2];           // Double-buffered PBOs
    int currentPBO_;
    CaptureSource source_;
    int captureOutputIndex_;
};
```

### Operating Modes

The unified architecture supports multiple operating modes:

| Mode | Physical Displays | Virtual Outputs | Use Case |

|------|------------------|-----------------|----------|

| **Standard** | 1+ outputs | None | Traditional playback |

| **Multi-Display** | 2+ outputs | None | Multi-projector show |

| **NDI Output** | 1 output | NDI | Preview + streaming |

| **Headless NDI** | None | NDI | Server-side encoding |

| **Multi + NDI** | 2+ outputs | NDI | Show + broadcast |

### Configuration Integration

```json
{
    "name": "Show Setup with NDI",
    "displays": {
        "outputs": [
            {
                "name": "HDMI-A-1",
                "mode": {"width": 1920, "height": 1080, "refresh": 60.0},
                "layers": [0, 1],
                "captureForNDI": true
            },
            {
                "name": "HDMI-A-2",
                "mode": {"width": 1920, "height": 1080, "refresh": 60.0},
                "layers": [0, 1]
            }
        ]
    },
    "virtualOutputs": {
        "ndi": {
            "enabled": true,
            "sourceName": "CUEMS VideoComposer",
            "captureSource": "PRIMARY_OUTPUT",
            "resolution": {"width": 1920, "height": 1080},
            "frameRate": 60
        }
    },
    "headless": false
}
```

### Proposed Class Hierarchy (Combined)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        DisplayConfiguration                                  │
│  Global display settings, output arrangement, persistence                    │
├─────────────────────────────────────────────────────────────────────────────┤
│  + loadConfig(path)                                                          │
│  + saveConfig(path)                                                          │
│  + getOutputConfig(name) → OutputConfig                                      │
│  + setOutputConfig(name, OutputConfig)                                       │
│  + getOutputArrangement() → vector<OutputPlacement>                          │
│  + getVirtualOutputConfig() → VirtualOutputConfig                            │
│  + isHeadless() → bool                                                       │
└─────────────────────────────────────────────────────────────────────────────┘
                                       │
              ┌────────────────────────┴────────────────────────┐
              │                                                 │
              ▼                                                 ▼
┌──────────────────────────────┐             ┌──────────────────────────────┐
│       OutputManager          │             │    OutputSinkManager         │
│   (physical displays)        │             │    (virtual outputs)         │
├──────────────────────────────┤             ├──────────────────────────────┤
│  DRM / Wayland / X11         │             │  NDI / Stream / File         │
│  Multi-output rendering      │             │  Async encoding threads      │
└──────────────────────────────┘             └──────────────────────────────┘
              │                                                 ▲
              │                                                 │
              │         ┌──────────────────────┐                │
              └────────▶│ MultiOutputRenderer  │────────────────┘
                        │ + FrameCapture       │
                        └──────────────────────┘
```

### Data Structures

```cpp
// Common output information
struct OutputInfo {
    std::string name;           // "HDMI-A-1", "DP-2", etc.
    std::string make;           // "Samsung", "Dell", etc. (from EDID)
    std::string model;          // "U28E590" (from EDID)
    std::string serialNumber;   // For unique identification
    
    int32_t x, y;               // Position in global coordinate space
    int32_t width, height;      // Current resolution
    int32_t physicalWidth;      // Physical width in mm
    int32_t physicalHeight;     // Physical height in mm
    double refreshRate;         // Current refresh rate
    int32_t scale;              // HiDPI scale factor
    
    bool connected;
    bool enabled;
    int32_t index;              // Internal index
    
    // Available modes
    struct Mode {
        int32_t width, height;
        double refreshRate;
        bool preferred;
    };
    std::vector<Mode> availableModes;
};

// Per-output configuration
struct OutputConfig {
    std::string outputName;     // Match by name
    int32_t x, y;               // Desired position
    int32_t width, height;      // Desired resolution
    double refreshRate;         // Desired refresh
    bool enabled;               // Enable/disable output
    int32_t rotation;           // 0, 90, 180, 270
    
    // Layer routing
    std::vector<int> assignedLayers;  // Layer IDs routed here
    
    // Projector blending
    struct BlendRegion {
        float left, right, top, bottom;  // 0.0 - 0.5 typical
        float gamma;
    } blend;
    
    // Geometry correction
    bool warpEnabled;
    std::string warpMeshPath;
};

// Global display arrangement
struct DisplayArrangement {
    std::vector<OutputConfig> outputs;
    std::string name;           // "Default", "Show Setup 1", etc.
    bool valid;
};
```

---

## Phase 1: Display Configuration Manager

### Goal

Create a unified configuration system for display settings that works across all backends.

### Duration: 8-10 hours

### Files to Create

```
src/cuems_videocomposer/cpp/display/
├── DisplayConfiguration.h
├── DisplayConfiguration.cpp
├── OutputInfo.h              # Shared data structures
└── OutputConfig.h            # Configuration structures
```

### Implementation

#### 1.1 OutputInfo and OutputConfig Structures (2h)

```cpp
// OutputInfo.h
#ifndef VIDEOCOMPOSER_OUTPUTINFO_H
#define VIDEOCOMPOSER_OUTPUTINFO_H

#include <string>
#include <vector>
#include <cstdint>

namespace videocomposer {

struct OutputMode {
    int32_t width;
    int32_t height;
    double refreshRate;
    bool preferred;
    
    std::string toString() const;
};

struct OutputInfo {
    // Identification
    std::string name;           // "HDMI-A-1", "DP-2"
    std::string make;           // From EDID
    std::string model;          // From EDID
    std::string serialNumber;
    
    // Current state
    int32_t x, y;               // Position in global coords
    int32_t width, height;      // Current resolution
    int32_t physicalWidthMM;    // Physical dimensions
    int32_t physicalHeightMM;
    double refreshRate;
    int32_t scale;              // 1, 2, etc. for HiDPI
    
    // Status
    bool connected;
    bool enabled;
    int32_t index;
    
    // Capabilities
    std::vector<OutputMode> modes;
    
    // Helpers
    double getDPI() const;
    const OutputMode* getPreferredMode() const;
    const OutputMode* findMode(int w, int h, double refresh = 0) const;
    std::string getDisplayName() const;  // "make model (name)"
};

} // namespace videocomposer

#endif
```

#### 1.2 DisplayConfiguration Class (4h)

```cpp
// DisplayConfiguration.h
#ifndef VIDEOCOMPOSER_DISPLAYCONFIGURATION_H
#define VIDEOCOMPOSER_DISPLAYCONFIGURATION_H

#include "OutputInfo.h"
#include <string>
#include <vector>
#include <map>
#include <functional>

namespace videocomposer {

struct BlendRegion {
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;
    float gamma = 2.2f;
};

struct OutputConfig {
    std::string outputName;
    int32_t x = 0, y = 0;
    int32_t width = 0, height = 0;
    double refreshRate = 0;
    bool enabled = true;
    int32_t rotation = 0;
    
    // Layer routing (-1 = show on all outputs)
    std::vector<int> assignedLayers;
    
    // Blending
    BlendRegion blend;
    bool blendEnabled = false;
    
    // Warping
    bool warpEnabled = false;
    std::string warpMeshPath;
};

struct DisplayArrangement {
    std::string name;
    std::vector<OutputConfig> outputs;
    bool autoDetect = true;  // Auto-configure new outputs
};

class DisplayConfiguration {
public:
    DisplayConfiguration();
    ~DisplayConfiguration();
    
    // Load/save
    bool loadFromFile(const std::string& path);
    bool saveToFile(const std::string& path) const;
    bool loadFromString(const std::string& json);
    std::string saveToString() const;
    
    // Arrangement management
    void setArrangement(const DisplayArrangement& arrangement);
    const DisplayArrangement& getArrangement() const;
    
    // Per-output config
    OutputConfig* getOutputConfig(const std::string& outputName);
    const OutputConfig* getOutputConfig(const std::string& outputName) const;
    void setOutputConfig(const std::string& outputName, const OutputConfig& config);
    
    // Layer routing
    std::vector<std::string> getOutputsForLayer(int layerId) const;
    void assignLayerToOutput(int layerId, const std::string& outputName);
    void assignLayerToAllOutputs(int layerId);
    
    // Apply to outputs
    using ConfigApplier = std::function<bool(const std::string&, const OutputConfig&)>;
    bool applyToOutputs(ConfigApplier applier) const;
    
    // Auto-configuration
    OutputConfig createDefaultConfig(const OutputInfo& info) const;
    void autoConfigureNewOutput(const OutputInfo& info);
    
private:
    DisplayArrangement arrangement_;
    std::map<std::string, OutputConfig> outputConfigs_;
    std::string configPath_;
};

} // namespace videocomposer

#endif
```

#### 1.3 JSON Serialization (2h)

```cpp
// In DisplayConfiguration.cpp
#include <nlohmann/json.hpp>

bool DisplayConfiguration::loadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        LOG_WARNING << "Could not open display config: " << path;
        return false;
    }
    
    try {
        nlohmann::json j;
        file >> j;
        
        arrangement_.name = j.value("name", "Default");
        arrangement_.autoDetect = j.value("autoDetect", true);
        
        for (const auto& output : j["outputs"]) {
            OutputConfig config;
            config.outputName = output["name"];
            config.x = output.value("x", 0);
            config.y = output.value("y", 0);
            config.width = output.value("width", 0);
            config.height = output.value("height", 0);
            config.refreshRate = output.value("refresh", 0.0);
            config.enabled = output.value("enabled", true);
            config.rotation = output.value("rotation", 0);
            
            if (output.contains("layers")) {
                for (const auto& layer : output["layers"]) {
                    config.assignedLayers.push_back(layer);
                }
            }
            
            if (output.contains("blend")) {
                config.blendEnabled = true;
                config.blend.left = output["blend"].value("left", 0.0f);
                config.blend.right = output["blend"].value("right", 0.0f);
                config.blend.top = output["blend"].value("top", 0.0f);
                config.blend.bottom = output["blend"].value("bottom", 0.0f);
                config.blend.gamma = output["blend"].value("gamma", 2.2f);
            }
            
            arrangement_.outputs.push_back(config);
            outputConfigs_[config.outputName] = config;
        }
        
        configPath_ = path;
        LOG_INFO << "Loaded display configuration: " << arrangement_.name 
                 << " (" << arrangement_.outputs.size() << " outputs)";
        return true;
        
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to parse display config: " << e.what();
        return false;
    }
}
```

#### 1.4 Example Configuration File

```json
{
    "name": "3-Projector Setup",
    "autoDetect": false,
    "outputs": [
        {
            "name": "HDMI-A-1",
            "x": 0,
            "y": 0,
            "width": 1920,
            "height": 1080,
            "refresh": 60.0,
            "enabled": true,
            "layers": [0, 1],
            "blend": {
                "right": 0.15,
                "gamma": 2.2
            }
        },
        {
            "name": "HDMI-A-2",
            "x": 1680,
            "y": 0,
            "width": 1920,
            "height": 1080,
            "refresh": 60.0,
            "enabled": true,
            "layers": [0, 1],
            "blend": {
                "left": 0.15,
                "right": 0.15,
                "gamma": 2.2
            }
        },
        {
            "name": "DP-1",
            "x": 3360,
            "y": 0,
            "width": 1920,
            "height": 1080,
            "refresh": 60.0,
            "enabled": true,
            "layers": [0, 1],
            "blend": {
                "left": 0.15,
                "gamma": 2.2
            }
        }
    ]
}
```

---

## Future: Wayland Support

> ⏳ **DEFERRED** - The following Wayland phases are planned for future implementation after DRM/KMS is stable and tested. They provide an alternative display backend for systems running Wayland compositors.

---

## Future Phase A: Wayland Output Enumeration

> ⏳ **DEFERRED** - This phase is planned for future implementation after DRM/KMS is stable.

### Goal

Enumerate Wayland outputs with full metadata using `wl_output` and `xdg_output_manager_v1`.

### Duration: 10-12 hours

### Prerequisites

- `wayland-protocols` package
- Generate protocol headers

### Files to Create/Modify

```
src/cuems_videocomposer/cpp/display/
├── wayland/
│   ├── WaylandOutputManager.h
│   ├── WaylandOutputManager.cpp
│   ├── xdg-output-unstable-v1-client-protocol.h  (generated)
│   └── xdg-output-unstable-v1-client-protocol.c  (generated)
└── WaylandDisplay.cpp  (modify)
```

### Implementation

#### 2.1 Generate Protocol Headers (1h)

```bash
# Add to CMakeLists.txt or run manually:
find_program(WAYLAND_SCANNER wayland-scanner)

set(XDG_OUTPUT_PROTOCOL "/usr/share/wayland-protocols/unstable/xdg-output/xdg-output-unstable-v1.xml")

add_custom_command(
    OUTPUT xdg-output-unstable-v1-client-protocol.h
    COMMAND ${WAYLAND_SCANNER} client-header ${XDG_OUTPUT_PROTOCOL} 
            ${CMAKE_CURRENT_BINARY_DIR}/xdg-output-unstable-v1-client-protocol.h
    DEPENDS ${XDG_OUTPUT_PROTOCOL}
)

add_custom_command(
    OUTPUT xdg-output-unstable-v1-client-protocol.c
    COMMAND ${WAYLAND_SCANNER} private-code ${XDG_OUTPUT_PROTOCOL}
            ${CMAKE_CURRENT_BINARY_DIR}/xdg-output-unstable-v1-client-protocol.c
    DEPENDS ${XDG_OUTPUT_PROTOCOL}
)
```

#### 2.2 WaylandOutputManager Class (6h)

```cpp
// WaylandOutputManager.h
#ifndef VIDEOCOMPOSER_WAYLANDOUTPUTMANAGER_H
#define VIDEOCOMPOSER_WAYLANDOUTPUTMANAGER_H

#include "../OutputInfo.h"
#include <wayland-client.h>
#include <vector>
#include <map>
#include <functional>
#include <mutex>

struct zxdg_output_manager_v1;
struct zxdg_output_v1;

namespace videocomposer {

class WaylandOutputManager {
public:
    WaylandOutputManager();
    ~WaylandOutputManager();
    
    // Initialize from wl_display (call after wl_display_connect)
    bool init(wl_display* display);
    void cleanup();
    
    // Output enumeration
    const std::vector<OutputInfo>& getOutputs() const { return outputs_; }
    const OutputInfo* getOutput(int index) const;
    const OutputInfo* getOutputByName(const std::string& name) const;
    wl_output* getWlOutput(int index) const;
    wl_output* getWlOutput(const std::string& name) const;
    
    // Wait for all output info to be received
    bool waitForOutputs(int timeoutMs = 5000);
    
    // Hotplug notification
    using HotplugCallback = std::function<void(const OutputInfo&, bool connected)>;
    void setHotplugCallback(HotplugCallback callback);
    
    // Registry handlers (public for C callbacks)
    void handleRegistryGlobal(wl_registry* registry, uint32_t name,
                              const char* interface, uint32_t version);
    void handleRegistryGlobalRemove(wl_registry* registry, uint32_t name);
    
    // Output event handlers
    void handleOutputGeometry(wl_output* output, int32_t x, int32_t y,
                              int32_t physW, int32_t physH, int32_t subpixel,
                              const char* make, const char* model, int32_t transform);
    void handleOutputMode(wl_output* output, uint32_t flags,
                          int32_t width, int32_t height, int32_t refresh);
    void handleOutputScale(wl_output* output, int32_t scale);
    void handleOutputName(wl_output* output, const char* name);
    void handleOutputDone(wl_output* output);
    
    // XDG output handlers
    void handleXdgOutputLogicalPosition(zxdg_output_v1* xdgOutput, int32_t x, int32_t y);
    void handleXdgOutputLogicalSize(zxdg_output_v1* xdgOutput, int32_t width, int32_t height);
    void handleXdgOutputName(zxdg_output_v1* xdgOutput, const char* name);
    void handleXdgOutputDone(zxdg_output_v1* xdgOutput);
    
private:
    struct PendingOutput {
        wl_output* wlOutput = nullptr;
        zxdg_output_v1* xdgOutput = nullptr;
        uint32_t registryName = 0;
        OutputInfo info;
        bool geometryReceived = false;
        bool modeReceived = false;
        bool xdgDone = false;
        bool done = false;
    };
    
    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    zxdg_output_manager_v1* xdgOutputManager_ = nullptr;
    
    std::map<wl_output*, PendingOutput> pendingOutputs_;
    std::vector<OutputInfo> outputs_;
    std::map<std::string, size_t> outputsByName_;
    
    HotplugCallback hotplugCallback_;
    std::mutex mutex_;
    bool initialized_ = false;
    int outputsReady_ = 0;
    
    void finalizeOutput(wl_output* output);
    static std::string makeConnectorName(const PendingOutput& pending);
};

} // namespace videocomposer

#endif
```

#### 2.3 WaylandOutputManager Implementation (5h)

```cpp
// WaylandOutputManager.cpp
#include "WaylandOutputManager.h"
#include "../utils/Logger.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include <cstring>
#include <algorithm>

namespace videocomposer {

// C callback wrappers
static void registry_global(void* data, wl_registry* registry,
                           uint32_t name, const char* interface, uint32_t version) {
    auto* mgr = static_cast<WaylandOutputManager*>(data);
    mgr->handleRegistryGlobal(registry, name, interface, version);
}

static void registry_global_remove(void* data, wl_registry* registry, uint32_t name) {
    auto* mgr = static_cast<WaylandOutputManager*>(data);
    mgr->handleRegistryGlobalRemove(registry, name);
}

static const wl_registry_listener registry_listener = {
    registry_global,
    registry_global_remove
};

// wl_output listener
static void output_geometry(void* data, wl_output* output, int32_t x, int32_t y,
                           int32_t pw, int32_t ph, int32_t subpixel,
                           const char* make, const char* model, int32_t transform) {
    auto* mgr = static_cast<WaylandOutputManager*>(data);
    mgr->handleOutputGeometry(output, x, y, pw, ph, subpixel, make, model, transform);
}

static void output_mode(void* data, wl_output* output, uint32_t flags,
                       int32_t width, int32_t height, int32_t refresh) {
    auto* mgr = static_cast<WaylandOutputManager*>(data);
    mgr->handleOutputMode(output, flags, width, height, refresh);
}

static void output_scale(void* data, wl_output* output, int32_t scale) {
    auto* mgr = static_cast<WaylandOutputManager*>(data);
    mgr->handleOutputScale(output, scale);
}

static void output_name(void* data, wl_output* output, const char* name) {
    auto* mgr = static_cast<WaylandOutputManager*>(data);
    mgr->handleOutputName(output, name);
}

static void output_done(void* data, wl_output* output) {
    auto* mgr = static_cast<WaylandOutputManager*>(data);
    mgr->handleOutputDone(output);
}

static const wl_output_listener output_listener = {
    output_geometry,
    output_mode,
    nullptr,  // done (deprecated in v2, replaced with output_done event)
    output_scale,
    output_name,  // wl_output v4+
    nullptr,       // description (v4+)
};

// xdg_output listener
static void xdg_output_logical_position(void* data, zxdg_output_v1* xdg,
                                        int32_t x, int32_t y) {
    auto* mgr = static_cast<WaylandOutputManager*>(data);
    mgr->handleXdgOutputLogicalPosition(xdg, x, y);
}

static void xdg_output_logical_size(void* data, zxdg_output_v1* xdg,
                                    int32_t w, int32_t h) {
    auto* mgr = static_cast<WaylandOutputManager*>(data);
    mgr->handleXdgOutputLogicalSize(xdg, w, h);
}

static void xdg_output_done(void* data, zxdg_output_v1* xdg) {
    auto* mgr = static_cast<WaylandOutputManager*>(data);
    mgr->handleXdgOutputDone(xdg);
}

static void xdg_output_name(void* data, zxdg_output_v1* xdg, const char* name) {
    auto* mgr = static_cast<WaylandOutputManager*>(data);
    mgr->handleXdgOutputName(xdg, name);
}

static const zxdg_output_v1_listener xdg_output_listener = {
    xdg_output_logical_position,
    xdg_output_logical_size,
    xdg_output_done,
    xdg_output_name,
    nullptr  // description
};

// Implementation
WaylandOutputManager::WaylandOutputManager() = default;

WaylandOutputManager::~WaylandOutputManager() {
    cleanup();
}

bool WaylandOutputManager::init(wl_display* display) {
    if (!display) {
        LOG_ERROR << "WaylandOutputManager: null display";
        return false;
    }
    
    display_ = display;
    registry_ = wl_display_get_registry(display_);
    if (!registry_) {
        LOG_ERROR << "WaylandOutputManager: failed to get registry";
        return false;
    }
    
    wl_registry_add_listener(registry_, &registry_listener, this);
    
    // First roundtrip to get globals
    wl_display_roundtrip(display_);
    
    // Second roundtrip to get output events
    wl_display_roundtrip(display_);
    
    initialized_ = true;
    LOG_INFO << "WaylandOutputManager: initialized with " << outputs_.size() << " outputs";
    
    return true;
}

void WaylandOutputManager::cleanup() {
    for (auto& [wlOutput, pending] : pendingOutputs_) {
        if (pending.xdgOutput) {
            zxdg_output_v1_destroy(pending.xdgOutput);
        }
        if (pending.wlOutput) {
            wl_output_destroy(pending.wlOutput);
        }
    }
    pendingOutputs_.clear();
    
    if (xdgOutputManager_) {
        zxdg_output_manager_v1_destroy(xdgOutputManager_);
        xdgOutputManager_ = nullptr;
    }
    
    if (registry_) {
        wl_registry_destroy(registry_);
        registry_ = nullptr;
    }
    
    outputs_.clear();
    outputsByName_.clear();
    initialized_ = false;
}

void WaylandOutputManager::handleRegistryGlobal(wl_registry* registry, uint32_t name,
                                                 const char* interface, uint32_t version) {
    if (strcmp(interface, "wl_output") == 0) {
        // Bind to wl_output (version 4 for name event)
        uint32_t bindVersion = std::min(version, 4u);
        wl_output* output = static_cast<wl_output*>(
            wl_registry_bind(registry, name, &wl_output_interface, bindVersion));
        
        PendingOutput pending;
        pending.wlOutput = output;
        pending.registryName = name;
        pending.info.connected = true;
        pendingOutputs_[output] = pending;
        
        wl_output_add_listener(output, &output_listener, this);
        
        // If we have xdg_output_manager, get xdg_output for this output
        if (xdgOutputManager_) {
            zxdg_output_v1* xdgOutput = zxdg_output_manager_v1_get_xdg_output(
                xdgOutputManager_, output);
            pendingOutputs_[output].xdgOutput = xdgOutput;
            zxdg_output_v1_add_listener(xdgOutput, &xdg_output_listener, this);
        }
        
        LOG_INFO << "WaylandOutputManager: found output (registry " << name << ")";
    }
    else if (strcmp(interface, "zxdg_output_manager_v1") == 0) {
        xdgOutputManager_ = static_cast<zxdg_output_manager_v1*>(
            wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface,
                            std::min(version, 3u)));
        LOG_INFO << "WaylandOutputManager: bound to xdg_output_manager_v1";
        
        // Get xdg_output for any outputs we already have
        for (auto& [wlOutput, pending] : pendingOutputs_) {
            if (!pending.xdgOutput) {
                pending.xdgOutput = zxdg_output_manager_v1_get_xdg_output(
                    xdgOutputManager_, wlOutput);
                zxdg_output_v1_add_listener(pending.xdgOutput, 
                                            &xdg_output_listener, this);
            }
        }
    }
}

void WaylandOutputManager::handleRegistryGlobalRemove(wl_registry* registry, 
                                                       uint32_t name) {
    // Find output by registry name
    for (auto it = pendingOutputs_.begin(); it != pendingOutputs_.end(); ++it) {
        if (it->second.registryName == name) {
            LOG_INFO << "WaylandOutputManager: output removed: " 
                    << it->second.info.name;
            
            // Notify callback
            if (hotplugCallback_) {
                it->second.info.connected = false;
                hotplugCallback_(it->second.info, false);
            }
            
            // Remove from outputs list
            auto nameIt = outputsByName_.find(it->second.info.name);
            if (nameIt != outputsByName_.end()) {
                outputs_.erase(outputs_.begin() + nameIt->second);
                outputsByName_.erase(nameIt);
                
                // Rebuild index map
                outputsByName_.clear();
                for (size_t i = 0; i < outputs_.size(); ++i) {
                    outputsByName_[outputs_[i].name] = i;
                }
            }
            
            // Cleanup Wayland objects
            if (it->second.xdgOutput) {
                zxdg_output_v1_destroy(it->second.xdgOutput);
            }
            wl_output_destroy(it->second.wlOutput);
            
            pendingOutputs_.erase(it);
            break;
        }
    }
}

void WaylandOutputManager::handleOutputGeometry(wl_output* output, 
                                                 int32_t x, int32_t y,
                                                 int32_t physW, int32_t physH,
                                                 int32_t subpixel,
                                                 const char* make, const char* model,
                                                 int32_t transform) {
    auto it = pendingOutputs_.find(output);
    if (it == pendingOutputs_.end()) return;
    
    auto& info = it->second.info;
    info.x = x;
    info.y = y;
    info.physicalWidthMM = physW;
    info.physicalHeightMM = physH;
    info.make = make ? make : "";
    info.model = model ? model : "";
    it->second.geometryReceived = true;
}

void WaylandOutputManager::handleOutputMode(wl_output* output, uint32_t flags,
                                            int32_t width, int32_t height,
                                            int32_t refresh) {
    auto it = pendingOutputs_.find(output);
    if (it == pendingOutputs_.end()) return;
    
    OutputMode mode;
    mode.width = width;
    mode.height = height;
    mode.refreshRate = refresh / 1000.0;  // mHz to Hz
    mode.preferred = (flags & WL_OUTPUT_MODE_PREFERRED) != 0;
    
    it->second.info.modes.push_back(mode);
    
    // If this is the current mode, set as current resolution
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        it->second.info.width = width;
        it->second.info.height = height;
        it->second.info.refreshRate = mode.refreshRate;
        it->second.modeReceived = true;
    }
}

void WaylandOutputManager::handleOutputScale(wl_output* output, int32_t scale) {
    auto it = pendingOutputs_.find(output);
    if (it == pendingOutputs_.end()) return;
    
    it->second.info.scale = scale;
}

void WaylandOutputManager::handleOutputName(wl_output* output, const char* name) {
    auto it = pendingOutputs_.find(output);
    if (it == pendingOutputs_.end()) return;
    
    // This is the connector name (HDMI-A-1, etc.) from wl_output v4
    it->second.info.name = name ? name : "";
}

void WaylandOutputManager::handleOutputDone(wl_output* output) {
    auto it = pendingOutputs_.find(output);
    if (it == pendingOutputs_.end()) return;
    
    it->second.done = true;
    
    // If we're not waiting for xdg_output, finalize now
    if (!xdgOutputManager_ || it->second.xdgDone) {
        finalizeOutput(output);
    }
}

void WaylandOutputManager::handleXdgOutputLogicalPosition(zxdg_output_v1* xdg,
                                                          int32_t x, int32_t y) {
    // Find the output this xdg_output belongs to
    for (auto& [wlOutput, pending] : pendingOutputs_) {
        if (pending.xdgOutput == xdg) {
            // Use logical position (accounts for scale)
            pending.info.x = x;
            pending.info.y = y;
            break;
        }
    }
}

void WaylandOutputManager::handleXdgOutputLogicalSize(zxdg_output_v1* xdg,
                                                       int32_t w, int32_t h) {
    // Logical size (scaled) - we mostly care about raw resolution
    // but store for reference
}

void WaylandOutputManager::handleXdgOutputName(zxdg_output_v1* xdg, 
                                                const char* name) {
    for (auto& [wlOutput, pending] : pendingOutputs_) {
        if (pending.xdgOutput == xdg) {
            // xdg_output name is the connector name (preferred over wl_output name)
            if (name && strlen(name) > 0) {
                pending.info.name = name;
            }
            break;
        }
    }
}

void WaylandOutputManager::handleXdgOutputDone(zxdg_output_v1* xdg) {
    for (auto& [wlOutput, pending] : pendingOutputs_) {
        if (pending.xdgOutput == xdg) {
            pending.xdgDone = true;
            
            // If wl_output is also done, finalize
            if (pending.done) {
                finalizeOutput(wlOutput);
            }
            break;
        }
    }
}

void WaylandOutputManager::finalizeOutput(wl_output* output) {
    auto it = pendingOutputs_.find(output);
    if (it == pendingOutputs_.end()) return;
    
    auto& pending = it->second;
    auto& info = pending.info;
    
    // Generate connector name if not received
    if (info.name.empty()) {
        info.name = makeConnectorName(pending);
    }
    
    info.index = static_cast<int32_t>(outputs_.size());
    info.enabled = true;
    
    // Add to outputs list
    outputs_.push_back(info);
    outputsByName_[info.name] = outputs_.size() - 1;
    
    LOG_INFO << "WaylandOutputManager: output ready: " << info.name
            << " (" << info.width << "x" << info.height << "@" 
            << info.refreshRate << "Hz at " << info.x << "," << info.y << ")";
    
    // Notify callback
    if (hotplugCallback_) {
        hotplugCallback_(info, true);
    }
}

std::string WaylandOutputManager::makeConnectorName(const PendingOutput& pending) {
    // Generate a name like "Unknown-1" if we didn't get one
    static int unknownCount = 0;
    return "Unknown-" + std::to_string(++unknownCount);
}

const OutputInfo* WaylandOutputManager::getOutput(int index) const {
    if (index < 0 || index >= static_cast<int>(outputs_.size())) {
        return nullptr;
    }
    return &outputs_[index];
}

const OutputInfo* WaylandOutputManager::getOutputByName(const std::string& name) const {
    auto it = outputsByName_.find(name);
    if (it != outputsByName_.end()) {
        return &outputs_[it->second];
    }
    return nullptr;
}

wl_output* WaylandOutputManager::getWlOutput(int index) const {
    int i = 0;
    for (const auto& [wlOutput, pending] : pendingOutputs_) {
        if (i == index) return wlOutput;
        i++;
    }
    return nullptr;
}

wl_output* WaylandOutputManager::getWlOutput(const std::string& name) const {
    for (const auto& [wlOutput, pending] : pendingOutputs_) {
        if (pending.info.name == name) return wlOutput;
    }
    return nullptr;
}

void WaylandOutputManager::setHotplugCallback(HotplugCallback callback) {
    hotplugCallback_ = std::move(callback);
}

} // namespace videocomposer
```

---

## Phase 2: DRM/KMS Backend

### Goal

Implement direct DRM/KMS rendering for lowest-latency production use. This is the **primary display backend** for cuems-videocomposer.

### Duration: 20-25 hours

### Files to Create

```
src/cuems_videocomposer/cpp/display/drm/
├── DRMBackend.h
├── DRMBackend.cpp
├── DRMOutputManager.h
├── DRMOutputManager.cpp
├── DRMSurface.h
├── DRMSurface.cpp
└── DRMAtomic.h        # Atomic modesetting helpers
```

### Implementation

#### 3.1 DRMOutputManager (8h)

```cpp
// DRMOutputManager.h
#ifndef VIDEOCOMPOSER_DRMOUTPUTMANAGER_H
#define VIDEOCOMPOSER_DRMOUTPUTMANAGER_H

#include "../OutputInfo.h"
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <vector>
#include <map>
#include <string>

namespace videocomposer {

struct DRMConnector {
    uint32_t connectorId;
    uint32_t encoderId;
    uint32_t crtcId;
    drmModeConnector* connector;  // Owned, must free
    drmModeCrtc* savedCrtc;       // Original mode (for restore)
    OutputInfo info;
};

class DRMOutputManager {
public:
    DRMOutputManager();
    ~DRMOutputManager();
    
    // Initialize with DRM device
    bool init(const std::string& devicePath = "");  // Empty = auto-detect
    void cleanup();
    
    // Get DRM fd for other operations
    int getFd() const { return drmFd_; }
    
    // Output enumeration
    const std::vector<OutputInfo>& getOutputs() const;
    const DRMConnector* getConnector(int index) const;
    const DRMConnector* getConnectorByName(const std::string& name) const;
    
    // Mode setting
    bool setMode(int index, int width, int height, double refreshRate);
    bool setMode(const std::string& name, int width, int height, double refreshRate);
    
    // Atomic operations
    bool supportsAtomic() const { return atomicSupported_; }
    drmModeAtomicReq* createAtomicRequest();
    bool commitAtomic(drmModeAtomicReq* request, uint32_t flags);
    
    // Get CRTC for output
    uint32_t getCrtcId(int index) const;
    
    // Find a free CRTC for connector
    uint32_t findCrtcForConnector(const drmModeConnector* connector);
    
    // Get property IDs
    uint32_t getPropertyId(uint32_t objectId, uint32_t objectType, 
                          const std::string& name);
    
private:
    bool openDRMDevice(const std::string& path);
    bool detectOutputs();
    std::string getConnectorName(const drmModeConnector* conn);
    void readEDID(DRMConnector& connector);
    
    int drmFd_ = -1;
    std::string devicePath_;
    bool atomicSupported_ = false;
    
    drmModeRes* resources_ = nullptr;
    std::vector<DRMConnector> connectors_;
    std::vector<OutputInfo> outputs_;
    std::map<std::string, size_t> outputsByName_;
    
    // Track used CRTCs
    std::map<uint32_t, uint32_t> crtcToConnector_;
};

} // namespace videocomposer

#endif
```

#### 3.2 DRMSurface - Per-Output Rendering (8h)

```cpp
// DRMSurface.h
#ifndef VIDEOCOMPOSER_DRMSURFACE_H
#define VIDEOCOMPOSER_DRMSURFACE_H

#include "../OutputInfo.h"
#include <gbm.h>
#include <EGL/egl.h>
#include <xf86drmMode.h>
#include <vector>

namespace videocomposer {

class DRMOutputManager;

class DRMSurface {
public:
    DRMSurface(DRMOutputManager* outputManager, int outputIndex);
    ~DRMSurface();
    
    // Initialize GBM + EGL for this output
    bool init();
    void cleanup();
    
    // Rendering
    bool beginFrame();
    void endFrame();
    
    // Page flipping
    bool schedulePageFlip();
    void waitForFlip();
    
    // Get output info
    const OutputInfo& getOutputInfo() const;
    uint32_t getWidth() const { return width_; }
    uint32_t getHeight() const { return height_; }
    
    // EGL/OpenGL access
    void makeCurrent();
    void releaseCurrent();
    EGLContext getContext() const { return eglContext_; }
    
private:
    struct Framebuffer {
        gbm_bo* bo = nullptr;
        uint32_t fbId = 0;
    };
    
    bool createFramebuffer(gbm_bo* bo, Framebuffer& fb);
    void destroyFramebuffer(Framebuffer& fb);
    
    static void pageFlipHandler(int fd, unsigned int frame, unsigned int sec,
                                unsigned int usec, void* data);
    
    DRMOutputManager* outputManager_;
    int outputIndex_;
    uint32_t width_, height_;
    
    // GBM
    gbm_device* gbmDevice_ = nullptr;
    gbm_surface* gbmSurface_ = nullptr;
    
    // EGL
    EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
    EGLContext eglContext_ = EGL_NO_CONTEXT;
    EGLSurface eglSurface_ = EGL_NO_SURFACE;
    EGLConfig eglConfig_ = nullptr;
    
    // Framebuffers
    std::vector<Framebuffer> framebuffers_;
    Framebuffer* currentFb_ = nullptr;
    Framebuffer* nextFb_ = nullptr;
    
    // Flip state
    bool flipPending_ = false;
    bool initialized_ = false;
};

} // namespace videocomposer

#endif
```

#### 3.3 DRMBackend - Main Backend Class (6h)

```cpp
// DRMBackend.h
#ifndef VIDEOCOMPOSER_DRMBACKEND_H
#define VIDEOCOMPOSER_DRMBACKEND_H

#include "../DisplayBackend.h"
#include "DRMOutputManager.h"
#include "DRMSurface.h"
#include <vector>
#include <memory>

namespace videocomposer {

class DRMBackend : public DisplayBackend {
public:
    DRMBackend();
    virtual ~DRMBackend();
    
    // DisplayBackend interface
    bool openWindow() override;
    void closeWindow() override;
    bool isWindowOpen() const override;
    void render(LayerManager* layerManager, OSDManager* osdManager = nullptr) override;
    void handleEvents() override;
    void resize(unsigned int width, unsigned int height) override;
    void getWindowSize(unsigned int* width, unsigned int* height) const override;
    void setPosition(int x, int y) override;
    void getWindowPos(int* x, int* y) const override;
    void setFullscreen(int action) override;
    bool getFullscreen() const override;
    void setOnTop(int action) override;
    bool getOnTop() const override;
    bool supportsMultiDisplay() const override { return true; }
    void* getContext() override;
    void makeCurrent() override;
    void clearCurrent() override;
    OpenGLRenderer* getRenderer() override;
    
    // DRM-specific
    const std::vector<OutputInfo>& getOutputs() const;
    DRMSurface* getSurface(int index);
    DRMSurface* getSurface(const std::string& name);
    
    // Mode setting
    bool setOutputMode(int index, int width, int height, double refresh);
    
private:
    std::unique_ptr<DRMOutputManager> outputManager_;
    std::vector<std::unique_ptr<DRMSurface>> surfaces_;
    std::unique_ptr<OpenGLRenderer> renderer_;
    
    bool initialized_ = false;
    int primaryOutput_ = 0;
};

} // namespace videocomposer

#endif
```

#### 3.4 VT (Virtual Terminal) Handling

```cpp
// VT switching for console takeover
class VTHandler {
public:
    bool acquireVT();
    void releaseVT();
    void setVTSwitchCallback(std::function<void(bool acquired)> callback);
    
private:
    int ttyFd_ = -1;
    bool vtAcquired_ = false;
};
```

---

## Future Phase B: Wayland Layer-Shell

> ⏳ **DEFERRED** - This phase is planned for future implementation after DRM/KMS is stable.

### Goal

Implement `wlr-layer-shell` for exclusive fullscreen surfaces on wlroots compositors.

### Duration: 12-15 hours

### Files to Create

```
src/cuems_videocomposer/cpp/display/wayland/
├── WaylandLayerShell.h
├── WaylandLayerShell.cpp
├── WaylandSurface.h
├── WaylandSurface.cpp
├── wlr-layer-shell-unstable-v1-client-protocol.h  (generated)
└── wlr-layer-shell-unstable-v1-client-protocol.c  (generated)
```

### Implementation

#### 4.1 Generate Protocol Headers

```bash
# Download wlr-protocols
git clone https://gitlab.freedesktop.org/wlroots/wlr-protocols.git

# Generate headers
wayland-scanner client-header \
    wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml \
    wlr-layer-shell-unstable-v1-client-protocol.h

wayland-scanner private-code \
    wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml \
    wlr-layer-shell-unstable-v1-client-protocol.c
```

#### 4.2 WaylandSurface - Per-Output Surface (8h)

```cpp
// WaylandSurface.h
#ifndef VIDEOCOMPOSER_WAYLANDSURFACE_H
#define VIDEOCOMPOSER_WAYLANDSURFACE_H

#include "../OutputInfo.h"
#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>

struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;

namespace videocomposer {

class WaylandOutputManager;

enum class SurfaceType {
    XDG_TOPLEVEL,     // Standard window (fallback)
    LAYER_SHELL       // wlr-layer-shell (preferred for wlroots)
};

class WaylandSurface {
public:
    WaylandSurface(wl_display* display, wl_compositor* compositor,
                   wl_output* output, const OutputInfo& outputInfo,
                   SurfaceType type = SurfaceType::LAYER_SHELL);
    ~WaylandSurface();
    
    bool init(zwlr_layer_shell_v1* layerShell, EGLDisplay eglDisplay,
              EGLConfig eglConfig, EGLContext sharedContext);
    void cleanup();
    
    // Rendering
    void makeCurrent();
    void releaseCurrent();
    void swapBuffers();
    
    // Surface state
    bool isConfigured() const { return configured_; }
    void waitForConfigure();
    
    // Output info
    const OutputInfo& getOutputInfo() const { return outputInfo_; }
    uint32_t getWidth() const { return width_; }
    uint32_t getHeight() const { return height_; }
    
    // Layer-shell callbacks
    void onLayerSurfaceConfigure(uint32_t serial, uint32_t width, uint32_t height);
    void onLayerSurfaceClosed();
    
    // XDG callbacks (fallback)
    void onXdgSurfaceConfigure(uint32_t serial);
    void onXdgToplevelConfigure(int32_t width, int32_t height, wl_array* states);
    void onXdgToplevelClose();
    
private:
    bool createLayerShellSurface(zwlr_layer_shell_v1* layerShell);
    bool createXdgSurface();
    bool createEGLSurface(EGLDisplay eglDisplay, EGLConfig eglConfig,
                          EGLContext sharedContext);
    
    wl_display* display_;
    wl_compositor* compositor_;
    wl_output* wlOutput_;
    OutputInfo outputInfo_;
    SurfaceType surfaceType_;
    
    wl_surface* surface_ = nullptr;
    wl_egl_window* eglWindow_ = nullptr;
    
    // Layer-shell (preferred)
    zwlr_layer_surface_v1* layerSurface_ = nullptr;
    
    // XDG (fallback)
    struct xdg_surface* xdgSurface_ = nullptr;
    struct xdg_toplevel* xdgToplevel_ = nullptr;
    
    // EGL
    EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
    EGLContext eglContext_ = EGL_NO_CONTEXT;
    EGLSurface eglSurface_ = EGL_NO_SURFACE;
    
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool configured_ = false;
    bool closed_ = false;
};

} // namespace videocomposer

#endif
```

#### 4.3 Layer-Shell Surface Creation

```cpp
bool WaylandSurface::createLayerShellSurface(zwlr_layer_shell_v1* layerShell) {
    if (!layerShell) {
        LOG_WARNING << "Layer shell not available, falling back to XDG";
        surfaceType_ = SurfaceType::XDG_TOPLEVEL;
        return createXdgSurface();
    }
    
    // Create layer surface on the OVERLAY layer (above all windows)
    layerSurface_ = zwlr_layer_shell_v1_get_layer_surface(
        layerShell,
        surface_,
        wlOutput_,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        "cuems-videocomposer"
    );
    
    if (!layerSurface_) {
        LOG_ERROR << "Failed to create layer surface";
        return false;
    }
    
    // Request exclusive fullscreen
    zwlr_layer_surface_v1_set_size(layerSurface_, 0, 0);  // Use output size
    zwlr_layer_surface_v1_set_exclusive_zone(layerSurface_, -1);  // Exclusive
    zwlr_layer_surface_v1_set_anchor(layerSurface_,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | 
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | 
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    
    // Request keyboard interactivity (optional)
    zwlr_layer_surface_v1_set_keyboard_interactivity(layerSurface_,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    
    // Add listener
    static const zwlr_layer_surface_v1_listener listener = {
        [](void* data, zwlr_layer_surface_v1*, uint32_t serial, 
           uint32_t w, uint32_t h) {
            static_cast<WaylandSurface*>(data)->onLayerSurfaceConfigure(serial, w, h);
        },
        [](void* data, zwlr_layer_surface_v1*) {
            static_cast<WaylandSurface*>(data)->onLayerSurfaceClosed();
        }
    };
    zwlr_layer_surface_v1_add_listener(layerSurface_, &listener, this);
    
    // Commit to trigger configure
    wl_surface_commit(surface_);
    
    LOG_INFO << "Created layer-shell surface for output " << outputInfo_.name;
    return true;
}

void WaylandSurface::onLayerSurfaceConfigure(uint32_t serial, 
                                              uint32_t width, uint32_t height) {
    zwlr_layer_surface_v1_ack_configure(layerSurface_, serial);
    
    // Use output size if compositor didn't specify
    if (width == 0) width = outputInfo_.width;
    if (height == 0) height = outputInfo_.height;
    
    if (width_ != width || height_ != height) {
        width_ = width;
        height_ = height;
        
        if (eglWindow_) {
            wl_egl_window_resize(eglWindow_, width, height, 0, 0);
        }
    }
    
    configured_ = true;
    LOG_INFO << "Layer surface configured: " << width << "x" << height;
}
```

---

## Phase 3: Multi-Output Rendering

### Goal

Implement rendering pipeline that draws to multiple outputs with layer routing, integrated with FrameCapture for virtual outputs (NDI, streaming).

### Duration: 15-20 hours

### Files to Create/Modify

```
src/cuems_videocomposer/cpp/display/
├── MultiOutputRenderer.h
├── MultiOutputRenderer.cpp
├── OpenGLRenderer.cpp  (modify for multi-output)
└── CompositeFramebuffer.h/cpp  (for NDI capture)

src/cuems_videocomposer/cpp/output/   (shared with NDI plan)
├── FrameCapture.h
├── FrameCapture.cpp
├── OutputSink.h
└── OutputSinkManager.h/cpp
```

### Integration with Output System

The `MultiOutputRenderer` connects physical display rendering with virtual output capture:

```
┌─────────────────────────────────────────────────────────────────────┐
│                      MultiOutputRenderer                             │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    Composite FBO                              │   │
│  │   (Internal framebuffer for NDI/stream capture)              │   │
│  └─────────────────────┬───────────────────────────────────────┘   │
│                        │                                             │
│         ┌──────────────┼──────────────┐                             │
│         │              │              │                              │
│         ▼              ▼              ▼                              │
│    ┌─────────┐   ┌─────────┐   ┌─────────────┐                      │
│    │Output 0 │   │Output 1 │   │FrameCapture │                      │
│    │(DRM/WL) │   │(DRM/WL) │   │(PBO Async)  │                      │
│    └─────────┘   └─────────┘   └──────┬──────┘                      │
│         │              │              │                              │
│         ▼              ▼              ▼                              │
│    ┌─────────┐   ┌─────────┐   ┌─────────────┐                      │
│    │HDMI-A-1 │   │HDMI-A-2 │   │OutputSinkMgr│                      │
│    │(monitor)│   │(monitor)│   │(NDI/Stream) │                      │
│    └─────────┘   └─────────┘   └─────────────┘                      │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

### Implementation

#### 5.1 MultiOutputRenderer Class

```cpp
// MultiOutputRenderer.h
#ifndef VIDEOCOMPOSER_MULTIOUTPUTRENDERER_H
#define VIDEOCOMPOSER_MULTIOUTPUTRENDERER_H

#include "OutputInfo.h"
#include "OutputConfig.h"
#include "OpenGLRenderer.h"
#include "../layer/LayerManager.h"
#include "../osd/OSDManager.h"
#include <vector>
#include <memory>
#include <functional>

namespace videocomposer {

class OutputSurface;  // Abstract base for DRM/Wayland surfaces
class BlendShader;
class WarpMesh;

struct LayerRenderInfo {
    int layerId;
    Rect sourceRegion;      // Region of video to show
    Rect destRegion;        // Where on output to render
    float opacity;
    BlendMode blendMode;
};

class FrameCapture;      // From output/ module (shared with NDI plan)
class OutputSinkManager; // From output/ module (shared with NDI plan)

class MultiOutputRenderer {
public:
    MultiOutputRenderer();
    ~MultiOutputRenderer();
    
    // Initialize with list of output surfaces
    bool init(const std::vector<OutputSurface*>& surfaces);
    void cleanup();
    
    // Configuration
    void setLayerMapping(int layerId, const std::string& outputName,
                        const Rect& source, const Rect& dest);
    void setLayerMapping(int layerId, int outputIndex,
                        const Rect& source, const Rect& dest);
    void clearLayerMapping(int layerId);
    void assignLayerToAllOutputs(int layerId);
    
    // Blending configuration
    void setBlendRegion(int outputIndex, const BlendRegion& blend);
    void setWarpMesh(int outputIndex, std::shared_ptr<WarpMesh> mesh);
    
    // Virtual output integration (NDI, streaming, etc.)
    void setOutputSinkManager(OutputSinkManager* sinkManager);
    void setCaptureEnabled(bool enabled);
    void setCaptureSource(int outputIndex);  // -1 = composite FBO
    void setCaptureResolution(int width, int height);
    
    // Render all outputs
    void render(LayerManager* layerManager, OSDManager* osdManager = nullptr);
    
    // Render single output (for async/threaded rendering)
    void renderOutput(int outputIndex, LayerManager* layerManager,
                      OSDManager* osdManager = nullptr);
    
    // Synchronize all outputs (for DRM atomic)
    void presentAll();
    
private:
    struct OutputState {
        OutputSurface* surface = nullptr;
        std::vector<LayerRenderInfo> layerMappings;
        BlendRegion blend;
        std::shared_ptr<WarpMesh> warpMesh;
        std::unique_ptr<BlendShader> blendShader;
        bool needsBlending = false;
        bool needsWarping = false;
        bool captureEnabled = false;  // Capture this output for virtual outputs
    };
    
    std::vector<OutputState> outputs_;
    std::unique_ptr<OpenGLRenderer> renderer_;
    std::map<int, std::vector<int>> layerToOutputs_;  // layerId -> output indices
    
    // Virtual output capture (shared with NDI plan)
    std::unique_ptr<FrameCapture> frameCapture_;
    OutputSinkManager* outputSinkManager_ = nullptr;  // Not owned
    GLuint compositeFBO_ = 0;       // For capturing composite of all outputs
    GLuint compositeTexture_ = 0;
    int captureWidth_ = 0;
    int captureHeight_ = 0;
    int captureSourceIndex_ = -1;   // -1 = composite FBO
    bool captureEnabled_ = false;
    
    void renderLayersToOutput(OutputState& output, LayerManager* layerManager);
    void applyBlending(OutputState& output);
    void applyWarping(OutputState& output);
    void captureForVirtualOutputs();  // Capture frame for NDI/streaming
    void renderToCompositeFBO(LayerManager* layerManager);
};

} // namespace videocomposer

#endif
```

#### 5.2 Render Loop with Frame Capture

```cpp
void MultiOutputRenderer::render(LayerManager* layerManager, 
                                  OSDManager* osdManager) {
    // Step 1: Render to composite FBO if needed for virtual outputs
    if (captureEnabled_ && captureSourceIndex_ < 0 && outputSinkManager_) {
        renderToCompositeFBO(layerManager);
    }
    
    // Step 2: Render to each physical output
    for (size_t i = 0; i < outputs_.size(); ++i) {
        auto& output = outputs_[i];
        
        // Make this output's context current
        output.surface->makeCurrent();
        
        // Set viewport
        glViewport(0, 0, output.surface->getWidth(), output.surface->getHeight());
        
        // Clear to black
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        
        // Render layers assigned to this output
        renderLayersToOutput(output, layerManager);
        
        // Apply edge blending if configured
        if (output.needsBlending) {
            applyBlending(output);
        }
        
        // Apply warping if configured  
        if (output.needsWarping) {
            applyWarping(output);
        }
        
        // Render OSD if on this output
        // (usually only on primary output)
        if (osdManager && i == 0) {
            // ... render OSD
        }
        
        // Capture this output if configured for virtual outputs
        if (captureEnabled_ && captureSourceIndex_ == static_cast<int>(i)) {
            captureForVirtualOutputs();
        }
        
        output.surface->releaseCurrent();
    }
    
    // Step 3: Capture from composite FBO if that's the source
    if (captureEnabled_ && captureSourceIndex_ < 0 && outputSinkManager_) {
        captureForVirtualOutputs();
    }
    
    // Step 4: Present all outputs (synchronized for DRM atomic)
    presentAll();
}

void MultiOutputRenderer::renderLayersToOutput(OutputState& output,
                                                LayerManager* layerManager) {
    // Get layers sorted by z-order
    auto layers = layerManager->getLayersSortedByZOrder();
    
    for (const auto& mapping : output.layerMappings) {
        VideoLayer* layer = layerManager->getLayer(mapping.layerId);
        if (!layer || !layer->isVisible()) continue;
        
        // Get prepared frame
        auto* frame = layer->getPreparedFrame();
        if (!frame) continue;
        
        // Render with source/dest regions
        renderer_->renderLayerRegion(
            layer,
            mapping.sourceRegion,
            mapping.destRegion,
            mapping.opacity,
            mapping.blendMode
        );
    }
}

void MultiOutputRenderer::renderToCompositeFBO(LayerManager* layerManager) {
    // Bind composite FBO
    glBindFramebuffer(GL_FRAMEBUFFER, compositeFBO_);
    glViewport(0, 0, captureWidth_, captureHeight_);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Render all layers to composite FBO (ignoring per-output routing)
    auto layers = layerManager->getLayersSortedByZOrder();
    for (auto* layer : layers) {
        if (!layer || !layer->isVisible()) continue;
        auto* frame = layer->getPreparedFrame();
        if (!frame) continue;
        
        renderer_->renderLayer(layer);
    }
    
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void MultiOutputRenderer::captureForVirtualOutputs() {
    if (!frameCapture_ || !outputSinkManager_) return;
    
    // Start async capture (non-blocking)
    frameCapture_->startCapture();
    
    // Get previously completed frame and send to virtual outputs
    FrameData completedFrame;
    if (frameCapture_->getCompletedFrame(completedFrame)) {
        outputSinkManager_->writeFrameToAll(completedFrame);
    }
}

void MultiOutputRenderer::presentAll() {
    // For DRM: use atomic commit for synchronized flip
    // For Wayland: swap buffers on each surface
    
    for (auto& output : outputs_) {
        output.surface->swapBuffers();
    }
}
```

#### 5.3 Headless Mode Integration

For server deployments without physical displays:

```cpp
// In VideoComposerApplication
void VideoComposerApplication::initializeHeadlessMode() {
    // Create headless display (EGL+GBM, no physical output)
    headlessDisplay_ = std::make_unique<HeadlessDisplay>();
    if (!headlessDisplay_->init()) {
        LOG_ERROR << "Failed to initialize headless display";
        return;
    }
    
    // Create MultiOutputRenderer with no physical surfaces
    multiOutputRenderer_ = std::make_unique<MultiOutputRenderer>();
    multiOutputRenderer_->init({});  // Empty surface list
    
    // Set up capture for NDI output
    multiOutputRenderer_->setOutputSinkManager(outputSinkManager_.get());
    multiOutputRenderer_->setCaptureEnabled(true);
    multiOutputRenderer_->setCaptureResolution(1920, 1080);
    
    // Create NDI output sink
    auto ndiOutput = std::make_unique<NDIVideoOutput>();
    ndiOutput->open("CUEMS VideoComposer", outputConfig_);
    outputSinkManager_->addSink(std::move(ndiOutput));
}

void VideoComposerApplication::renderHeadless() {
    headlessDisplay_->makeCurrent();
    
    // Render to composite FBO
    multiOutputRenderer_->render(layerManager_.get(), nullptr);
    
    // No swap buffers needed for headless
}
```

---

## Phase 4: Configuration Persistence

### Goal

Save and load display configurations, support runtime reconfiguration.

### Duration: 6-8 hours

### Implementation

#### 6.1 OSC Commands for Display Configuration

```cpp
// Add to OSCHandler.cpp

// List outputs
// /videocomposer/display/list -> returns JSON
void handleDisplayList() {
    auto& outputs = displayManager_->getOutputs();
    json j = json::array();
    for (const auto& output : outputs) {
        j.push_back({
            {"name", output.name},
            {"width", output.width},
            {"height", output.height},
            {"x", output.x},
            {"y", output.y},
            {"refresh", output.refreshRate},
            {"enabled", output.enabled}
        });
    }
    sendOSCReply("/videocomposer/display/list/reply", j.dump());
}

// Set output mode
// /videocomposer/display/mode <name> <width> <height> <refresh>
void handleDisplayMode(const std::string& name, int w, int h, double refresh) {
    if (displayManager_->setOutputMode(name, w, h, refresh)) {
        LOG_INFO << "Set mode " << name << " to " << w << "x" << h << "@" << refresh;
    }
}

// Assign layer to output
// /videocomposer/display/assign <layerId> <outputName>
void handleDisplayAssign(int layerId, const std::string& outputName) {
    multiOutputRenderer_->setLayerMapping(layerId, outputName, 
                                          Rect::fullFrame(), Rect::fullFrame());
}

// Set blend region
// /videocomposer/display/blend <outputName> <left> <right> <top> <bottom>
void handleDisplayBlend(const std::string& name, float l, float r, float t, float b);

// Save/load config
// /videocomposer/display/save <path>
// /videocomposer/display/load <path>
```

#### 6.2 Configuration File Format

```json
{
    "$schema": "cuems-display-config-v1",
    "name": "Main Show Setup",
    "outputs": [
        {
            "name": "HDMI-A-1",
            "position": {"x": 0, "y": 0},
            "mode": {"width": 1920, "height": 1080, "refresh": 60.0},
            "enabled": true,
            "layers": [0, 1, 2],
            "blend": {
                "enabled": true,
                "right": 0.15,
                "gamma": 2.2
            },
            "warp": {
                "enabled": false,
                "meshPath": ""
            }
        }
    ],
    "defaultLayerAssignment": "all"
}
```

---

## Testing Matrix

### Hardware Configurations to Test

| Configuration | Platform | Outputs | Priority |

|---------------|----------|---------|----------|

| Intel N100 Mini PC | Low-end | 2x HDMI | ⭐⭐⭐⭐⭐ |

| Intel N100 Mini PC | Low-end | HDMI + DP | ⭐⭐⭐⭐⭐ |

| Intel i5 Desktop | Mid-range | 3x DP | ⭐⭐⭐⭐ |

| AMD Ryzen Laptop | Mid-range | HDMI + USB-C | ⭐⭐⭐⭐ |

| NVIDIA GTX 1660 | High-end | 3x HDMI | ⭐⭐⭐ |

| NVIDIA RTX 3060 | High-end | 4x DP | ⭐⭐⭐ |

### Test Scenarios

| Test | Description | Expected |

|------|-------------|----------|

| Output enumeration | Detect all connected outputs | Names, resolutions, positions |

| Hotplug | Connect/disconnect display | Callback fired, list updated |

| Mode setting | Change resolution | Output updated, no crash |

| Multi-output render | 2 outputs, different content | Correct layer routing |

| Synchronized flip | 2 outputs, same content | No tearing between outputs |

| Edge blending | 2 adjacent projectors | Smooth blend in overlap |

| Layer routing | Layer 0→output 0, Layer 1→output 1 | Correct routing |

### DRM/KMS Backend Testing

| Test | Description | Expected |

|------|-------------|----------|

| DRM device open | Open /dev/dri/cardX | Success with video group |

| Connector enumeration | Detect HDMI/DP/VGA | All connectors listed |

| EDID parsing | Read monitor make/model | Correct identification |

| Mode enumeration | List available modes | All modes per output |

| Atomic modesetting | Set resolution | No flickering |

| GBM surface creation | Create render target | Valid gbm_surface |

| EGL context | Create GL context | OpenGL 3.3+ available |

| Page flip | Double buffering | Vsync, no tearing |

| Multi-CRTC | 2+ outputs same time | All outputs active |

### Compositor Testing (Wayland) - FUTURE

> ⏳ **DEFERRED** - Test after Wayland phases are implemented.

| Compositor | Layer-Shell | XDG Fallback | Priority |

|------------|-------------|--------------|----------|

| Sway | ✅ Test | ✅ Test | ⭐⭐⭐⭐⭐ |

| Hyprland | ✅ Test | ✅ Test | ⭐⭐⭐⭐ |

| GNOME | ❌ N/A | ✅ Test | ⭐⭐⭐ |

| KDE | ❌ N/A | ✅ Test | ⭐⭐⭐ |

---

## Risk Assessment

### DRM/KMS Risks (Current Focus)

| Risk | Impact | Probability | Mitigation |

|------|--------|-------------|------------|

| DRM requires root/video group | Setup complexity | High | Document, add udev rules |

| VT switching issues | Black screen | Medium | Graceful fallback, logging |

| NVIDIA driver quirks | DRM issues | Medium | Test specific versions, prefer Intel/AMD |

| Memory leaks in hotplug | Stability | Low | Careful cleanup, valgrind |

| EGL context sharing | Crashes | Medium | Per-output contexts |

| CRTC allocation conflicts | Some outputs fail | Low | Proper CRTC selection algorithm |

| Missing /dev/dri access | App won't start | High | Clear error messages, docs |

### Wayland Risks (Future)

| Risk | Impact | Probability | Mitigation |

|------|--------|-------------|------------|

| Compositor differences | Inconsistent behavior | Medium | Test matrix, fallbacks |

| Layer-shell not supported | Limited to XDG | Medium | Automatic fallback |

| GNOME/KDE restrictions | Can't go fullscreen properly | Medium | Use DRM/KMS for production |

---

## Implementation Schedule (DRM/KMS First)

### Week 1-2: Foundation + DRM Output Manager

| Task | Hours | Dependencies |

|------|-------|--------------|

| Phase 1: DisplayConfiguration | 8-10 | None |

| Phase 2: DRM Output Manager | 8 | Phase 1 |

### Week 3-4: DRM Backend Core

| Task | Hours | Dependencies |

|------|-------|--------------|

| Phase 2: DRM Surface + EGL | 8 | DRM Output Manager |

| Phase 2: DRM Backend class | 6 | DRM Surface |

### Week 5-6: Multi-Output Rendering

| Task | Hours | Dependencies |

|------|-------|--------------|

| Phase 3: Multi-Output Renderer | 15-20 | Phase 2 |

| Phase 3: FrameCapture integration | 5 | Multi-Output Renderer |

### Week 7: Configuration + Polish

| Task | Hours | Dependencies |

|------|-------|--------------|

| Phase 4: Configuration Persistence | 6-8 | Phase 3 |

| Testing + Bug fixes | 8 | All phases |

### Total (DRM/KMS only): 60-73 hours (7-9 weeks at ~8h/week)

### Future (Wayland): 22-27 hours additional

| Task | Hours | Dependencies |

|------|-------|--------------|

| Future Phase A: Wayland Output Enumeration | 10-12 | Phase 1 |

| Future Phase B: Wayland Layer-Shell | 12-15 | Future Phase A |

---

## Quick Start (DRM/KMS Path)

For fastest path to multi-display with DRM/KMS:

1. **Week 1-2:** Implement `DRMOutputManager` - Get output enumeration working via DRM
2. **Week 3:** Create `DRMSurface` with GBM + EGL - Single output first
3. **Week 4:** Multi-output surfaces - Render same content to all outputs
4. **Week 5:** Layer routing - Different content per output
5. **Week 6-7:** Configuration + Testing

This gets multi-output working with DRM/KMS (lowest latency, production ready). Wayland can be added later for compositor integration if needed.

---

## Shared Components with NDI Plan

This section documents the components shared between the Multi-Display Implementation and the NDI Output Support plans. These should be implemented once and used by both features.

### Shared Files

| File | Purpose | Used By |

|------|---------|---------|

| `cpp/output/OutputSink.h` | Abstract interface for all virtual outputs | NDI, Streaming, File |

| `cpp/output/OutputSinkManager.h/cpp` | Manages multiple output sinks | Both plans |

| `cpp/output/FrameCapture.h/cpp` | PBO-based async GPU→CPU capture | Both plans |

| `cpp/display/HeadlessDisplay.h/cpp` | EGL+GBM headless rendering | Both plans |

| `cpp/display/OutputInfo.h` | Common output info structures | Both plans |

### FrameCapture Class (Shared)

```cpp
// cpp/output/FrameCapture.h
#ifndef VIDEOCOMPOSER_FRAMECAPTURE_H
#define VIDEOCOMPOSER_FRAMECAPTURE_H

#include <GL/gl.h>
#include <queue>
#include <mutex>

namespace videocomposer {

enum class PixelFormat {
    RGBA,
    BGRA,
    NV12,
    UYVY
};

struct FrameData {
    uint8_t* data = nullptr;
    size_t size = 0;
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::RGBA;
    int64_t timestamp = 0;
    bool ownsData = false;
    
    ~FrameData() { if (ownsData && data) delete[] data; }
};

class FrameCapture {
public:
    FrameCapture();
    ~FrameCapture();
    
    // Initialize with capture dimensions
    bool initialize(int width, int height, PixelFormat format = PixelFormat::RGBA);
    void cleanup();
    
    // Capture operations (double-buffered PBO)
    void startCapture();                           // Initiate async read (non-blocking)
    bool getCompletedFrame(FrameData& frame);      // Get previously captured frame
    
    // Capabilities
    bool hasPBOSupport() const;
    bool isInitialized() const { return initialized_; }
    
    // Configuration
    void setSourceFBO(GLuint fbo);  // 0 = default framebuffer
    void setReadBuffer(GLenum buffer);  // GL_FRONT, GL_BACK, etc.
    
private:
    GLuint pbo_[2];           // Double-buffered PBOs
    int currentPBO_ = 0;
    int pendingPBO_ = -1;     // PBO with pending read
    
    int width_ = 0;
    int height_ = 0;
    PixelFormat format_ = PixelFormat::RGBA;
    size_t frameSize_ = 0;
    
    GLuint sourceFBO_ = 0;
    GLenum readBuffer_ = GL_BACK;
    
    bool pboSupported_ = false;
    bool initialized_ = false;
    bool pendingRead_ = false;
    
    std::queue<FrameData> completedFrames_;
    std::mutex queueMutex_;
    
    void checkPBOSupport();
    GLenum getGLFormat() const;
    GLenum getGLType() const;
};

} // namespace videocomposer

#endif
```

### OutputSink Interface (Shared)

```cpp
// cpp/output/OutputSink.h
#ifndef VIDEOCOMPOSER_OUTPUTSINK_H
#define VIDEOCOMPOSER_OUTPUTSINK_H

#include "FrameCapture.h"
#include <string>

namespace videocomposer {

struct OutputConfig {
    int width = 1920;
    int height = 1080;
    double frameRate = 60.0;
    PixelFormat format = PixelFormat::RGBA;
    std::string name;
    // Additional codec/format settings
};

class OutputSink {
public:
    enum class Type {
        NDI,
        HARDWARE,
        STREAMING,
        FILE
    };
    
    virtual ~OutputSink() = default;
    
    // Lifecycle
    virtual bool open(const std::string& destination, const OutputConfig& config) = 0;
    virtual void close() = 0;
    virtual bool isReady() const = 0;
    
    // Frame output (must be thread-safe)
    virtual bool writeFrame(const FrameData& frame) = 0;
    
    // Identification
    virtual Type getType() const = 0;
    virtual std::string getId() const = 0;
    virtual std::string getDescription() const = 0;
};

} // namespace videocomposer

#endif
```

### OutputSinkManager (Shared)

```cpp
// cpp/output/OutputSinkManager.h
#ifndef VIDEOCOMPOSER_OUTPUTSINKMANAGER_H
#define VIDEOCOMPOSER_OUTPUTSINKMANAGER_H

#include "OutputSink.h"
#include <vector>
#include <memory>
#include <mutex>

namespace videocomposer {

class OutputSinkManager {
public:
    OutputSinkManager();
    ~OutputSinkManager();
    
    // Sink management
    bool addSink(std::unique_ptr<OutputSink> sink);
    bool removeSink(const std::string& id);
    OutputSink* getSink(const std::string& id);
    
    // Frame distribution (thread-safe)
    void writeFrameToAll(const FrameData& frame);
    
    // Status
    bool hasActiveSinks() const;
    size_t getActiveSinkCount() const;
    std::vector<std::string> getActiveSinkIds() const;
    
    // Cleanup
    void closeAll();
    
private:
    std::vector<std::unique_ptr<OutputSink>> sinks_;
    mutable std::mutex sinksMutex_;
};

} // namespace videocomposer

#endif
```

### HeadlessDisplay (Shared)

```cpp
// cpp/display/HeadlessDisplay.h
#ifndef VIDEOCOMPOSER_HEADLESSDISPLAY_H
#define VIDEOCOMPOSER_HEADLESSDISPLAY_H

#include "DisplayBackend.h"
#include <EGL/egl.h>
#include <gbm.h>

namespace videocomposer {

class HeadlessDisplay : public DisplayBackend {
public:
    HeadlessDisplay();
    virtual ~HeadlessDisplay();
    
    // DisplayBackend interface
    bool openWindow() override;
    void closeWindow() override;
    bool isWindowOpen() const override;
    void render(LayerManager* layerManager, OSDManager* osdManager = nullptr) override;
    void handleEvents() override {}  // No events in headless
    void resize(unsigned int width, unsigned int height) override;
    void getWindowSize(unsigned int* width, unsigned int* height) const override;
    void setPosition(int x, int y) override {}  // N/A
    void getWindowPos(int* x, int* y) const override { *x = 0; *y = 0; }
    void setFullscreen(int action) override {}  // N/A
    bool getFullscreen() const override { return true; }
    void setOnTop(int action) override {}  // N/A
    bool getOnTop() const override { return false; }
    bool supportsMultiDisplay() const override { return false; }
    void* getContext() override;
    void makeCurrent() override;
    void clearCurrent() override;
    OpenGLRenderer* getRenderer() override;
    
    // Headless-specific
    GLuint getOffscreenFBO() const { return offscreenFBO_; }
    GLuint getOffscreenTexture() const { return offscreenTexture_; }
    
private:
    bool initDRM();
    bool initEGL();
    bool createOffscreenSurface(int width, int height);
    
    int drmFd_ = -1;
    struct gbm_device* gbmDevice_ = nullptr;
    
    EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
    EGLContext eglContext_ = EGL_NO_CONTEXT;
    EGLSurface eglSurface_ = EGL_NO_SURFACE;  // Dummy/pbuffer surface
    EGLConfig eglConfig_ = nullptr;
    
    GLuint offscreenFBO_ = 0;
    GLuint offscreenTexture_ = 0;
    GLuint depthRBO_ = 0;
    
    int width_ = 1920;
    int height_ = 1080;
    bool initialized_ = false;
    
    std::unique_ptr<OpenGLRenderer> renderer_;
};

} // namespace videocomposer

#endif
```

### Implementation Order

When implementing both plans, follow this order for shared components:

1. **FrameCapture** - Required by both MultiOutputRenderer and NDI output
2. **OutputSink / OutputSinkManager** - Framework for virtual outputs
3. **HeadlessDisplay** - Required for server deployments
4. **NDIVideoOutput** - First virtual output implementation

### CMake Integration

```cmake
# Shared output components
set(OUTPUT_SOURCES
    src/cuems_videocomposer/cpp/output/FrameCapture.cpp
    src/cuems_videocomposer/cpp/output/OutputSinkManager.cpp
)

# HeadlessDisplay (requires GBM + EGL)
if(GBM_FOUND AND EGL_FOUND)
    list(APPEND DISPLAY_SOURCES
        src/cuems_videocomposer/cpp/display/HeadlessDisplay.cpp
    )
    add_definitions(-DHAVE_HEADLESS_DISPLAY)
endif()

# NDI output (optional)
if(NDI_FOUND)
    list(APPEND OUTPUT_SOURCES
        src/cuems_videocomposer/cpp/output/NDIVideoOutput.cpp
    )
    add_definitions(-DHAVE_NDI_OUTPUT)
endif()
```

---

*Document prepared for cuems-videocomposer project - Multi-Display + NDI Integration*