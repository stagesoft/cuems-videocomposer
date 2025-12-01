#ifndef VIDEOCOMPOSER_APPLICATION_H
#define VIDEOCOMPOSER_APPLICATION_H

#include <memory>
#include <string>

namespace videocomposer {

// Forward declarations
class ConfigurationManager;
class InputSource;
class SyncSource;
class MIDISyncSource;
class RemoteControl;
class DisplayBackend;
class LayerManager;
class VideoLayer;
class DisplayManager;
class OSDManager;
class OpenGLRenderer;

#ifdef HAVE_VAAPI_INTEROP
class VaapiInterop;
#endif

#ifdef HAVE_OSCQUERY
class OSCQueryServer;
#endif

/**
 * VideoComposerApplication - Main application orchestrator
 * 
 * Manages all components, coordinates the event loop, layer updates, and rendering.
 * This is the central class that ties everything together.
 */
class VideoComposerApplication {
public:
    VideoComposerApplication();
    ~VideoComposerApplication();

    // Initialize application
    bool initialize(int argc, char** argv);

    // Run main event loop
    int run();

    // Shutdown application
    void shutdown();

    // Check if application should continue running
    bool shouldContinue() const { return running_; }

    // Quit application (called from remote control)
    void quit() { running_ = false; }

    // Configuration methods (called from remote control)
    bool setFPS(double fps);
    bool setTimeOffset(int64_t offset);
    
    // Get configuration
    ConfigurationManager* getConfig() { return config_.get(); }
    LayerManager* getLayerManager() { return layerManager_.get(); }
    OSDManager* getOSDManager() { return osdManager_.get(); }
    
    // Get renderer access (for master layer controls)
    OpenGLRenderer& renderer();
    
    // File loading methods (called from RemoteCommandRouter)
    bool createLayerWithFile(const std::string& cueId, const std::string& filepath);
    bool loadFileIntoLayer(const std::string& cueId, const std::string& filepath);
    bool unloadFileFromLayer(const std::string& cueId);
    
    // OSCQuery notifications (called from RemoteCommandRouter)
    void notifyLayerRemoved(int layerId);

private:
    // Component initialization
    bool initializeConfiguration(int argc, char** argv);
    bool initializeDisplay();
    bool initializeRemoteControl();
    bool initializeLayerManager();
    bool createInitialLayer();
    bool initializeGlobalSyncSource();
    
    // Common helper methods
    std::unique_ptr<InputSource> createInputSource(const std::string& source);
    std::unique_ptr<InputSource> createInputSourceFromFile(const std::string& filepath);
    std::unique_ptr<VideoLayer> createEmptyLayer(const std::string& cueId);
    std::unique_ptr<SyncSource> createLayerSyncSource(InputSource* inputSource);
    void configureMIDISyncSource(MIDISyncSource* midiSync);
    void setupLayerWithInputSource(VideoLayer* layer, std::unique_ptr<InputSource> inputSource);
    
    // Source detection helpers
    bool isNDISource(const std::string& source);
    bool isV4L2Source(const std::string& source);
    bool isNetworkStream(const std::string& source);

    // Event loop
    void processEvents();
    void updateLayers();
    void render();

    // Component pointers
    std::unique_ptr<ConfigurationManager> config_;
    std::unique_ptr<RemoteControl> remoteControl_;
#ifdef HAVE_OSCQUERY
    std::unique_ptr<OSCQueryServer> oscQueryServer_;
#endif
    std::unique_ptr<DisplayBackend> displayBackend_;
    std::unique_ptr<DisplayManager> displayManager_;
    std::unique_ptr<LayerManager> layerManager_;
    std::unique_ptr<OSDManager> osdManager_;
    
    // Global sync source (shared across all layers)
    std::unique_ptr<SyncSource> globalSyncSource_;

    // Application state
    bool running_;
    bool initialized_;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_APPLICATION_H

