# OSCQuery Integration Plan for cuems-videocomposer

## Overview

Add OSCQuery protocol support to cuems-videocomposer using libossia, enabling auto-discovery, parameter introspection, and state queries from cuems-engine and other OSCQuery-compatible clients.

## Status: PLANNED

## Background

### What is OSCQuery?

OSCQuery extends OSC by adding:
1. **HTTP/JSON Server** - Exposes the OSC address space as a queryable JSON tree
2. **Zeroconf/mDNS Discovery** - Automatic service advertisement on the network
3. **Parameter Metadata** - Types, ranges, descriptions, current values for each endpoint
4. **Bidirectional Queries** - Clients can GET current values, not just SET

### Current State

- cuems-videocomposer uses **liblo** for OSC (UDP-based, port 7000)
- cuems-engine and cuems-utils use **libossia** (via pyossia)
- cuems-engine currently connects to videocomposer via plain OSC (`ClientDevices.OSC`)
- libossia is already available at `/home/ion/src/cuems/libossia`

### Current Architecture

```
cuems-engine (Python/pyossia) ──OSC──> cuems-videocomposer (C++/liblo)
          │                                      │
          └── Uses ClientDevices.OSC ───────────┘
              (no discovery, no state query)
```

### Proposed Architecture

```
cuems-engine (Python/pyossia) ──OSCQuery──> cuems-videocomposer (C++/libossia)
          │                                         │
          └── Uses ClientDevices.OSCQUERY ─────────┘
              (auto-discovery, state queries, WebSocket)
```

## Benefits

| Benefit | Description |
|---------|-------------|
| **Auto-Discovery** | Control apps (cuems-engine, QLab, TouchOSC) can find cuems-videocomposer on the network automatically |
| **Parameter Introspection** | Clients can discover all ~80 OSC endpoints programmatically without documentation |
| **State Queries** | Clients can GET current values (opacity=0.5, visible=true, etc.) |
| **Metadata** | Expose parameter types, ranges, descriptions (e.g., "opacity: FLOAT [0.0-1.0]") |
| **Tool Compatibility** | Works with OSCQuery browsers in VDMX, Score (ossia), TouchOSC Pro, etc. |
| **Ecosystem Consistency** | Aligns with cuems-engine/cuems-utils which already use libossia |

## Implementation Plan

### Phase 1: Build System Integration
- [ ] Add libossia as a CMake subdirectory or find_package
- [ ] Configure libossia build options (disable unused features)
- [ ] Verify compilation with existing codebase
- [ ] Update debian/control for new dependencies

### Phase 2: OSCQueryServer Class
- [ ] Create `src/cuems_videocomposer/cpp/remote/OSCQueryServer.h`
- [ ] Create `src/cuems_videocomposer/cpp/remote/OSCQueryServer.cpp`
- [ ] Initialize libossia device with OSCQuery protocol
- [ ] Integrate with VideoComposerApplication startup/shutdown

### Phase 3: Parameter Registration
- [ ] Register application-level parameters (/videocomposer/quit, /fps, /offset)
- [ ] Register layer parameters (opacity, position, scale, rotation, etc.)
- [ ] Register master layer parameters
- [ ] Register OSD parameters
- [ ] Register color correction parameters
- [ ] Add parameter metadata (ranges, descriptions, access modes)

### Phase 4: Value Callbacks & State Sync
- [ ] Connect libossia callbacks to existing RemoteCommandRouter handlers
- [ ] Implement value push for state changes (update clients when values change)
- [ ] Handle dynamic layer creation/removal

### Phase 5: Testing & Integration
- [ ] Test with cuems-engine using ClientDevices.OSCQUERY
- [ ] Test with Score (ossia) for parameter browsing
- [ ] Test mDNS discovery on local network
- [ ] Performance testing (ensure no frame drops)

### Phase 6: cuems-engine Updates
- [ ] Update VideoClient to use OSCQUERY instead of OSC
- [ ] Remove manual endpoint definitions (auto-discovered)
- [ ] Add state query on connect

## Technical Details

### File Structure

```
src/cuems_videocomposer/cpp/remote/
├── OSCRemoteControl.cpp      # Existing (may be replaced or kept for compatibility)
├── OSCRemoteControl.h
├── OSCQueryServer.cpp        # NEW
├── OSCQueryServer.h          # NEW
├── RemoteCommandRouter.cpp   # Existing (reused)
├── RemoteCommandRouter.h
└── RemoteControl.h           # Existing interface
```

### OSCQueryServer Class Design

```cpp
// OSCQueryServer.h
#pragma once

#include <ossia/network/generic/generic_device.hpp>
#include <ossia/network/oscquery/oscquery_server.hpp>
#include <memory>
#include <map>
#include <string>

namespace videocomposer {

class VideoComposerApplication;
class LayerManager;

class OSCQueryServer {
public:
    OSCQueryServer(VideoComposerApplication* app, LayerManager* layerManager);
    ~OSCQueryServer();
    
    bool initialize(int oscPort = 7000, int wsPort = 7001);
    void shutdown();
    bool isActive() const;
    
    // Update parameter values (called when internal state changes)
    void updateLayerOpacity(int layerId, float opacity);
    void updateLayerPosition(int layerId, float x, float y);
    // ... etc
    
private:
    void registerApplicationParameters();
    void registerLayerParameters(int layerId);
    void registerMasterParameters();
    void registerOSDParameters();
    
    VideoComposerApplication* app_;
    LayerManager* layerManager_;
    
    std::unique_ptr<ossia::net::generic_device> device_;
    std::map<std::string, ossia::net::parameter_base*> parameters_;
    
    bool active_ = false;
};

} // namespace videocomposer
```

### Example Parameter Registration

```cpp
void OSCQueryServer::registerLayerParameters(int layerId) {
    using namespace ossia::net;
    
    std::string basePath = "/videocomposer/layer/" + std::to_string(layerId);
    
    // Opacity parameter
    {
        auto& node = find_or_create_node(*device_, basePath + "/opacity");
        auto param = node.create_parameter(ossia::val_type::FLOAT);
        
        // Metadata
        node.set(domain_attribute{}, make_domain(0.0f, 1.0f));
        node.set(description_attribute{}, "Layer opacity (0.0 = transparent, 1.0 = opaque)");
        node.set(access_mode_attribute{}, access_mode::BI);  // Bidirectional
        node.set(default_value_attribute{}, 1.0f);
        
        // Callback for incoming values
        param->add_callback([this, layerId](const ossia::value& v) {
            if (auto* layer = layerManager_->getLayer(layerId)) {
                layer->properties().opacity = v.get<float>();
            }
        });
        
        parameters_[basePath + "/opacity"] = param;
    }
    
    // Position parameter (vec2)
    {
        auto& node = find_or_create_node(*device_, basePath + "/position");
        auto param = node.create_parameter(ossia::val_type::VEC2F);
        
        node.set(description_attribute{}, "Layer position (x, y)");
        node.set(access_mode_attribute{}, access_mode::BI);
        
        param->add_callback([this, layerId](const ossia::value& v) {
            if (auto* layer = layerManager_->getLayer(layerId)) {
                auto vec = v.get<ossia::vec2f>();
                layer->properties().x = static_cast<int>(vec[0]);
                layer->properties().y = static_cast<int>(vec[1]);
            }
        });
        
        parameters_[basePath + "/position"] = param;
    }
    
    // ... more parameters
}
```

### CMake Integration

```cmake
# Option 1: Add as subdirectory (if libossia is in tree)
option(ENABLE_OSCQUERY "Enable OSCQuery protocol support" ON)

if(ENABLE_OSCQUERY)
    # Configure libossia build options
    set(OSSIA_STATIC ON CACHE BOOL "" FORCE)
    set(OSSIA_NO_QT ON CACHE BOOL "" FORCE)
    set(OSSIA_NO_INSTALL ON CACHE BOOL "" FORCE)
    set(OSSIA_PROTOCOL_OSCQUERY ON CACHE BOOL "" FORCE)
    set(OSSIA_PROTOCOL_OSC ON CACHE BOOL "" FORCE)
    # Disable unused protocols
    set(OSSIA_PROTOCOL_MIDI OFF CACHE BOOL "" FORCE)
    set(OSSIA_PROTOCOL_MINUIT OFF CACHE BOOL "" FORCE)
    set(OSSIA_PROTOCOL_HTTP OFF CACHE BOOL "" FORCE)
    
    add_subdirectory(${CMAKE_SOURCE_DIR}/../libossia ${CMAKE_BINARY_DIR}/libossia)
    
    add_definitions(-DHAVE_OSCQUERY)
    
    list(APPEND CPP_SOURCES
        src/cuems_videocomposer/cpp/remote/OSCQueryServer.cpp
    )
    
    target_link_libraries(cuems-videocomposer ossia)
endif()
```

### Ports Configuration

| Port | Protocol | Purpose |
|------|----------|---------|
| 7000 | OSC (UDP) | OSC message reception (same as current) |
| 7001 | WebSocket | OSCQuery HTTP/WebSocket server |

The OSCQuery server advertises both ports via mDNS:
- `_osc._udp` → port 7000
- `_oscjson._tcp` → port 7001

### Parameter List

Full list of parameters to expose via OSCQuery:

**Application Level:**
- `/videocomposer/quit` - Impulse
- `/videocomposer/fps` - Float [1.0-120.0]
- `/videocomposer/offset` - Int64 (frames)

**Per-Layer (`/videocomposer/layer/{id}/`):**
- `opacity` - Float [0.0-1.0]
- `visible` - Bool
- `position` - Vec2f
- `scale` - Vec2f
- `xscale` - Float
- `yscale` - Float
- `rotation` - Float [0-360]
- `zorder` - Int
- `blendmode` - Int [0-3] (normal, multiply, screen, overlay)
- `corners` - Vec8f (corner deform)
- `corner1`, `corner2`, `corner3`, `corner4` - Vec2f
- `file` - String (current file path)
- `offset` - String/Int64 (SMPTE or frames)
- `mtcfollow` - Bool
- `loop` - Bool
- `timescale` - Float
- `brightness` - Float [-1.0-1.0]
- `contrast` - Float [0.0-2.0]
- `saturation` - Float [0.0-2.0]
- `hue` - Float [-180-180]
- `gamma` - Float [0.1-3.0]
- `crop` - Vec4f (x, y, width, height)
- `panorama` - Bool
- `pan` - Int

**Master Level (`/videocomposer/master/`):**
- Same transform parameters as per-layer
- Same color correction parameters

**OSD (`/videocomposer/osd/`):**
- `frame` - Bool/Int
- `smpte` - Bool/Int
- `text` - String
- `box` - Bool
- `font` - String
- `pos` - Vec2f

## Dependencies

libossia brings bundled dependencies:
- WebSocket++ (WebSocket server)
- Avahi client (mDNS on Linux) - system package: `libavahi-client-dev`
- RapidJSON (JSON serialization)

### Debian Packages Required

```
libavahi-client-dev
```

## Compatibility

### Option A: Replace liblo with libossia (Recommended)
- libossia includes full OSC support
- Simpler codebase, single protocol stack
- cuems-engine can use `ClientDevices.OSCQUERY`

### Option B: Run both in parallel
- Keep liblo for backwards compatibility
- Add libossia OSCQuery on different ports
- More complex but preserves legacy client support

**Recommendation: Option A** - Clean replacement since cuems ecosystem already standardized on libossia.

## Effort Estimate

| Task | Time |
|------|------|
| Add libossia to CMake build | 2-4 hours |
| Create OSCQueryServer class | 1-2 days |
| Register all ~80 parameters | 1-2 days |
| Add value callbacks | 1 day |
| Testing with cuems-engine | 1 day |
| **Total** | **~1 week** |

## cuems-engine Changes

After videocomposer has OSCQuery support, update cuems-engine:

```python
# Current (VideoPlayer.py)
class VideoClient(PlayerClient):
    def __init__(self, player_port: int, name: str = "videoplayer"):
        super().__init__(
            local_port = player_port,
            name = name,
            endpoints = OSC_VIDEOPLAYER_CONF  # Manual endpoint list
        )

# With OSCQuery
class VideoClient(OssiaClient):
    def __init__(self, host: str, ws_port: int = 7001):
        super().__init__(
            host = host,
            remote_port = ws_port,
            remote_type = ClientDevices.OSCQUERY,  # Auto-discovery!
            # endpoints parameter not needed - discovered from server
        )
```

## Testing Checklist

- [ ] OSCQuery server starts on specified ports
- [ ] mDNS advertisement works (visible in `avahi-browse -a`)
- [ ] cuems-engine can discover and connect
- [ ] All parameters visible in Score (ossia) browser
- [ ] Parameter values update bidirectionally
- [ ] No frame drops during OSCQuery operations
- [ ] Dynamic layer creation updates OSCQuery tree
- [ ] Graceful shutdown and cleanup

## References

- libossia documentation: https://ossia.io/
- OSCQuery specification: https://github.com/Vidvox/OSCQueryProposal
- cuems-engine OSCQuery usage: `/home/ion/src/cuems/cuems-engine/src/cuemsengine/osc/`
- libossia C++ examples: `/home/ion/src/cuems/libossia/examples/Network/`

