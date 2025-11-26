/*
 * cuems-videocomposer - Video composer for CUEMS
 *
 * Copyright (C) 2024 stagelab.coop
 * Ion Reguera <ion@stagelab.coop>
 *
 * This program is partially based on xjadeo code:
 * Copyright (C) 2005-2014 Robin Gareus <robin@gareus.org>
 * Copyright (C) 2010-2012 Fons Adriaensen <fons@linuxaudio.org>
 * Copyright (C) 2009-2010 Tim Mayberry <mojofunk@gmail.com>
 * Copyright (C) 2005-2008 Jörn Nettingsmeier
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "VideoComposerApplication.h"
#include "config/ConfigurationManager.h"
#include "input/VideoFileInput.h"
#include "display/X11Display.h"
#ifdef HAVE_WAYLAND
#include "display/WaylandDisplay.h"
#endif
#include "input/HAPVideoInput.h"
#include "sync/MIDISyncSource.h"
#include "sync/FramerateConverterSyncSource.h"
#include "layer/LayerManager.h"
#include "layer/VideoLayer.h"
#include "video/FrameFormat.h"
#include "display/DisplayManager.h"
#include "remote/OSCRemoteControl.h"

#ifdef HAVE_VAAPI_INTEROP
#include "hwdec/VaapiInterop.h"
#endif
#include "osd/OSDManager.h"
#include "utils/Logger.h"
#include "utils/SMPTEUtils.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <cctype>

namespace videocomposer {

VideoComposerApplication::VideoComposerApplication()
    : running_(false)
    , initialized_(false)
{
}

VideoComposerApplication::~VideoComposerApplication() {
    shutdown();
}

bool VideoComposerApplication::initialize(int argc, char** argv) {
    if (initialized_) {
        return true;
    }

    // Initialize logger based on configuration
    // This will be set after config is loaded, but we can set defaults
    Logger::getInstance().setQuiet(false);

    // Initialize configuration
    if (!initializeConfiguration(argc, argv)) {
        // Check if help/version was requested - if so, don't log error
        bool helpRequested = false;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help" || arg == "-h" || arg == "--version" || arg == "-V") {
                helpRequested = true;
                break;
            }
        }
        if (!helpRequested) {
            LOG_ERROR << "Failed to initialize configuration";
        }
        return false;
    }
    
    // Update logger based on config
    bool quiet = config_->getBool("want_quiet", false);
    bool verbose = config_->getBool("want_verbose", false);
    Logger::getInstance().setQuiet(quiet);
    if (verbose) {
        Logger::getInstance().setLevel(Logger::VERBOSE);
    }

    // Initialize display
    if (!initializeDisplay()) {
        LOG_ERROR << "Failed to initialize display";
        return false;
    }

    // Initialize layer manager
    if (!initializeLayerManager()) {
        return false;
    }

    // Initialize OSD manager
    osdManager_ = std::make_unique<OSDManager>();

    // Initialize remote control
    if (!initializeRemoteControl()) {
        LOG_WARNING << "Failed to initialize remote control (continuing without it)";
        // Don't fail initialization if remote control fails
    }

    // Initialize global MIDI sync source (shared across all layers)
    initializeGlobalSyncSource();

    // Create initial layer if movie file provided
    std::string movieFile = config_->getMovieFile();
    if (!movieFile.empty()) {
        if (!createInitialLayer()) {
            LOG_ERROR << "Failed to create initial layer with file: " << movieFile;
            return false;
        }
        LOG_INFO << "Loaded video file: " << movieFile;
    }

    initialized_ = true;
    running_ = true;
    return true;
}

bool VideoComposerApplication::initializeConfiguration(int argc, char** argv) {
    config_ = std::make_unique<ConfigurationManager>();
    
    // Load default config file (if exists)
    std::string configFile = config_->getConfigFilePath();
    config_->loadFromFile(configFile); // Ignore errors if file doesn't exist
    
    // Parse command line (overrides config file)
    int parseResult = config_->parseCommandLine(argc, argv);
    if (parseResult != 0) {
        // Help or version was printed, exit gracefully
        return false;
    }
    
    return true;
}

bool VideoComposerApplication::initializeDisplay() {
    // Create display manager
    displayManager_ = std::make_unique<DisplayManager>();
    if (!displayManager_->detectDisplays()) {
        std::cerr << "Failed to detect displays" << std::endl;
        return false;
    }

    // Auto-detect and create display backend (prefer Wayland over X11)
    const char* waylandDisplay = getenv("WAYLAND_DISPLAY");
    const char* x11Display = getenv("DISPLAY");
    
    bool waylandAttempted = false;
    
#ifdef HAVE_WAYLAND
    if (waylandDisplay) {
        LOG_INFO << "WAYLAND_DISPLAY=" << waylandDisplay << " detected - attempting Wayland backend";
        displayBackend_ = std::make_unique<WaylandDisplay>();
        waylandAttempted = true;
    } else
#endif
    if (x11Display) {
        LOG_INFO << "Using X11 display backend (DISPLAY=" << x11Display << ")";
        displayBackend_ = std::make_unique<X11Display>();
    } else {
        LOG_ERROR << "No display server detected (neither WAYLAND_DISPLAY nor DISPLAY set)";
        return false;
    }

    // Create window (single window mode for now)
    if (!displayManager_->createWindows(displayBackend_.get(), 0)) {
#ifdef HAVE_WAYLAND
        // If Wayland failed but X11 is available, try falling back
        if (waylandAttempted && x11Display) {
            LOG_WARNING << "Wayland backend failed - falling back to X11";
            displayBackend_.reset();
            displayBackend_ = std::make_unique<X11Display>();
            
            if (!displayManager_->createWindows(displayBackend_.get(), 0)) {
                LOG_ERROR << "X11 fallback also failed";
                return false;
            }
            LOG_INFO << "Successfully fell back to X11 display backend";
        } else
#endif
        {
            LOG_ERROR << "Failed to create display window";
            return false;
        }
    }

#ifdef HAVE_VAAPI_INTEROP
    // Initialize VAAPI zero-copy interop (if available)
    // Need to make OpenGL context current for texture generation
    if (displayBackend_->hasVaapiSupport()) {
        displayBackend_->makeCurrent();  // Need GL context for texture generation
        vaapiInterop_ = std::make_unique<VaapiInterop>();
        if (vaapiInterop_->init(displayBackend_.get())) {
            LOG_INFO << "VaapiInterop initialized - VAAPI zero-copy enabled for video playback";
        } else {
            LOG_WARNING << "VaapiInterop initialization failed - falling back to CPU copy";
            vaapiInterop_.reset();
        }
        displayBackend_->clearCurrent();
    }
#endif

    return true;
}

bool VideoComposerApplication::initializeRemoteControl() {
    // Get OSC port from config (default: 7000)
    int oscPort = config_->getInt("osc_port", 7000);
    
    // Create OSC remote control
    remoteControl_ = std::make_unique<OSCRemoteControl>(this, layerManager_.get());
    
    if (!remoteControl_->initialize(oscPort)) {
        std::cerr << "Failed to initialize OSC remote control on port " << oscPort << std::endl;
        return false;
    }
    
    return true;
}

bool VideoComposerApplication::initializeLayerManager() {
    layerManager_ = std::make_unique<LayerManager>();
    return true;
}

bool VideoComposerApplication::createInitialLayer() {
    std::string movieFile = config_->getMovieFile();
    if (movieFile.empty()) {
        return false;
    }

    // Use common method to create input source
    auto inputSource = createInputSourceFromFile(movieFile);
    if (!inputSource) {
        return false;
    }

    // Create layer (use empty string as cue ID for initial layer - backward compatibility)
    auto layer = createEmptyLayer("");
    if (!layer) {
                return false;
            }
    
    // Setup layer with input source (sets input, sync source, and properties)
    setupLayerWithInputSource(layer.get(), std::move(inputSource));

    // Add layer to manager (use old addLayer method for backward compatibility)
    int layerId = layerManager_->addLayer(std::move(layer));
    if (layerId < 0) {
        return false;
    }

    // Note: Playback will automatically start when MTC is received
    // The VideoLayer::updateFromSyncSource() method will detect when
    // MTC starts rolling (rolling != 0) and automatically start playback.
    // This ensures the video starts playing forward as soon as MTC is received.

    return true;
}

int VideoComposerApplication::run() {
    if (!initialized_) {
        LOG_ERROR << "Application not initialized";
        return 1;
    }
    
    LOG_INFO << "Starting cuems-videocomposer application";

    // Main event loop
    // Use high-resolution clock for timing
    auto lastFrameTime = std::chrono::high_resolution_clock::now();
    const auto targetFrameTime = std::chrono::microseconds(16667); // ~60 FPS default (16.67ms)
    
    while (running_ && shouldContinue()) {
        auto currentTime = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(currentTime - lastFrameTime);
        
        processEvents();
        
        // Make OpenGL context current before updating layers
        // This is needed because hardware decoding may allocate GPU textures during frame loading
        if (displayBackend_ && displayBackend_->isWindowOpen()) {
            displayBackend_->makeCurrent();
        }
        
        updateLayers();
        
        // Clear OpenGL context after updating (render will make it current again)
        if (displayBackend_ && displayBackend_->isWindowOpen()) {
            displayBackend_->clearCurrent();
        }
        
        render();
        
        // Frame rate limiting - sleep if we're ahead of target frame time
        // This prevents busy-waiting while still allowing sync sources to drive timing
        auto sleepTime = targetFrameTime - elapsed;
        if (sleepTime.count() > 0) {
            std::this_thread::sleep_for(sleepTime);
        }
        
        lastFrameTime = std::chrono::high_resolution_clock::now();
    }

    return 0;
}

void VideoComposerApplication::processEvents() {
    // Process display window events
    if (displayBackend_) {
        displayBackend_->handleEvents();
    }
    
    // Process remote control events
    if (remoteControl_) {
        remoteControl_->process();
    }
}

void VideoComposerApplication::updateLayers() {
    if (!layerManager_) {
        return;
    }
    
        layerManager_->updateAll();
        
    // Update OSD with sync source timecode (like xjadeo: osd_smpte_ts = dispFrame - ts_offset)
    // Use global sync source frame directly to avoid backwards jumps from video wraparound
    if (osdManager_ && globalSyncSource_ && globalSyncSource_->isConnected()) {
        // Poll sync source to ensure current frame is up to date (layers poll their wrappers, but we poll directly)
        uint8_t rolling = 0;
        int64_t syncFrame = globalSyncSource_->pollFrame(&rolling);
        if (syncFrame >= 0) {
            // Get framerate from sync source (MTC framerate)
            double syncFps = globalSyncSource_->getFramerate();
            if (syncFps > 0.0) {
                // Display sync source timecode (MTC) - this is monotonic and won't jump backwards
                std::string smpte = SMPTEUtils::frameToSmpteString(syncFrame, syncFps);
                osdManager_->setSMPTETimecode(smpte);
                
                // Also set frame number from sync source
                osdManager_->setFrameNumber(syncFrame);
            } else {
                // Fallback: use first layer's frame if sync source framerate not available
                auto layers = layerManager_->getLayers();
                if (!layers.empty() && layers[0]) {
                    VideoLayer* layer = layers[0];
                    if (layer->isReady()) {
                        FrameInfo info = layer->getFrameInfo();
                        if (info.framerate > 0.0) {
                            std::string smpte = SMPTEUtils::frameToSmpteString(syncFrame, info.framerate);
                            osdManager_->setSMPTETimecode(smpte);
                            osdManager_->setFrameNumber(syncFrame);
                        } else {
                            osdManager_->setSMPTETimecode("00:00:00:00");
                        }
                    }
                }
            }
        } else {
            // No valid sync frame - fallback to first layer
            if (layerManager_->getLayerCount() > 0) {
            auto layers = layerManager_->getLayers();
            if (!layers.empty() && layers[0]) {
                VideoLayer* layer = layers[0];
                if (layer->isReady()) {
                    int64_t currentFrame = layer->getCurrentFrame();
                    if (currentFrame >= 0) {
                            FrameInfo info = layer->getFrameInfo();
                            if (info.framerate > 0.0) {
                                std::string smpte = SMPTEUtils::frameToSmpteString(currentFrame, info.framerate);
                                osdManager_->setSMPTETimecode(smpte);
                                osdManager_->setFrameNumber(currentFrame);
                            } else {
                                osdManager_->setSMPTETimecode("00:00:00:00");
                            }
                        }
                    }
                }
            }
        }
    } else if (osdManager_ && layerManager_->getLayerCount() > 0) {
        // Fallback: no global sync source, use first layer
        auto layers = layerManager_->getLayers();
        if (!layers.empty() && layers[0]) {
            VideoLayer* layer = layers[0];
            if (layer->isReady()) {
                int64_t currentFrame = layer->getCurrentFrame();
                if (currentFrame >= 0) {
                    FrameInfo info = layer->getFrameInfo();
                    if (info.framerate > 0.0) {
                        std::string smpte = SMPTEUtils::frameToSmpteString(currentFrame, info.framerate);
                        osdManager_->setSMPTETimecode(smpte);
                        osdManager_->setFrameNumber(currentFrame);
                    } else {
                        osdManager_->setSMPTETimecode("00:00:00:00");
                    }
                }
            }
        }
    }
}

void VideoComposerApplication::render() {
    if (displayBackend_ && layerManager_) {
        displayBackend_->render(layerManager_.get(), osdManager_.get());
    }
}

bool VideoComposerApplication::setFPS(double fps) {
    if (fps <= 0.0) {
        return false;
    }
    
    // Store FPS in configuration
    if (config_) {
        config_->setDouble("fps", fps);
    }
    
    // Apply FPS to all layers (if they support it)
    // For now, FPS is typically a property of the video file itself
    // This could be used for time-scaling in the future
    // TODO: Implement time-scaling per layer
    
    return true;
}

bool VideoComposerApplication::setTimeOffset(int64_t offset) {
    // Store offset in configuration
    if (config_) {
        config_->setInt("offset", static_cast<int>(offset));
    }
    
    // Apply offset to all layers
    if (layerManager_) {
        auto layers = layerManager_->getLayers();
        for (auto* layer : layers) {
            if (layer) {
                layer->setTimeOffset(offset);
            }
        }
    }
    
    return true;
}

void VideoComposerApplication::shutdown() {
    running_ = false;
    
    // Shutdown layers (they will clean up their input/sync sources)
    if (layerManager_) {
        layerManager_.reset();
    }
    
    if (remoteControl_) {
        remoteControl_->shutdown();
        remoteControl_.reset();
    }
    
    if (displayBackend_) {
        displayBackend_->closeWindow();
        displayBackend_.reset();
    }
    
    if (displayManager_) {
        displayManager_.reset();
    }
    
    config_.reset();
    initialized_ = false;
}

void VideoComposerApplication::configureMIDISyncSource(MIDISyncSource* midiSync) {
    if (!midiSync) {
        return;
    }
    
    // Configure MIDI sync source from config
    bool verbose = config_->getBool("want_verbose", false);
    bool midiClkAdj = config_->getBool("midi_clkadj", false);
    double delay = config_->getDouble("delay", -1.0);
    
    // Set configuration before connecting
    midiSync->setVerbose(verbose);
    midiSync->setClockAdjustment(midiClkAdj);
    midiSync->setDelay(delay);
}

bool VideoComposerApplication::initializeGlobalSyncSource() {
    // Always create and enable global MIDI sync source by default
    auto midiSync = std::make_unique<MIDISyncSource>();
    
    // Configure MIDI sync source
    configureMIDISyncSource(midiSync.get());
    
    // Get MIDI port (default: "-1" for autodetect, can be disabled with "none" or "off")
    std::string midiPort = config_->getString("midi_port", "-1");
    
    // Connect to MIDI port (use "-1" for autodetect)
    // Only skip if explicitly disabled
    if (midiPort != "none" && midiPort != "off") {
        midiSync->connect(midiPort.c_str());
        
        if (midiSync->isConnected()) {
            LOG_INFO << "Global MIDI sync source initialized and connected";
        } else {
            LOG_WARNING << "Global MIDI sync source created but not connected (continuing without MIDI)";
        }
    } else {
        LOG_INFO << "Global MIDI sync source created (MIDI explicitly disabled in config)";
    }
    
    // Store as global sync source (always created)
    globalSyncSource_ = std::move(midiSync);
    
    return true;
}

std::unique_ptr<InputSource> VideoComposerApplication::createInputSourceFromFile(const std::string& filepath) {
    // Create input source with codec-aware routing
    // Set no-index option before opening (to avoid reopening)
    bool noIndex = config_->getBool("want_noindex", false);
    std::string hwPrefStr = config_->getString("hardware_decoder", "auto");
    std::transform(hwPrefStr.begin(), hwPrefStr.end(), hwPrefStr.begin(), [](unsigned char c){ return std::tolower(c); });
    
    VideoFileInput::HardwareDecodePreference hwPref = VideoFileInput::HardwareDecodePreference::AUTO;
    if (hwPrefStr == "software" || hwPrefStr == "cpu") {
        hwPref = VideoFileInput::HardwareDecodePreference::SOFTWARE_ONLY;
    } else if (hwPrefStr == "vaapi") {
        hwPref = VideoFileInput::HardwareDecodePreference::VAAPI;
    } else if (hwPrefStr == "cuda" || hwPrefStr == "nvdec") {
        hwPref = VideoFileInput::HardwareDecodePreference::CUDA;
    }
    
    // First, detect codec by opening with VideoFileInput
    auto tempInput = std::make_unique<VideoFileInput>();
    tempInput->setNoIndex(noIndex);  // Set before opening to avoid reopening
    tempInput->setHardwareDecodePreference(hwPref);
    
#ifdef HAVE_VAAPI_INTEROP
    // Set VAAPI interop for zero-copy hardware decoding
    if (vaapiInterop_) {
        tempInput->setVaapiInterop(vaapiInterop_.get());
    }
#endif
    
    if (!tempInput->open(filepath)) {
        return nullptr;
    }

    // Detect codec and create appropriate input source
    std::unique_ptr<InputSource> inputSource;
    InputSource::CodecType codec = tempInput->detectCodec();
    
    // Check for any HAP variant (HAP, HAP_Q, or HAP_ALPHA)
    if (codec == InputSource::CodecType::HAP || 
        codec == InputSource::CodecType::HAP_Q || 
        codec == InputSource::CodecType::HAP_ALPHA) {
        // HAP codec: use HAPVideoInput for optimal GPU decoding
        tempInput->close();
        auto hapInput = std::make_unique<HAPVideoInput>();
        if (!hapInput->open(filepath)) {
            return nullptr;
        }
        inputSource = std::move(hapInput);
    } else {
        // Other codecs: use VideoFileInput (already opened with correct settings)
        inputSource = std::move(tempInput);
    }
    
    return inputSource;
}

std::unique_ptr<VideoLayer> VideoComposerApplication::createEmptyLayer(const std::string& cueId) {
    auto layer = std::make_unique<VideoLayer>();
    
    // Set basic properties
    auto& props = layer->properties();
    props.visible = true;
    props.opacity = 1.0f;
    props.zOrder = 0;
    
    // Note: Layer ID will be set when added to LayerManager
    // Input source and sync source are not set here
    
    return layer;
}

std::unique_ptr<SyncSource> VideoComposerApplication::createLayerSyncSource(InputSource* inputSource) {
    if (!globalSyncSource_) {
        return nullptr;
    }
    
    // Wrap global sync source with framerate converter (non-owning reference)
    // Each layer gets its own framerate converter, but they all share the same global sync source
    return std::make_unique<FramerateConverterSyncSource>(globalSyncSource_.get(), inputSource);
}

void VideoComposerApplication::setupLayerWithInputSource(VideoLayer* layer, std::unique_ptr<InputSource> inputSource) {
    if (!layer || !inputSource) {
        return;
    }
    
    // Set input source
    layer->setInputSource(std::move(inputSource));
    
    // Create and set sync source (wraps shared global sync source with framerate converter)
    // All layers share the same global MTC sync source, but each has its own framerate converter
    auto syncSource = createLayerSyncSource(layer->getInputSource());
    if (syncSource) {
        layer->setSyncSource(std::move(syncSource));
    }
    
    // Set layer properties from input source
    auto& props = layer->properties();
    if (layer->isReady()) {
        FrameInfo info = layer->getFrameInfo();
        props.width = info.width;
        props.height = info.height;
    }
}

bool VideoComposerApplication::createLayerWithFile(const std::string& cueId, const std::string& filepath) {
    // Create input source
    auto inputSource = createInputSourceFromFile(filepath);
    if (!inputSource) {
        LOG_ERROR << "Failed to create input source from file: " << filepath;
        return false;
    }
    
    // Create empty layer
    auto layer = createEmptyLayer(cueId);
    if (!layer) {
        LOG_ERROR << "Failed to create empty layer";
        return false;
    }
    
    // Setup layer with input source (sets input, sync source, and properties)
    setupLayerWithInputSource(layer.get(), std::move(inputSource));
    
    // Add layer to manager with cue ID
    if (!layerManager_->addLayerWithId(cueId, std::move(layer))) {
        LOG_ERROR << "Failed to add layer with cue ID: " << cueId;
        return false;
    }
    
    LOG_INFO << "Created layer with file: " << filepath << " (cue ID: " << cueId << ")";
    return true;
}

bool VideoComposerApplication::loadFileIntoLayer(const std::string& cueId, const std::string& filepath) {
    // Get or create layer
    VideoLayer* layer = layerManager_->getLayerByCueId(cueId);
    
    if (!layer) {
        // Layer doesn't exist - create it
        return createLayerWithFile(cueId, filepath);
    }
    
    // Layer exists - load file into it
    auto inputSource = createInputSourceFromFile(filepath);
    if (!inputSource) {
        LOG_ERROR << "Failed to create input source from file: " << filepath;
        return false;
    }
    
    // Setup layer with input source (sets input, sync source, and properties)
    setupLayerWithInputSource(layer, std::move(inputSource));
    
    LOG_INFO << "Loaded file into layer: " << filepath << " (cue ID: " << cueId << ")";
    return true;
}

bool VideoComposerApplication::unloadFileFromLayer(const std::string& cueId) {
    VideoLayer* layer = layerManager_->getLayerByCueId(cueId);
    if (!layer) {
        LOG_WARNING << "Layer not found for cue ID: " << cueId;
        return false;
    }
    
    // Clear input source (unloads file but keeps layer)
    layer->setInputSource(nullptr);
    
    // Clear sync source
    layer->setSyncSource(nullptr);
    
    LOG_INFO << "Unloaded file from layer (cue ID: " << cueId << ")";
    return true;
}

} // namespace videocomposer

