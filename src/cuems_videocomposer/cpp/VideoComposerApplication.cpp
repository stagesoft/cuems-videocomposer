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
#include "sync/MIDISyncSource.h"
#include "sync/FramerateConverterSyncSource.h"
#include "layer/LayerManager.h"
#include "layer/VideoLayer.h"
#include "video/FrameFormat.h"
#include "display/OpenGLDisplay.h"
#include "display/DisplayManager.h"
#include "remote/OSCRemoteControl.h"
#include "osd/OSDManager.h"
#include "utils/Logger.h"
#include "utils/SMPTEUtils.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <vector>

// C MIDI bridge functions
extern "C" {
    void midi_open(char *midiid);
    int midi_choose_driver(const char *driver);
    int midi_connected(void);
}

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

    // Initialize MIDI sync source if ALSA Sequencer is default
    // This ensures the MIDI port is opened and visible in aconnect
    initializeMIDI();

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

    // Create OpenGL display backend
    displayBackend_ = std::make_unique<OpenGLDisplay>();

    // Create window (single window mode for now)
    if (!displayManager_->createWindows(displayBackend_.get(), 0)) {
        std::cerr << "Failed to create display window" << std::endl;
        return false;
    }

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

bool VideoComposerApplication::initializeMIDI() {
    // Initialize MIDI sync source - prefer mtcreceiver (proven working, like cuems-audioplayer)
    // The C MIDI bridge will handle the actual port opening
    
    // Check if MIDI is explicitly disabled
    std::string midiPort = config_->getString("midi_port", "-1");
    if (midiPort == "none" || midiPort == "off") {
        return true; // MIDI disabled, but that's OK
    }
    
    // Use C MIDI bridge to open MIDI port
    // This ensures compatibility with C display backends
    // (Functions declared at top of file)
    
    // Try mtcreceiver first (proven working implementation from cuems-audioplayer)
    // midi_choose_driver() already has fallback logic to ALSA-Sequencer if mtcreceiver fails
    // Returns 1 on success, 0 on failure
    if (midi_choose_driver("mtcreceiver")) {
        // Driver selected successfully (mtcreceiver or ALSA-Sequencer fallback)
        // Open MIDI port (autodetect if port not specified)
        std::vector<char> portBuf(midiPort.begin(), midiPort.end());
        portBuf.push_back('\0');
        midi_open(portBuf.data());
        
        // Check if connection succeeded
        if (midi_connected()) {
            LOG_INFO << "MIDI sync source initialized (mtcreceiver preferred)";
            LOG_INFO << "MTC: Waiting for MIDI Time Code...";
        } else {
            LOG_WARNING << "MIDI sync source initialization failed (continuing without MIDI)";
        }
    } else {
        LOG_WARNING << "No MIDI drivers available (mtcreceiver and ALSA Sequencer both unavailable)";
    }
    
    return true; // Don't fail initialization if MIDI fails
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

    // Create input source
    auto inputSource = std::make_unique<VideoFileInput>();
    
    // Set no-index option if configured
    bool noIndex = config_->getBool("want_noindex", false);
    inputSource->setNoIndex(noIndex);
    
    if (!inputSource->open(movieFile)) {
        return false;
    }

    // Create sync source (optional - can be manual)
    // Always create MIDI sync source if MIDI is enabled (even with autodetect "-1")
    std::unique_ptr<SyncSource> syncSource;
    std::string midiPort = config_->getString("midi_port", "-1");
    if (midiPort != "none" && midiPort != "off") {
        auto midiSync = std::make_unique<MIDISyncSource>();
        
        // Configure MIDI sync source from config
        bool verbose = config_->getBool("want_verbose", false);
        bool midiClkAdj = config_->getBool("midi_clkadj", false);
        double delay = config_->getDouble("delay", -1.0);
        
        // Set configuration before connecting
        midiSync->setVerbose(verbose);
        midiSync->setClockAdjustment(midiClkAdj);
        midiSync->setDelay(delay);
        
        // Connect to MIDI port (use "-1" for autodetect, which will use the driver selected in initializeMIDI)
        midiSync->connect(midiPort.c_str());
        
        // Wrap sync source with framerate converter (converts from sync source fps to input source fps)
        // This is timecode-agnostic and works with any sync source and any input source
        syncSource = std::make_unique<FramerateConverterSyncSource>(std::move(midiSync), inputSource.get());
    }

    // Create layer
    auto layer = std::make_unique<VideoLayer>();
    layer->setInputSource(std::move(inputSource));
    if (syncSource) {
        layer->setSyncSource(std::move(syncSource));
    }

    // Set layer properties from config
    auto& props = layer->properties();
    if (layer->isReady()) {
        FrameInfo info = layer->getFrameInfo();
        props.width = info.width;
        props.height = info.height;
    }
    props.visible = true;
    props.opacity = 1.0f;
    props.zOrder = 0;

    // Add layer to manager
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
        updateLayers();
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
        
        // Update OSD with current frame information from first layer
        if (osdManager_ && layerManager_->getLayerCount() > 0) {
            auto layers = layerManager_->getLayers();
            if (!layers.empty() && layers[0]) {
                VideoLayer* layer = layers[0];
                if (layer->isReady()) {
                    int64_t currentFrame = layer->getCurrentFrame();
                    if (currentFrame >= 0) {
                        osdManager_->setFrameNumber(currentFrame);
                        
                        // Update SMPTE if enabled
                        if (osdManager_->isModeEnabled(OSDManager::SMPTE)) {
                            FrameInfo info = layer->getFrameInfo();
                            if (info.framerate > 0.0) {
                                // Use SMPTEUtils for proper timecode formatting
                                std::string smpte = SMPTEUtils::frameToSmpteString(currentFrame, info.framerate);
                                osdManager_->setSMPTETimecode(smpte);
                        }
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

} // namespace videocomposer

