/*
 * SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
 *
 * This file is part of cuems-videocomposer.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "VideoComposerApplication.h"
#include "config/ConfigurationManager.h"
#include "input/VideoFileInput.h"
#include "input/AsyncVideoLoader.h"
#include "display/X11Display.h"
#ifdef HAVE_WAYLAND
#include "display/WaylandDisplay.h"
#endif
#ifdef HAVE_DRM_BACKEND
#include "display/drm/DRMBackend.h"
#include "display/HeadlessDisplay.h"
#endif
#include "input/HAPVideoInput.h"
#include "input/FFmpegLiveInput.h"
#ifdef HAVE_NDI_SDK
#include "input/NDIVideoInput.h"
#endif
#include "sync/MIDISyncSource.h"
#include "sync/FramerateConverterSyncSource.h"
#include "sync/MtcReceiverMIDIDriver.h"  // MtcReceiver::resetWrapOffset() in resetAll()
#include "layer/LayerManager.h"
#include "layer/VideoLayer.h"
#include "video/FrameFormat.h"
#include "display/DisplayManager.h"
#include "display/OpenGLRenderer.h"
#include "display/StartupSplash.h"
#include "remote/OSCRemoteControl.h"

#ifdef HAVE_VAAPI_INTEROP
#endif
#include "osd/OSDManager.h"
#include "utils/Logger.h"
#include "utils/SMPTEUtils.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <thread>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cmath>

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

    showStartupSplash();

    // Initialize layer manager
    if (!initializeLayerManager()) {
        return false;
    }

    // Initialize async video loader (for non-blocking video file loading)
    asyncVideoLoader_ = std::make_unique<AsyncVideoLoader>();
    asyncVideoLoader_->initialize(config_.get(), displayBackend_.get());

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
    
    // Auto-detect and create display backend (prefer Wayland over X11, then DRM)
    const char* waylandDisplay = getenv("WAYLAND_DISPLAY");
    const char* x11Display = getenv("DISPLAY");
    
    bool waylandAttempted = false;
    bool needsDisplayManager = true;
    
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
    }
#ifdef HAVE_DRM_BACKEND
    else {
        // No display server - try DRM/KMS direct rendering
        LOG_INFO << "No display server detected - attempting DRM/KMS direct rendering";
        auto drmBackend = std::make_unique<DRMBackend>();
        
        // Apply resolution mode from command line
        std::string resMode = config_->getString("resolution_mode", "1080p");
        drmBackend->setResolutionExplicit(config_->isResolutionExplicit());
        if (!drmBackend->setResolutionMode(resMode)) {
            LOG_ERROR << "Invalid resolution mode: " << resMode;
            LOG_ERROR << "Valid modes: native, maximum, 1080p, 720p, 4k";
            return false;
        }
        
        if (drmBackend->openWindow()) {
            LOG_INFO << "DRM/KMS backend initialized with " << drmBackend->getOutputCount() << " output(s)";
            displayBackend_ = std::move(drmBackend);
            needsDisplayManager = false;  // DRM manages its own outputs
        } else {
            // DRM failed - try headless as last resort
            LOG_WARNING << "DRM backend failed - attempting headless mode";
            auto headless = std::make_unique<HeadlessDisplay>();
            headless->setDimensions(1920, 1080);  // Default resolution
            
            if (headless->openWindow()) {
                LOG_INFO << "Headless display backend initialized (1920x1080)";
                displayBackend_ = std::move(headless);
                needsDisplayManager = false;
            } else {
                LOG_ERROR << "All display backends failed (X11, Wayland, DRM, Headless)";
                return false;
            }
        }
    }
#else
    else {
        LOG_ERROR << "No display server detected (neither WAYLAND_DISPLAY nor DISPLAY set)";
        LOG_ERROR << "DRM backend not available - rebuild with libgbm-dev installed";
        return false;
    }
#endif

    // Only use DisplayManager for X11/Wayland backends
    if (needsDisplayManager) {
        if (!displayManager_->detectDisplays()) {
            std::cerr << "Failed to detect displays" << std::endl;
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
    }

#ifdef HAVE_VAAPI_INTEROP
    // VAAPI zero-copy interop is now per-instance (created by each VideoFileInput)
    // Each layer gets its own interop instance when it opens a file
    if (displayBackend_->hasVaapiSupport()) {
        LOG_INFO << "VAAPI support available - layers will use per-instance zero-copy interop";
    }
#endif

    return true;
}

void VideoComposerApplication::showStartupSplash() {
    if (config_->getBool("no_splash", false)) {
        return;
    }
    if (!displayBackend_ || !displayBackend_->isWindowOpen()) {
        return;
    }
    splash_ = std::make_unique<StartupSplash>();
    if (!splash_->loadFromEmbedded()) {
        splash_.reset();
        return;
    }
    splash_->show(displayBackend_.get(), displayManager_.get(),
                  StartupSplash::SPLASH_DURATION_SECONDS);
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
    auto inputSource = createInputSource(movieFile);
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

    // Multi-layer compositor main loop:
    // - Run at DISPLAY refresh rate, driven by vsync/page-flip
    // - Each layer updates independently based on MTC + its video framerate
    // - This ensures smooth compositing of multiple videos at different framerates
    //
    // Unlike xjadeo (single video, runs at video fps), we're a compositor that
    // can have layers at 24fps, 25fps, 29.97fps, 30fps, etc. all playing together.
    // The display rate is the common output rate that accommodates all.
    //
    // Multi-monitor handling:
    // - With multiple monitors at different refresh rates (e.g., 60Hz + 50Hz),
    //   we wait for ALL page flips to complete before the next frame
    // - Effective rate is limited by the slowest monitor in sequential mode
    // - TODO: Atomic modesetting would allow independent flip timing per monitor
    //
    // Frame updates are optimized:
    // - Each layer only decodes when its frame changes (based on MTC timing)
    // - Same-frame requests skip decoding (cached in frame_ buffer)
    // - Rendering happens every vsync for tear-free smooth output
    
    LOG_INFO << "Entering video update loop @ display refresh rate (vsync-driven)";

    while (running_ && shouldContinue()) {
        processEvents();

        // Bail out if a backend has surfaced a fatal error (e.g. DRM cold-boot
        // modeset verifier failed even after the in-process retry). Without
        // this, the loop would call schedulePageFlip every frame against a
        // broken modeset forever; instead we exit non-zero so systemd
        // Restart=on-failure brings up a fresh process.
        if (displayBackend_ && displayBackend_->hasFatalError()) {
            LOG_ERROR << "Display backend reports fatal error — exiting for systemd restart";
            return 1;
        }

        // Make OpenGL context current before updating layers
        // Required for VAAPI: EGL image creation needs current EGL context
        if (displayBackend_ && displayBackend_->isWindowOpen()) {
            displayBackend_->makeCurrent();
        }

        updateLayers();

        // Render - vsync/page-flip wait provides timing (60Hz)
        render();
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
    
    // Process completed async video loads
    processAsyncLoads();
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
            // Undo the display-latency advance: pollFrame() returns the
            // GPU-pipeline-compensated frame, but the OSD must show wire MTC
            // so operators can compare against an external SMPTE reference.
            long latencyCompMs = globalSyncSource_->getDisplayLatencyMs();
            int64_t osdFrame = syncFrame;
            if (latencyCompMs > 0 && syncFps > 0.0) {
                int64_t latencyFrames = static_cast<int64_t>(
                    std::round(static_cast<double>(latencyCompMs) * syncFps / 1000.0));
                osdFrame = std::max(int64_t(0), syncFrame - latencyFrames);
            }
            if (syncFps > 0.0) {
                // Display sync source timecode (MTC) - this is monotonic and won't jump backwards
                std::string smpte = SMPTEUtils::frameToSmpteString(osdFrame, syncFps);
                osdManager_->setSMPTETimecode(smpte);

                // Also set frame number from sync source
                osdManager_->setFrameNumber(osdFrame);
            } else {
                // Fallback: use first layer's frame if sync source framerate not available
                auto layers = layerManager_->getLayers();
                if (!layers.empty() && layers[0]) {
                    VideoLayer* layer = layers[0];
                    if (layer->isReady()) {
                        FrameInfo info = layer->getFrameInfo();
                        if (info.framerate > 0.0) {
                            std::string smpte = SMPTEUtils::frameToSmpteString(osdFrame, info.framerate);
                            osdManager_->setSMPTETimecode(smpte);
                            osdManager_->setFrameNumber(osdFrame);
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
        
        // Note: Each layer now has its own VaapiInterop instance
        // Frame release is handled per-layer in LayerDisplay/OpenGLRenderer
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

OpenGLRenderer& VideoComposerApplication::renderer() {
    // Get renderer from display backend
    OpenGLRenderer* renderer = displayBackend_ ? displayBackend_->getRenderer() : nullptr;
    if (!renderer) {
        // This should never happen if display is properly initialized
        static OpenGLRenderer dummyRenderer;
        LOG_ERROR << "renderer() called but display backend or renderer is null!";
        return dummyRenderer;
    }
    return *renderer;
}

void VideoComposerApplication::shutdown() {
    running_ = false;
    
    // Shutdown async video loader first (before layer manager)
    if (asyncVideoLoader_) {
        asyncVideoLoader_->shutdown();
        asyncVideoLoader_.reset();
    }
    
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

void VideoComposerApplication::configureMIDISyncSource(MIDISyncSource* midiSync,
                                                       DisplayBackend* displayBackend) {
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

    // Display-pipeline latency compensation. Three-way decision:
    //   1. CLI passed --output-latency-ms <N>  (operator override)  → use N, skip measurement.
    //   2. CLI did not pass; backend is DRM    (auto)                 → measure on real surfaces.
    //   3. CLI did not pass; backend is X11/headless                  → leave constructor default 33 ms.
    int outputLatencyMs = config_->getInt("output_latency_ms", -1);
    if (outputLatencyMs >= 0) {
        midiSync->setDisplayLatencyMs(static_cast<long>(outputLatencyMs));
        LOG_INFO << "DisplayLatency: applied = " << outputLatencyMs
                 << "ms (operator override via --output-latency-ms)";
        LOG_INFO << "MIDISyncSource: display latency compensation = "
                 << outputLatencyMs << " ms";
        return;
    }

    DRMBackend* drmBackend = dynamic_cast<DRMBackend*>(displayBackend);
    if (!drmBackend) {
        LOG_INFO << "DisplayLatency: backend is not DRM, skipping measurement (X11 / headless)";
        LOG_INFO << "DisplayLatency: applied = 33ms (constructor default)";
        return;
    }

    // 30 warmup + 120 sample = 2.0 s of visible aura pulse on a 60 Hz display,
    // plus the orchestrator's own pre-fill (≤6 frames) and 1 s fade-out tail.
    int measured = drmBackend->measureDisplayLatencyMs(30, 120, splash_.get());
    midiSync->setDisplayLatencyMs(static_cast<long>(measured));
    LOG_INFO << "MIDISyncSource: display latency compensation = " << measured << " ms";
}

bool VideoComposerApplication::initializeGlobalSyncSource() {
    // Always create and enable global MIDI sync source by default
    auto midiSync = std::make_unique<MIDISyncSource>();
    
    // Configure MIDI sync source (also runs the auto display-latency measurement
    // when --output-latency-ms is unset and the backend is DRM)
    configureMIDISyncSource(midiSync.get(), displayBackend_.get());
    
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

std::unique_ptr<InputSource> VideoComposerApplication::createInputSource(const std::string& source) {
    // Detect source type and create appropriate input
    
    // NDI source (ndi://source_name or just source_name for NDI)
    if (isNDISource(source)) {
#ifdef HAVE_NDI_SDK
        auto ndiInput = std::make_unique<NDIVideoInput>();
        std::string ndiName = source;
        if (ndiName.find("ndi://") == 0) {
            ndiName = ndiName.substr(6);  // Remove prefix
        }
        if (ndiInput->open(ndiName)) {
            return ndiInput;
        }
        LOG_WARNING << "Failed to open NDI source: " << ndiName;
        return nullptr;
#else
        LOG_ERROR << "NDI SDK not available (compiled without HAVE_NDI_SDK)";
        return nullptr;
#endif
    }
    
    // V4L2 device (/dev/video*)
    if (isV4L2Source(source)) {
        auto v4l2Input = std::make_unique<FFmpegLiveInput>();
        v4l2Input->setFormat("v4l2");
        if (v4l2Input->open(source)) {
            return v4l2Input;
        }
        LOG_WARNING << "Failed to open V4L2 source: " << source;
        return nullptr;
    }
    
    // Network stream (rtsp://, http://, udp://)
    if (isNetworkStream(source)) {
        auto streamInput = std::make_unique<FFmpegLiveInput>();
        if (streamInput->open(source)) {
            return streamInput;
        }
        LOG_WARNING << "Failed to open network stream: " << source;
        return nullptr;
    }
    
    // File path - use existing createInputSourceFromFile logic
    return createInputSourceFromFile(source);
}

bool VideoComposerApplication::isNDISource(const std::string& source) {
    // Check if source matches NDI naming pattern or has ndi:// prefix
    if (source.find("ndi://") == 0) {
        return true;
    }
    
    // Check if source matches NDI naming pattern: "HOSTNAME (Source Name)"
    // Or query NDI SDK to see if it's a known source
#ifdef HAVE_NDI_SDK
    auto ndiSources = NDIVideoInput::discoverSources(1000);
    for (const auto& s : ndiSources) {
        if (s == source) {
            return true;
        }
    }
#endif
    
    return false;
}

bool VideoComposerApplication::isV4L2Source(const std::string& source) {
    return source.find("/dev/video") == 0;
}

bool VideoComposerApplication::isNetworkStream(const std::string& source) {
    return source.find("rtsp://") == 0 || 
           source.find("http://") == 0 || 
           source.find("https://") == 0 ||
           source.find("udp://") == 0 ||
           source.find("tcp://") == 0;
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
    // Set DisplayBackend for per-instance VaapiInterop creation
    if (displayBackend_) {
        tempInput->setDisplayBackend(displayBackend_.get());
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
    
    // Set basic properties — layers start hidden and without MTC follow.
    // The engine will explicitly set visible=1 and mtcfollow when running a cue.
    auto& props = layer->properties();
    props.visible = false;
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
    // If a layer with this cueId already exists, reuse it instead of creating
    // a duplicate (the old layer would be orphaned in layers_ but unreachable
    // by cueId, leaking resources and wasting update cycles).
    if (layerManager_->getLayerByCueId(cueId)) {
        LOG_INFO << "Layer already exists for cue ID: " << cueId << ", reloading file";
        return loadFileIntoLayer(cueId, filepath);
    }

    // Create empty layer first (fast, non-blocking)
    auto layer = createEmptyLayer(cueId);
    if (!layer) {
        LOG_ERROR << "Failed to create empty layer";
        return false;
    }

    // Add layer to manager with cue ID
    if (!layerManager_->addLayerWithId(cueId, std::move(layer))) {
        LOG_ERROR << "Failed to add layer with cue ID: " << cueId;
        return false;
    }
    
    // Queue async load of video file
    if (asyncVideoLoader_) {
        asyncVideoLoader_->requestLoad(cueId, filepath, 
            [this](const std::string& cid, const std::string& fp, 
                   std::unique_ptr<InputSource> input, bool success) {
                onAsyncLoadComplete(cid, fp, std::move(input), success);
            });
        LOG_INFO << "Queued async load for layer: " << filepath << " (cue ID: " << cueId << ")";
        return true;
    }
    
    // Fallback: synchronous loading if async loader not available
    auto inputSource = createInputSource(filepath);
    if (!inputSource) {
        LOG_ERROR << "Failed to create input source from file: " << filepath;
        return false;
    }
    
    VideoLayer* layerPtr = layerManager_->getLayerByCueId(cueId);
    if (layerPtr) {
        setupLayerWithInputSource(layerPtr, std::move(inputSource));
    }
    
    LOG_INFO << "Created layer with file: " << filepath << " (cue ID: " << cueId << ")";
    return true;
}

bool VideoComposerApplication::createSharedLayer(const std::string& layerId, const std::string& driverLayerId, const std::string& filepath) {
    // Look up the driver layer
    VideoLayer* driverLayer = layerManager_->getLayerByCueId(driverLayerId);

    // Driver not ready (async load still in flight): create empty placeholder layer
    // and register as pending. Will be set up when driver's onAsyncLoadComplete fires.
    if (!driverLayer || !driverLayer->playback().hasInputSource()) {
        // Reuse existing layer if present (from previous arm cycle's unload — layer kept but InputSource cleared)
        if (!layerManager_->getLayerByCueId(layerId)) {
            auto emptyLayer = createEmptyLayer(layerId);
            if (emptyLayer) {
                layerManager_->addLayerWithId(layerId, std::move(emptyLayer));
            }
        }
        pendingSharedLayers_[driverLayerId].push_back({layerId, filepath});
        LOG_INFO << "Driver " << driverLayerId << " not ready, deferred shared setup for " << layerId;
        return true;
    }

    // Get shared InputSource and SyncSource from the driver
    auto sharedInput = driverLayer->playback().getSharedInputSource();
    auto sharedSync = driverLayer->playback().getSharedSyncSource();

    // Mark the driver as such (first load_shared call marks it).
    // Do NOT call setInputSource() — it resets playback state (pause, currentFrame=-1).
    // The driver already has the correct InputSource from the normal /layer/load path.
    if (!driverLayer->playback().isDecodeDriver()) {
        driverLayer->playback().setDecodeDriver(true);
        driverLayer->setUpdatePriority(0);
    }

    // Reuse existing layer or create new one
    VideoLayer* secondary = layerManager_->getLayerByCueId(layerId);
    if (!secondary) {
        auto newLayer = createEmptyLayer(layerId);
        if (!newLayer) {
            LOG_ERROR << "Failed to create empty shared layer";
            return false;
        }
        layerManager_->addLayerWithId(layerId, std::move(newLayer));
        secondary = layerManager_->getLayerByCueId(layerId);
    }

    // Set shared InputSource (isShared=true, isDriver=false)
    secondary->playback().setInputSource(sharedInput, true, false);

    // Share the same SyncSource (FramerateConverterSyncSource)
    if (sharedSync) {
        secondary->playback().setSyncSource(sharedSync);
    }

    // Secondary gets updatePriority 1 (driver = 0) to ensure driver updates first
    secondary->setUpdatePriority(1);

    // Set layer properties from shared input source
    if (secondary->isReady()) {
        FrameInfo info = secondary->getFrameInfo();
        secondary->properties().width = info.width;
        secondary->properties().height = info.height;
    }

    LOG_INFO << "Created shared layer " << layerId << " (driver: " << driverLayerId << ")";
    return true;
}

bool VideoComposerApplication::loadFileIntoLayer(const std::string& cueId, const std::string& filepath) {
    // Get or create layer
    VideoLayer* layer = layerManager_->getLayerByCueId(cueId);
    
    if (!layer) {
        // Layer doesn't exist - create it (handles async load internally)
        return createLayerWithFile(cueId, filepath);
    }
    
    // Layer exists - cancel any pending load and queue new async load
    if (asyncVideoLoader_) {
        // Cancel any previous pending load for this cue
        asyncVideoLoader_->cancelLoad(cueId);
        
        asyncVideoLoader_->requestLoad(cueId, filepath, 
            [this](const std::string& cid, const std::string& fp, 
                   std::unique_ptr<InputSource> input, bool success) {
                onAsyncLoadComplete(cid, fp, std::move(input), success);
            });
        LOG_INFO << "Queued async load into existing layer: " << filepath << " (cue ID: " << cueId << ")";
        return true;
    }
    
    // Fallback: synchronous loading if async loader not available
    auto inputSource = createInputSource(filepath);
    if (!inputSource) {
        LOG_ERROR << "Failed to create input source from file: " << filepath;
        return false;
    }
    
    setupLayerWithInputSource(layer, std::move(inputSource));
    
    LOG_INFO << "Loaded file into layer: " << filepath << " (cue ID: " << cueId << ")";
    return true;
}

void VideoComposerApplication::resetAll() {
    LOG_INFO << "Reset: removing all layers, cancelling loads, resetting master";

    // Cancel all pending async loads
    if (asyncVideoLoader_) {
        asyncVideoLoader_->cancelAll();
    }

    // Clear pending shared layer registrations
    pendingSharedLayers_.clear();

    // Remove all layers (unique_ptr destructors close files, stop decode queues, free textures)
    if (layerManager_) {
        layerManager_->removeAllLayers();
    }

    // Reset master properties (position, scale, rotation, opacity, color, warp)
    renderer().masterProperties().reset();

    // Clear the >24h wrap accumulator on project reset. MtcReceiver keeps it in a
    // process-global static; this long-running compositor would otherwise carry a
    // stale +86_400_000 ms offset into the next project after a >24h run (the
    // wire-driven reset can't catch a graceful reload's small backward delta).
    // NOTE: the driver's pollFrame() static lastReportedFrame is intentionally
    // NOT reset here — on the first full frame of the next project the large
    // frameDiff correctly classifies a seek-to-0, which is the desired behaviour.
    // (Plan 3a)
    MtcReceiver::resetWrapOffset();
}

bool VideoComposerApplication::unloadFileFromLayer(const std::string& cueId) {
    // Clear any pending secondaries waiting for this driver
    pendingSharedLayers_.erase(cueId);

    // Fully remove the layer (triggers promoteDecodeDriver if this was a shared driver)
    if (!layerManager_->removeLayerByCueId(cueId)) {
        LOG_WARNING << "Layer not found for cue ID: " << cueId;
        return false;
    }

    LOG_INFO << "Removed layer (cue ID: " << cueId << ")";
    return true;
}

bool VideoComposerApplication::isLoadPending(const std::string& cueId) const {
    if (asyncVideoLoader_) {
        return asyncVideoLoader_->isLoadPending(cueId);
    }
    return false;
}

void VideoComposerApplication::processAsyncLoads() {
    if (asyncVideoLoader_) {
        asyncVideoLoader_->pollCompleted();
    }
}

void VideoComposerApplication::onAsyncLoadComplete(const std::string& cueId, const std::string& filepath,
                                                   std::unique_ptr<InputSource> inputSource, bool success) {
    if (!success || !inputSource) {
        LOG_ERROR << "Async load failed for: " << filepath << " (cue ID: " << cueId << ")";
        pendingSharedLayers_.erase(cueId);  // cleanup any pending secondaries
        return;
    }

    // Get the layer for this cue ID
    VideoLayer* layer = layerManager_->getLayerByCueId(cueId);
    if (!layer) {
        LOG_WARNING << "Layer no longer exists for cue ID: " << cueId;
        pendingSharedLayers_.erase(cueId);
        return;
    }

    // If this layer is already a decode driver (deferred setup already ran from a
    // previous async load), don't replace its InputSource.
    if (layer->playback().isDecodeDriver()) {
        LOG_INFO << "Async load complete but layer " << cueId
                 << " is already decode driver — discarding stale InputSource";
        return;
    }

    // Setup layer with the loaded input source
    setupLayerWithInputSource(layer, std::move(inputSource));
    LOG_INFO << "Async load complete: " << filepath << " (cue ID: " << cueId << ")";

    // Resolve pending shared layers waiting for this driver
    auto it = pendingSharedLayers_.find(cueId);
    if (it != pendingSharedLayers_.end()) {
        auto sharedInput = layer->playback().getSharedInputSource();
        auto sharedSync = layer->playback().getSharedSyncSource();
        layer->playback().setDecodeDriver(true);
        layer->setUpdatePriority(0);

        for (auto& pending : it->second) {
            VideoLayer* secondary = layerManager_->getLayerByCueId(pending.layerId);
            if (!secondary) {
                LOG_WARNING << "Pending shared layer " << pending.layerId << " no longer exists — skipping";
                continue;
            }
            secondary->playback().setInputSource(sharedInput, true, false);
            if (sharedSync) secondary->playback().setSyncSource(sharedSync);
            secondary->setUpdatePriority(1);
            if (secondary->isReady()) {
                FrameInfo info = secondary->getFrameInfo();
                secondary->properties().width = info.width;
                secondary->properties().height = info.height;
            }
            LOG_INFO << "Deferred shared setup complete: " << pending.layerId << " (driver: " << cueId << ")";
        }
        pendingSharedLayers_.erase(it);
    }
}

} // namespace videocomposer

