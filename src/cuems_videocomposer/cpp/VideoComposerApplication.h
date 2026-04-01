/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (C) 2020-2026 Stage Lab Coop.
 * Author: Ion Reguera <ion@stagelab.coop>
 *
 * This file is part of cuems-videocomposer.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef VIDEOCOMPOSER_APPLICATION_H
#define VIDEOCOMPOSER_APPLICATION_H

#include <map>
#include <memory>
#include <string>
#include <vector>

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
class AsyncVideoLoader;
class StartupSplash;

#ifdef HAVE_VAAPI_INTEROP
class VaapiInterop;
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
    DisplayBackend* getDisplayBackend() { return displayBackend_.get(); }
    
    // Get renderer access (for master layer controls)
    OpenGLRenderer& renderer();
    
    // Reset all state (called on project load - removes all layers, cancels loads, resets master)
    void resetAll();

    // File loading methods (called from RemoteCommandRouter)
    bool createLayerWithFile(const std::string& cueId, const std::string& filepath);
    bool createSharedLayer(const std::string& layerId, const std::string& driverLayerId, const std::string& filepath);
    bool loadFileIntoLayer(const std::string& cueId, const std::string& filepath);
    bool unloadFileFromLayer(const std::string& cueId);
    
    // Check if a video load is in progress for a cue ID
    bool isLoadPending(const std::string& cueId) const;

private:
    // Component initialization
    bool initializeConfiguration(int argc, char** argv);
    bool initializeDisplay();
    void showStartupSplash();
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
    void processAsyncLoads();
    
    // Async load callback (called when a video finishes loading)
    void onAsyncLoadComplete(const std::string& cueId, const std::string& filepath,
                            std::unique_ptr<InputSource> inputSource, bool success);

    // Component pointers
    std::unique_ptr<ConfigurationManager> config_;
    std::unique_ptr<RemoteControl> remoteControl_;
    std::unique_ptr<DisplayBackend> displayBackend_;
    std::unique_ptr<DisplayManager> displayManager_;
    std::unique_ptr<LayerManager> layerManager_;
    std::unique_ptr<OSDManager> osdManager_;
    
    // Global sync source (shared across all layers)
    std::unique_ptr<SyncSource> globalSyncSource_;
    
    // Async video loader
    std::unique_ptr<AsyncVideoLoader> asyncVideoLoader_;

    // Pending shared layers waiting for their driver's async load to complete.
    // Key: driver cueId, Value: list of {secondaryCueId, filepath}
    struct PendingSharedLayer {
        std::string layerId;
        std::string filepath;  // kept for logging/diagnostics
    };
    std::map<std::string, std::vector<PendingSharedLayer>> pendingSharedLayers_;

    // Application state
    bool running_;
    bool initialized_;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_APPLICATION_H

