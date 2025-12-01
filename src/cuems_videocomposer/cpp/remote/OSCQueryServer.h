#ifndef VIDEOCOMPOSER_OSCQUERYSERVER_H
#define VIDEOCOMPOSER_OSCQUERYSERVER_H

#ifdef HAVE_OSCQUERY

#include <ossia/network/generic/generic_device.hpp>
#include <ossia/network/oscquery/oscquery_server.hpp>
#include <memory>
#include <map>
#include <string>
#include <mutex>

namespace videocomposer {

// Forward declarations
class VideoComposerApplication;
class LayerManager;
class VideoLayer;

/**
 * OSCQueryServer - OSCQuery protocol server implementation using libossia
 * 
 * Provides auto-discovery, parameter introspection, and bidirectional
 * state queries for cuems-videocomposer.
 */
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
    void updateLayerScale(int layerId, float scaleX, float scaleY);
    void updateLayerRotation(int layerId, float rotation);
    void updateLayerVisible(int layerId, bool visible);
    void updateLayerZOrder(int layerId, int zOrder);
    void updateLayerBlendMode(int layerId, int blendMode);
    void updateLayerFile(int layerId, const std::string& filepath);
    void updateLayerMtcFollow(int layerId, bool enabled);
    void updateLayerLoop(int layerId, bool enabled);
    void updateLayerTimeScale(int layerId, double timescale);
    void updateLayerColorAdjustment(int layerId, float brightness, float contrast, 
                                     float saturation, float hue, float gamma);
    void updateLayerCrop(int layerId, int x, int y, int width, int height);
    void updateLayerPanorama(int layerId, bool enabled);
    void updateLayerPan(int layerId, int panOffset);
    
    // Master layer updates
    void updateMasterOpacity(float opacity);
    void updateMasterPosition(float x, float y);
    void updateMasterScale(float scaleX, float scaleY);
    void updateMasterRotation(float rotation);
    void updateMasterColorAdjustment(float brightness, float contrast,
                                     float saturation, float hue, float gamma);
    
    // OSD updates
    void updateOSDFrame(bool enabled);
    void updateOSDSMPTE(bool enabled);
    void updateOSDText(const std::string& text);
    void updateOSDBox(bool enabled);
    void updateOSDPos(int x, int y);
    
    // Application updates
    void updateFPS(double fps);
    void updateOffset(int64_t offset);
    
    // Layer lifecycle
    void onLayerAdded(int layerId);
    void onLayerRemoved(int layerId);
    
private:
    void registerApplicationParameters();
    void registerLayerParameters(int layerId);
    void registerMasterParameters();
    void registerOSDParameters();
    void unregisterLayerParameters(int layerId);
    
    // Helper to find or create node
    ossia::net::node_base& findOrCreateNode(const std::string& path);
    
    VideoComposerApplication* app_;
    LayerManager* layerManager_;
    
    std::unique_ptr<ossia::net::generic_device> device_;
    std::map<std::string, ossia::net::parameter_base*> parameters_;
    std::map<int, std::vector<std::string>> layerParameterPaths_;  // Track paths per layer
    
    mutable std::mutex mutex_;  // Protect parameter map updates
    bool active_ = false;
    int oscPort_ = 7000;
    int wsPort_ = 7001;
};

} // namespace videocomposer

#endif // HAVE_OSCQUERY

#endif // VIDEOCOMPOSER_OSCQUERYSERVER_H

