#include "OSCRemoteControl.h"
#include "../VideoComposerApplication.h"
#include "../layer/LayerManager.h"
#include <cstring>
#include <cstdio>
#include <sstream>
#include <chrono>

extern "C" {
#include <lo/lo_lowlevel.h>
}

namespace videocomposer {

static void oscErrorHandler(int num, const char* msg, const char* path) {
    fprintf(stderr, "liblo server error %d in path %s: %s\n", num, path ? path : "unknown", msg);
}

OSCRemoteControl::OSCRemoteControl(VideoComposerApplication* app, LayerManager* layerManager)
    : oscServer_(nullptr)
    , router_(std::make_unique<RemoteCommandRouter>(app, layerManager))
    , userData_(nullptr)
    , port_(7000)
    , active_(false)
{
}

OSCRemoteControl::~OSCRemoteControl() {
    shutdown();
}

bool OSCRemoteControl::initialize(int port) {
    if (active_) {
        shutdown();
    }

    port_ = (port > 100 && port < 60000) ? port : 7000;

    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", port_);

    oscServer_ = lo_server_new(portStr, oscErrorHandler);
    if (!oscServer_) {
        return false;
    }

    // Register OSC message handler
    userData_ = new OSCUserData;
    userData_->instance = this;

    // Register catch-all handler
    lo_server_add_method(oscServer_, nullptr, nullptr, handleOSCMessage, userData_);

    // Register specific methods for compatibility with existing OSC interface
    lo_server_add_method(oscServer_, "/videocomposer/quit", "", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/load", "s", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/seek", "i", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/fps", "f", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/offset", "i", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/offset", "s", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/cmd", "s", handleOSCMessage, userData_);  // Remote command interface
    lo_server_add_method(oscServer_, "/videocomposer/midi/connect", "s", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/midi/disconnect", "", handleOSCMessage, userData_);
    // OSD commands
    lo_server_add_method(oscServer_, "/videocomposer/osd/timecode", "i", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/osd/smpte", "s", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/osd/frame", "i", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/osd/box", "i", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/osd/text", "s", handleOSCMessage, userData_);

    // Layer-level commands
    lo_server_add_method(oscServer_, "/videocomposer/layer/add", "s", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/remove", "i", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/duplicate", "i", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/reorder", "is", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/list", "", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/remove", "", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/seek", "i", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/play", "", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/pause", "", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/position", "ii", handleOSCMessage, userData_);  // Integer position
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/position", "ff", handleOSCMessage, userData_);  // Float position (smooth)
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/opacity", "f", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/visible", "i", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/zorder", "i", handleOSCMessage, userData_);
    
    // New file loading commands
    lo_server_add_method(oscServer_, "/videocomposer/layer/load", "ss", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/unload", "s", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/file", "s", handleOSCMessage, userData_);
    
    // Loop and auto-unload commands
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/autounload", "i", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/loop", "i", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/loop", "ii", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/loop/region", "ii", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/loop/region", "iii", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/loop/region/disable", "", handleOSCMessage, userData_);
    
    // Offset and MTC follow commands
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/offset", "i", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/offset", "s", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/mtcfollow", "i", handleOSCMessage, userData_);
    
    // Transform commands
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/scale", "ff", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/xscale", "f", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/yscale", "f", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/rotation", "f", handleOSCMessage, userData_);
    
    // Corner deformation commands
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/corners", "ffffffff", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/corner1", "ff", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/corner2", "ff", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/corner3", "ff", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/corner4", "ff", handleOSCMessage, userData_);
    
    // Blend mode command
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/blendmode", "i", handleOSCMessage, userData_);
    
    // Crop commands
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/crop", "iiii", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/crop/disable", "", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/panorama", "i", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/pan", "i", handleOSCMessage, userData_);
    
    // Layer color correction commands
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/brightness", "f", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/contrast", "f", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/saturation", "f", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/hue", "f", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/gamma", "f", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/layer/*/color/reset", "", handleOSCMessage, userData_);
    
    // Master layer commands (applied to composite before OSD)
    lo_server_add_method(oscServer_, "/videocomposer/master/opacity", "f", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/master/position", "ff", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/master/scale", "ff", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/master/xscale", "f", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/master/yscale", "f", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/master/rotation", "f", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/master/corners", "ffffffff", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/master/corner1", "ff", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/master/corner2", "ff", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/master/corner3", "ff", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/master/corner4", "ff", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/master/reset", "", handleOSCMessage, userData_);
    
    // Master color correction commands
    lo_server_add_method(oscServer_, "/videocomposer/master/brightness", "f", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/master/contrast", "f", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/master/saturation", "f", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/master/hue", "f", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/master/gamma", "f", handleOSCMessage, userData_);
    lo_server_add_method(oscServer_, "/videocomposer/master/color/reset", "", handleOSCMessage, userData_);

    active_ = true;
    return true;
}

int OSCRemoteControl::process() {
    if (!active_ || !oscServer_) {
        return 0;
    }

    // Use time-based budget to ensure video playback always has priority
    // Spend up to 2ms processing OSC messages per frame
    // This ensures video rendering (60fps = 16.67ms per frame) is never blocked
    // Video playback has absolute priority - OSC processing is secondary
    const auto MAX_TIME_BUDGET = std::chrono::microseconds(2000); // 2ms max per frame (12% of frame time)
    const int MAX_MESSAGES_PER_FRAME = 50; // Process enough messages for smooth animation (8+ per frame typical)
    
    auto startTime = std::chrono::high_resolution_clock::now();
    int count = 0;
    
    while (count < MAX_MESSAGES_PER_FRAME) {
        auto elapsed = std::chrono::high_resolution_clock::now() - startTime;
        if (elapsed >= MAX_TIME_BUDGET) {
            // Time budget exceeded - stop processing to preserve video playback priority
            break;
        }
        
        int result = lo_server_recv_noblock(oscServer_, 0);
        if (result <= 0) {
            // No more messages available
            break;
        }
        
        count++;
    }
    
    return count;
}

void OSCRemoteControl::shutdown() {
    if (oscServer_) {
        lo_server_free(oscServer_);
        oscServer_ = nullptr;
    }
    if (userData_) {
        delete userData_;
        userData_ = nullptr;
    }
    active_ = false;
}

bool OSCRemoteControl::isActive() const {
    return active_ && oscServer_ != nullptr;
}

int OSCRemoteControl::handleOSCMessage(const char* path, const char* types,
                                       lo_arg** argv, int argc, lo_message msg,
                                       void* userData) {
    OSCUserData* data = static_cast<OSCUserData*>(userData);
    if (!data || !data->instance) {
        return 1;
    }

    // Convert OSC arguments to string vector
    std::vector<std::string> args = data->instance->convertOSCArgs(types, argv, argc);

    // Route command
    std::string pathStr(path);
    bool handled = data->instance->router_->routeCommand(pathStr, args);

    return handled ? 0 : 1;
}

std::vector<std::string> OSCRemoteControl::convertOSCArgs(const char* types, lo_arg** argv, int argc) {
    std::vector<std::string> args;
    args.reserve(argc);

    for (int i = 0; i < argc && types[i] != '\0'; ++i) {
        std::ostringstream oss;
        switch (types[i]) {
            case 'i':
                oss << argv[i]->i;
                break;
            case 'f':
                oss << argv[i]->f;
                break;
            case 's':
                oss << &argv[i]->s;
                break;
            case 'd':
                oss << argv[i]->d;
                break;
            case 'h':
                oss << argv[i]->h;
                break;
            case 't':
                oss << argv[i]->t.sec << "." << argv[i]->t.frac;
                break;
            default:
                // Unknown type, skip
                continue;
        }
        args.push_back(oss.str());
    }

    return args;
}

} // namespace videocomposer

