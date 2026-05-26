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

/**
 * DRMBackend.cpp - DRM/KMS display backend implementation
 */

#include "DRMBackend.h"
#include "../OpenGLRenderer.h"
#include "../DisplayConfigurationManager.h"
#include "../StartupSplash.h"
#include "../../layer/LayerManager.h"
#include "../../layer/VideoLayer.h"
#include "../../osd/OSDManager.h"
#include "../../utils/Logger.h"
#include <cerrno>
#include <cmath>
#include <set>

#include <algorithm>
#include <chrono>
#include <cstdlib>  // for getenv

// #region DEBUG
#include <fstream>
#include <iomanip>
#include <sys/stat.h>
// #endregion DEBUG
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <xf86drmMode.h>  // for atomic modesetting

#ifdef HAVE_VAAPI_INTEROP
#include <va/va.h>
#include <va/va_drm.h>
#endif

namespace videocomposer {

DRMBackend::DRMBackend() {
    outputManager_ = std::make_unique<DRMOutputManager>();
    configManager_ = std::make_unique<DisplayConfigurationManager>();
}

DRMBackend::~DRMBackend() {
    closeWindow();
}

bool DRMBackend::openWindow() {
    if (initialized_) {
        LOG_WARNING << "DRMBackend: Already initialized";
        return true;
    }
    
    LOG_INFO << "DRMBackend: Opening DRM display";
    
    // Initialize DRM output manager
    if (!outputManager_->init(devicePath_)) {
        LOG_ERROR << "DRMBackend: Failed to initialize DRM output manager";
        return false;
    }
    
    // Apply resolution mode before creating surfaces
    // This modifies the output dimensions in outputManager_
    if (configManager_) {
        ResolutionPolicy policy = configManager_->getResolutionPolicy();
        ResolutionMode resMode;
        switch (policy) {
            case ResolutionPolicy::NATIVE:   resMode = ResolutionMode::NATIVE; break;
            case ResolutionPolicy::MAXIMUM:  resMode = ResolutionMode::MAXIMUM; break;
            case ResolutionPolicy::HD_1080P: resMode = ResolutionMode::HD_1080P; break;
            case ResolutionPolicy::HD_720P:  resMode = ResolutionMode::HD_720P; break;
            case ResolutionPolicy::UHD_4K:   resMode = ResolutionMode::UHD_4K; break;
            default:                         resMode = ResolutionMode::HD_1080P; break;
        }
        outputManager_->setResolutionMode(resMode);
        outputManager_->applyResolutionMode();
    }
    
    // Get connected outputs (with resolution mode applied)
    const auto& outputs = outputManager_->getOutputs();
    if (outputs.empty()) {
        LOG_ERROR << "DRMBackend: No connected outputs found";
        outputManager_->cleanup();
        return false;
    }
    
    LOG_INFO << "DRMBackend: Found " << outputs.size() << " connected output(s)";
    
    // Create surfaces for each output (keyed by name)
    // Share EGL display and GBM device for context sharing
    EGLContext sharedContext = EGL_NO_CONTEXT;
    EGLDisplay sharedDisplay = EGL_NO_DISPLAY;
    gbm_device* sharedGbmDevice = nullptr;
    
    // Record kernel enumeration order before storing into the sorted map
    outputOrder_.clear();
    
    for (const auto& outputInfo : outputs) {
        const std::string& outputName = outputInfo.name;
        LOG_INFO << "DRMBackend: Creating surface for " << outputName;
        
        auto surface = std::make_unique<DRMSurface>(outputManager_.get(), outputName);
        
        // Pass shared resources to subsequent surfaces
        if (!surface->init(sharedContext, sharedDisplay, sharedGbmDevice)) {
            LOG_ERROR << "DRMBackend: Failed to create surface for " << outputName;
            // Continue with other outputs
            continue;
        }
        
        // Use first surface's resources for sharing
        if (sharedContext == EGL_NO_CONTEXT) {
            sharedContext = surface->getContext();
            sharedDisplay = surface->getDisplay();
            sharedGbmDevice = surface->getGbmDevice();
            LOG_INFO << "DRMBackend: First surface created shared resources (EGL display, GBM device, context)";
        }
        
        surfaces_[outputName] = std::move(surface);
        outputOrder_.push_back(outputName);
    }
    
    if (surfaces_.empty()) {
        LOG_ERROR << "DRMBackend: No surfaces could be created";
        outputManager_->cleanup();
        return false;
    }
    
    // Initialize EGL extensions
    initEGLExtensions();
    
    // Initialize VAAPI
#ifdef HAVE_VAAPI_INTEROP
    initVAAPI();
#endif
    
    // Initialize rendering
    DRMSurface* primary = getPrimarySurface();
    if (primary) {
        primary->makeCurrent();
        
        // Check for environment variable to disable Virtual Canvas (for debugging)
        const char* disableVC = std::getenv("VIDEOCOMPOSER_NO_VIRTUAL_CANVAS");
        if (disableVC && (std::string(disableVC) == "1" || std::string(disableVC) == "true")) {
            LOG_INFO << "DRMBackend: Virtual Canvas disabled via VIDEOCOMPOSER_NO_VIRTUAL_CANVAS";
            useVirtualCanvas_ = false;
        }
        
        if (useVirtualCanvas_) {
            // Virtual Canvas mode: use MultiOutputRenderer
            if (!initVirtualCanvas()) {
                LOG_WARNING << "DRMBackend: Virtual Canvas init failed, falling back to legacy mode";
                useVirtualCanvas_ = false;
            }
        }
        
        if (!useVirtualCanvas_) {
            // Legacy mode: single OpenGLRenderer
            renderer_ = std::make_unique<OpenGLRenderer>();
            if (!renderer_->init()) {
                LOG_ERROR << "DRMBackend: Failed to initialize OpenGL renderer";
            }
        }
        
        primary->releaseCurrent();
    }
    
    initialized_ = true;
    LOG_INFO << "DRMBackend: Initialized with " << surfaces_.size() << " output(s)"
             << (useVirtualCanvas_ ? " (Virtual Canvas mode)" : " (Legacy mode)");
    
    return true;
}

void DRMBackend::closeWindow() {
    if (!initialized_) {
        return;
    }
    
    LOG_INFO << "DRMBackend: Closing";
    
#ifdef HAVE_VAAPI_INTEROP
    if (vaDisplay_) {
        vaTerminate(vaDisplay_);
        vaDisplay_ = nullptr;
    }
#endif
    
    // Cleanup renderers
    multiRenderer_.reset();
    renderer_.reset();
    outputRegions_.clear();
    
    // Cleanup surfaces
    for (auto& [name, surface] : surfaces_) {
        surface->cleanup();
    }
    surfaces_.clear();
    outputOrder_.clear();
    
    // Cleanup DRM
    outputManager_->cleanup();
    
    initialized_ = false;
}

bool DRMBackend::isWindowOpen() const {
    return initialized_ && !surfaces_.empty();
}

// #region DEBUG: sub-phase timer accumulators for render-budget diagnosis
// (ClickUp 869dd1c4d / fix/render-budget-instrumented).
// File-scope so renderVirtualCanvas, atomicPageFlip and renderLegacy can all
// contribute. Reset every 1 s in the [RENDER] flush block in render() below.
namespace {
    int64_t dbg_total_composite_us = 0;
    int64_t dbg_total_flipdrain_us = 0;
    int64_t dbg_total_commit_us = 0;
    int     dbg_commit_count = 0;
}
// #endregion DEBUG

void DRMBackend::render(LayerManager* layerManager, OSDManager* osdManager) {
    if (!initialized_ || surfaces_.empty()) {
        return;
    }

    // #region DEBUG
    static int dbg_render_count = 0;
    static auto dbg_render_last = std::chrono::steady_clock::now();
    static int64_t dbg_total_render_us = 0;
    auto dbg_render_start = std::chrono::steady_clock::now();
    // #endregion DEBUG

    if (useVirtualCanvas_ && multiRenderer_) {
        renderVirtualCanvas(layerManager, osdManager);
    } else {
        renderLegacy(layerManager, osdManager);
    }

    // #region DEBUG
    dbg_render_count++;
    auto dbg_render_end = std::chrono::steady_clock::now();
    dbg_total_render_us += std::chrono::duration_cast<std::chrono::microseconds>(dbg_render_end - dbg_render_start).count();
    auto dbg_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(dbg_render_end - dbg_render_last).count();
    if (dbg_elapsed_ms >= 1000) {
        try {
            mkdir("/tmp/.claude", 0755);
            auto now = std::chrono::system_clock::now();
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1000000;
            auto t = std::chrono::system_clock::to_time_t(now);
            std::tm tm_buf{};
            localtime_r(&t, &tm_buf);
            std::ofstream f("/tmp/.claude/debug.log", std::ios::app);
            f << "[" << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S")
              << "." << std::setw(6) << std::setfill('0') << us.count()
              << "] [RENDER] [DEBUG H3 H4 H6 H7] RATE rendered_frames=" << dbg_render_count
              << " elapsed_ms=" << dbg_elapsed_ms
              << " effective_fps=" << ((double)dbg_render_count * 1000.0 / dbg_elapsed_ms)
              << " avg_render_us=" << (dbg_render_count > 0 ? dbg_total_render_us / dbg_render_count : 0)
              << " avg_composite_us=" << (dbg_render_count > 0 ? dbg_total_composite_us / dbg_render_count : 0)
              << " avg_prev_flip_drain_us=" << (dbg_render_count > 0 ? dbg_total_flipdrain_us / dbg_render_count : 0)
              << " avg_atomic_commit_us=" << (dbg_commit_count > 0 ? dbg_total_commit_us / dbg_commit_count : 0)
              << " commit_count=" << dbg_commit_count
              << "\n";
        } catch (...) {}
        dbg_render_count = 0;
        dbg_total_render_us = 0;
        dbg_total_composite_us = 0;
        dbg_total_flipdrain_us = 0;
        dbg_total_commit_us = 0;
        dbg_commit_count = 0;
        dbg_render_last = dbg_render_end;
    }
    // #endregion DEBUG
}

void DRMBackend::renderVirtualCanvas(LayerManager* layerManager, OSDManager* osdManager) {
    if (!multiRenderer_) {
        return;
    }
    
    // Make primary surface context current for canvas rendering
    DRMSurface* primary = getPrimarySurface();
    if (!primary) {
        return;
    }
    
    // Process any completed flips (non-blocking)
    for (auto& [name, surface] : surfaces_) {
        surface->processFlipEvents();
    }

    primary->makeCurrent();

    // MultiOutputRenderer::render() handles:
    // 1. Rendering all layers to VirtualCanvas
    // 2. Blitting regions to each output surface (with blend/warp)
    // 3. Swapping buffers on each surface
    // #region DEBUG
    auto _dbg_composite_t0 = std::chrono::steady_clock::now();
    // #endregion DEBUG
    multiRenderer_->render(layerManager, osdManager);
    // #region DEBUG
    dbg_total_composite_us += std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - _dbg_composite_t0).count();
    // #endregion DEBUG

    primary->releaseCurrent();

    // Wait for all pending flips to complete first. Note: this measures the
    // *previous* frame's vsync drain — the wait at the top of this frame for
    // the prior frame's flip event. Not current-commit latency.
    // #region DEBUG
    auto _dbg_flipdrain_t0 = std::chrono::steady_clock::now();
    // #endregion DEBUG
    for (auto& [name, surface] : surfaces_) {
        if (surface->isFlipPending()) {
            surface->waitForFlip();
        }
    }
    // #region DEBUG
    dbg_total_flipdrain_us += std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - _dbg_flipdrain_t0).count();
    // #endregion DEBUG

    // Check if we can use atomic modesetting with planes
    bool useAtomic = outputManager_->supportsAtomic() && surfaces_.size() > 1;
    bool allHavePlanes = true;
    bool allModesSet = true;

    if (useAtomic) {
        for (auto& [name, surface] : surfaces_) {
            if (!surface->getPlane()) {
                allHavePlanes = false;
            }
            if (!surface->isModeSet()) {
                allModesSet = false;
            }
        }
        useAtomic = allHavePlanes && allModesSet;
    }

    if (useAtomic) {
        // ATOMIC PATH: Submit all page flips in single atomic commit
        // All outputs flip on same vsync = 60fps for any number of outputs
        atomicPageFlip();
    } else {
        // LEGACY PATH: Sequential page flips (single output or no atomic/planes)
        for (auto& [name, surface] : surfaces_) {
            surface->schedulePageFlip();
        }
    }
}

bool DRMBackend::atomicPageFlip() {
    drmModeAtomicReq* request = outputManager_->createAtomicRequest();
    if (!request) {
        LOG_WARNING << "DRMBackend: Failed to create atomic request";
        return false;
    }
    
    std::vector<DRMSurface*> preparedSurfaces;
    bool success = true;
    
    // Prepare each surface and add to atomic request
    for (auto& [name, surface] : surfaces_) {
        uint32_t fbId = surface->prepareAtomicFlip();
        if (fbId == 0) {
            LOG_WARNING << "DRMBackend: Failed to prepare surface " << name;
            success = false;
            break;
        }
        preparedSurfaces.push_back(surface.get());
        
        DRMPlane* plane = surface->getPlane();
        if (!plane || !plane->propertiesLoaded) {
            LOG_WARNING << "DRMBackend: No plane for surface " << name;
            success = false;
            break;
        }
        
        // Set plane properties for atomic commit
        // FB_ID - the framebuffer to display
        if (drmModeAtomicAddProperty(request, plane->planeId, plane->propFbId, fbId) < 0) {
            LOG_WARNING << "DRMBackend: Failed to set FB_ID for plane " << plane->planeId;
            success = false;
            break;
        }
        
        // CRTC_ID - which CRTC this plane is connected to
        if (drmModeAtomicAddProperty(request, plane->planeId, plane->propCrtcId, surface->getCrtcId()) < 0) {
            LOG_WARNING << "DRMBackend: Failed to set CRTC_ID for plane " << plane->planeId;
            success = false;
            break;
        }
        
        // Source rectangle (in 16.16 fixed point)
        uint32_t w = surface->getWidth();
        uint32_t h = surface->getHeight();
        drmModeAtomicAddProperty(request, plane->planeId, plane->propSrcX, 0);
        drmModeAtomicAddProperty(request, plane->planeId, plane->propSrcY, 0);
        drmModeAtomicAddProperty(request, plane->planeId, plane->propSrcW, w << 16);
        drmModeAtomicAddProperty(request, plane->planeId, plane->propSrcH, h << 16);
        
        // Destination rectangle
        drmModeAtomicAddProperty(request, plane->planeId, plane->propCrtcX, 0);
        drmModeAtomicAddProperty(request, plane->planeId, plane->propCrtcY, 0);
        drmModeAtomicAddProperty(request, plane->planeId, plane->propCrtcW, w);
        drmModeAtomicAddProperty(request, plane->planeId, plane->propCrtcH, h);
    }
    
    if (success) {
        // Commit atomically with NONBLOCK + PAGE_FLIP_EVENT:
        // - NONBLOCK: returns immediately instead of blocking until vsync
        // - PAGE_FLIP_EVENT: kernel sends per-CRTC events when flip completes
        //   (handled by pageFlipHandler2 via processFlipEvents/waitForFlip)
        // Do NOT use ALLOW_MODESET — it forces full modeset (link training,
        // DPMS) which takes 2+ vsyncs. Only needed for initial modeset.
        uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
        // #region DEBUG
        auto _dbg_commit_t0 = std::chrono::steady_clock::now();
        bool _dbg_commit_ok = outputManager_->commitAtomic(request, flags);
        dbg_total_commit_us += std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - _dbg_commit_t0).count();
        dbg_commit_count++;
        if (_dbg_commit_ok) {
        // #endregion DEBUG
            // Success — mark surfaces as flip-pending; buffer release is
            // deferred to pageFlipHandler2 when flip event fires
            for (auto* surface : preparedSurfaces) {
                surface->finalizeAtomicFlipAsync();
            }
        } else {
            LOG_WARNING << "DRMBackend: Atomic commit failed";
            success = false;
        }
    }
    
    if (!success) {
        // Cancel prepared surfaces and fall back to legacy
        for (auto* surface : preparedSurfaces) {
            surface->cancelAtomicFlip();
        }
        for (auto& [name, surface] : surfaces_) {
            surface->schedulePageFlip();
        }
    }
    
    drmModeAtomicFree(request);
    return success;
}
    
void DRMBackend::renderLegacy(LayerManager* layerManager, OSDManager* osdManager) {
    (void)osdManager;  // OSD rendering handled separately
    
    // MPV-STYLE: Process flip events first (non-blocking), render, THEN wait
    for (auto& [name, surface] : surfaces_) {
        surface->processFlipEvents();
    }
    
    // Render to each output
    for (auto& [name, surface] : surfaces_) {
        if (!surface->isInitialized()) {
            continue;
        }
        
        // Begin frame
        if (!surface->beginFrame()) {
            continue;
        }
        
        // Set viewport for renderer (critical for aspect ratio calculations)
        if (renderer_) {
            renderer_->setViewport(0, 0, surface->getWidth(), surface->getHeight());
        }
        
        // Clear
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        
        // Render layers (use getLayersSortedByZOrder like X11/Wayland backends do)
        if (renderer_ && layerManager) {
            auto layers = layerManager->getLayersSortedByZOrder();
            
            for (size_t i = 0; i < layers.size(); ++i) {
                VideoLayer* layer = layers[i];
                if (layer) {
                    bool visible = layer->properties().visible;
                    bool ready = layer->isReady();
                    
                    if (visible && ready) {
                        renderer_->renderLayer(layer);
                    }
                }
            }
        }
        
        // End frame
        surface->endFrame();
        
        // Wait for pending flip before scheduling a new one
        // (can only have one flip pending at a time)
        if (surface->isFlipPending()) {
            // #region DEBUG
            auto _dbg_flipdrain_t0 = std::chrono::steady_clock::now();
            // #endregion DEBUG
            surface->waitForFlip();
            // #region DEBUG
            dbg_total_flipdrain_us += std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - _dbg_flipdrain_t0).count();
            // #endregion DEBUG
        }

        // Schedule page flip (non-blocking)
        surface->schedulePageFlip();
    }
}

void DRMBackend::handleEvents() {
    // Poll for hotplug events
    if (outputManager_) {
        outputManager_->pollHotplug();
    }
}

void DRMBackend::resize(unsigned int width, unsigned int height) {
    // DRM doesn't resize windows - mode must be changed
    LOG_WARNING << "DRMBackend: resize() not supported - use setOutputMode()";
}

void DRMBackend::getWindowSize(unsigned int* width, unsigned int* height) const {
    DRMSurface* primary = const_cast<DRMBackend*>(this)->getPrimarySurface();
    if (primary) {
        *width = primary->getWidth();
        *height = primary->getHeight();
    } else {
        *width = 0;
        *height = 0;
    }
}

void DRMBackend::setPosition(int x, int y) {
    // DRM doesn't support window positioning
    (void)x;
    (void)y;
}

void DRMBackend::getWindowPos(int* x, int* y) const {
    *x = 0;
    *y = 0;
}

void DRMBackend::setFullscreen(int action) {
    // DRM is always fullscreen
    (void)action;
}

bool DRMBackend::getFullscreen() const {
    return true;  // Always fullscreen
}

void DRMBackend::setOnTop(int action) {
    // DRM is always on top (no compositor)
    (void)action;
}

bool DRMBackend::getOnTop() const {
    return true;
}

void* DRMBackend::getContext() {
    DRMSurface* primary = getPrimarySurface();
    return primary ? primary->getContext() : nullptr;
}

void DRMBackend::makeCurrent() {
    DRMSurface* primary = getPrimarySurface();
    if (primary) {
        primary->makeCurrent();
    }
}

void DRMBackend::clearCurrent() {
    DRMSurface* primary = getPrimarySurface();
    if (primary) {
        primary->releaseCurrent();
    }
}

OpenGLRenderer* DRMBackend::getRenderer() {
    return renderer_.get();
}

#ifdef HAVE_EGL
EGLDisplay DRMBackend::getEGLDisplay() const {
    DRMSurface* primary = const_cast<DRMBackend*>(this)->getPrimarySurface();
    return primary ? primary->getDisplay() : EGL_NO_DISPLAY;
}

PFNEGLCREATEIMAGEKHRPROC DRMBackend::getEglCreateImageKHR() const {
    return eglCreateImageKHR_;
}

PFNEGLDESTROYIMAGEKHRPROC DRMBackend::getEglDestroyImageKHR() const {
    return eglDestroyImageKHR_;
}

PFNGLEGLIMAGETARGETTEXTURE2DOESPROC DRMBackend::getGlEGLImageTargetTexture2DOES() const {
    return glEGLImageTargetTexture2DOES_;
}

PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC DRMBackend::getGlEGLImageTargetTexStorageEXT() const {
    return glEGLImageTargetTexStorageEXT_;
}
#endif

#ifdef HAVE_VAAPI_INTEROP
VADisplay DRMBackend::getVADisplay() const {
    return vaDisplay_;
}
#endif

std::vector<OutputInfo> DRMBackend::getOutputs() const {
    return outputManager_->getOutputs();
}

size_t DRMBackend::getOutputCount() const {
    return surfaces_.size();
}

bool DRMBackend::hasFatalError() const {
    for (const auto& [name, surface] : surfaces_) {
        if (surface && surface->hasFatalModesetError()) {
            return true;
        }
    }
    return false;
}

bool DRMBackend::configureOutputRegion(const std::string& outputName, 
                                        int canvasX, int canvasY,
                                        int canvasWidth, int canvasHeight) {
    // Find the output region by name
    OutputRegion* region = nullptr;
    for (auto& r : outputRegions_) {
        if (r.name == outputName) {
            region = &r;
            break;
        }
    }
    
    if (!region) {
        LOG_ERROR << "DRMBackend::configureOutputRegion: Unknown output " << outputName;
        return false;
    }
    
    // Get output native dimensions if not specified
    if (canvasWidth <= 0 || canvasHeight <= 0) {
        auto it = surfaces_.find(outputName);
        if (it != surfaces_.end()) {
            const auto& info = it->second->getOutputInfo();
            if (canvasWidth <= 0) canvasWidth = info.width;
            if (canvasHeight <= 0) canvasHeight = info.height;
        }
    }
    
    // Update the output region
    region->canvasX = canvasX;
    region->canvasY = canvasY;
    region->canvasWidth = canvasWidth;
    region->canvasHeight = canvasHeight;
    
    LOG_INFO << "DRMBackend: Configured " << outputName 
             << " region: " << canvasX << "," << canvasY
             << " " << canvasWidth << "x" << canvasHeight;

    // Update MultiOutputRenderer if initialized
    if (multiRenderer_) {
        // Make GL context current before any GL operations (FBO creation, etc.)
        if (!surfaces_.empty()) {
            surfaces_.begin()->second->makeCurrent();
        }
        
        std::vector<OutputSurface*> surfacePtrs;
        for (const auto& reg : outputRegions_) {
            surfacePtrs.push_back(surfaces_.at(reg.name).get());
        }
        multiRenderer_->configureOutputs(outputRegions_, surfacePtrs);
    }
    
    return true;
}

bool DRMBackend::configureOutputBlend(const std::string& outputName,
                                       float left, float right,
                                       float top, float bottom, float gamma) {
    // Find the output region by name
    OutputRegion* region = nullptr;
    for (auto& r : outputRegions_) {
        if (r.name == outputName) {
            region = &r;
            break;
        }
    }
    
    if (!region) {
        LOG_ERROR << "DRMBackend::configureOutputBlend: Unknown output " << outputName;
        return false;
    }
    
    // Update blend configuration
    region->blend.left = left;
    region->blend.right = right;
    region->blend.top = top;
    region->blend.bottom = bottom;
    region->blend.gamma = gamma;
    
    LOG_INFO << "DRMBackend: Configured " << outputName 
             << " blend: L=" << left << " R=" << right
             << " T=" << top << " B=" << bottom << " gamma=" << gamma;
    
    // Update MultiOutputRenderer if initialized
    if (multiRenderer_) {
        // Make GL context current before any GL operations (FBO creation, etc.)
        if (!surfaces_.empty()) {
            surfaces_.begin()->second->makeCurrent();
        }
        
        std::vector<OutputSurface*> surfacePtrs;
        for (const auto& reg : outputRegions_) {
            surfacePtrs.push_back(surfaces_.at(reg.name).get());
        }
        multiRenderer_->configureOutputs(outputRegions_, surfacePtrs);
    }
    
    return true;
}

DRMSurface* DRMBackend::getSurface(const std::string& name) {
    auto it = surfaces_.find(name);
    if (it != surfaces_.end()) {
        return it->second.get();
    }
    return nullptr;
}

DRMSurface* DRMBackend::getPrimarySurface() {
    // Return first surface in map (consistent ordering)
    if (!surfaces_.empty()) {
        return surfaces_.begin()->second.get();
    }
    return nullptr;
}

bool DRMBackend::setOutputMode(const std::string& outputName, int width, int height, double refresh) {
    LOG_INFO << "DRMBackend::setOutputMode called: " << outputName 
             << " width=" << width << " height=" << height << " refresh=" << refresh;
    
    auto it = surfaces_.find(outputName);
    if (it == surfaces_.end()) {
        LOG_ERROR << "DRMBackend::setOutputMode: No surface for output " << outputName;
        return false;
    }
    
    DRMSurface* surface = it->second.get();
    if (!surface) {
        LOG_ERROR << "DRMBackend::setOutputMode: Null surface for " << outputName;
        return false;
    }
    
    // Check if resolution is actually changing
    int oldWidth = static_cast<int>(surface->getWidth());
    int oldHeight = static_cast<int>(surface->getHeight());
    
    LOG_INFO << "  Current surface size: " << oldWidth << "x" << oldHeight;
    
    if (width == oldWidth && height == oldHeight) {
        LOG_INFO << "DRMBackend::setOutputMode: Already at " << width << "x" << height;
        return true;
    }
    
    LOG_INFO << "DRMBackend: Changing " << outputName << " from "
             << oldWidth << "x" << oldHeight << " to " << width << "x" << height;
    
    // Get connector index for DRMOutputManager
    const DRMConnector* conn = outputManager_->getConnectorByName(outputName);
    if (!conn) {
        LOG_ERROR << "DRMBackend::setOutputMode: Unknown connector " << outputName;
        return false;
    }
    int connectorIndex = conn->info.index;
    
    // Step 1: Update the mode in DRMOutputManager (stores mode for later use)
    if (!outputManager_->prepareMode(connectorIndex, width, height, refresh)) {
        LOG_ERROR << "DRMBackend::setOutputMode: Mode not available";
        return false;
    }
    
    // Step 2: Resize the GBM/EGL surface (creates new buffers, resets modeSet_=false)
    if (!surface->resize(width, height)) {
        LOG_ERROR << "DRMBackend::setOutputMode: Failed to resize surface";
        return false;
    }
    
    // Step 3: Update output region by name
    for (auto& region : outputRegions_) {
        if (region.name == outputName) {
            region.physicalWidth = width;
            region.physicalHeight = height;
            if (region.canvasWidth == oldWidth) region.canvasWidth = width;
            if (region.canvasHeight == oldHeight) region.canvasHeight = height;
            break;
        }
    }
    
    // Step 4: Recalculate canvas size and reconfigure renderer
    if (multiRenderer_) {
        // Make GL context current before any GL operations
        if (!surfaces_.empty()) {
            surfaces_.begin()->second->makeCurrent();
        }
        
        // Recalculate total canvas size
        int canvasWidth = 0, canvasHeight = 0;
        for (const auto& region : outputRegions_) {
            int right = region.canvasX + region.canvasWidth;
            int bottom = region.canvasY + region.canvasHeight;
            canvasWidth = std::max(canvasWidth, right);
            canvasHeight = std::max(canvasHeight, bottom);
        }
        
        // Resize virtual canvas if needed
        VirtualCanvas* canvas = multiRenderer_->getCanvas();
        if (canvas && (canvasWidth != canvas->getWidth() || 
                       canvasHeight != canvas->getHeight())) {
            LOG_INFO << "DRMBackend: Resizing virtual canvas to " 
                     << canvasWidth << "x" << canvasHeight;
            canvas->configure(canvasWidth, canvasHeight);
        }
        
        // Reconfigure renderer with new regions (matching order)
        std::vector<OutputSurface*> surfacePtrs;
        for (const auto& reg : outputRegions_) {
            surfacePtrs.push_back(surfaces_.at(reg.name).get());
        }
        multiRenderer_->configureOutputs(outputRegions_, surfacePtrs);
    }
    
    LOG_INFO << "DRMBackend: " << outputName << " successfully changed to "
             << width << "x" << height;
    
    return true;
}

void DRMBackend::setCaptureEnabled(bool enabled, int width, int height) {
    if (!multiRenderer_) {
        LOG_WARNING << "DRMBackend::setCaptureEnabled: No MultiOutputRenderer";
        return;
    }
    
    if (width > 0 && height > 0) {
        multiRenderer_->setCaptureResolution(width, height);
    }
    
    multiRenderer_->setCaptureEnabled(enabled);
    
    LOG_INFO << "DRMBackend: Capture " << (enabled ? "enabled" : "disabled");
    if (enabled && width > 0 && height > 0) {
        LOG_INFO << "  Resolution: " << width << "x" << height;
    }
}

bool DRMBackend::isCaptureEnabled() const {
    if (!multiRenderer_) {
        return false;
    }
    return multiRenderer_->isCaptureEnabled();
}

void DRMBackend::setOutputSinkManager(OutputSinkManager* sinkManager) {
    if (multiRenderer_) {
        multiRenderer_->setOutputSinkManager(sinkManager);
        LOG_INFO << "DRMBackend: Output sink manager " 
                 << (sinkManager ? "connected" : "disconnected");
    }
}

bool DRMBackend::setResolutionMode(const std::string& mode) {
    if (!configManager_) {
        return false;
    }
    
    // Use config manager to parse and store the mode
    if (!configManager_->setResolutionPolicyFromString(mode)) {
        return false;
    }
    
    // If not initialized yet, just store the mode - it will be applied in openWindow()
    if (!initialized_ || !outputManager_ || surfaces_.empty()) {
        LOG_INFO << "Resolution mode set to: " << mode << " (will be applied when display is initialized)";
        return true;
    }
    
    LOG_INFO << "Resolution mode: " << mode;
    
    // Map to DRMOutputManager's ResolutionMode
    ResolutionPolicy policy = configManager_->getResolutionPolicy();
    ResolutionMode resMode;
    
    switch (policy) {
        case ResolutionPolicy::NATIVE:
            resMode = ResolutionMode::NATIVE;
            break;
        case ResolutionPolicy::MAXIMUM:
            resMode = ResolutionMode::MAXIMUM;
            break;
        case ResolutionPolicy::HD_1080P:
            resMode = ResolutionMode::HD_1080P;
            break;
        case ResolutionPolicy::HD_720P:
            resMode = ResolutionMode::HD_720P;
            break;
        case ResolutionPolicy::UHD_4K:
            resMode = ResolutionMode::UHD_4K;
            break;
        default:
            resMode = ResolutionMode::HD_1080P;
            break;
    }
    
    // Set the mode in output manager (updates internal state)
    outputManager_->setResolutionMode(resMode);
    
    // Apply mode to determine target resolutions for each output
    outputManager_->applyResolutionMode();
    
    // Now actually change each surface to its target resolution
    bool success = true;
    auto outputs = outputManager_->getOutputs();
    
    for (auto& [surfName, surface] : surfaces_) {
        if (!surface) continue;
        
        // Find the matching output info by name
        const OutputInfo* info = nullptr;
        for (const auto& out : outputs) {
            if (out.name == surfName) {
                info = &out;
                break;
            }
        }
        
        if (!info || !info->connected || !info->enabled) {
            continue;
        }
        
        int currentW = static_cast<int>(surface->getWidth());
        int currentH = static_cast<int>(surface->getHeight());
        
        // If the target resolution differs, apply the change
        if (info->width != currentW || info->height != currentH) {
            LOG_INFO << "Applying resolution mode to " << surfName << ": "
                     << currentW << "x" << currentH << " -> " 
                     << info->width << "x" << info->height;
            
            if (!setOutputMode(surfName, info->width, info->height, info->refreshRate)) {
                LOG_ERROR << "Failed to apply resolution mode to " << surfName;
                success = false;
            }
        }
    }
    
    return success;
}

bool DRMBackend::saveConfiguration(const std::string& path) {
    if (!configManager_) {
        return false;
    }
    
    std::string configPath = path.empty() ? 
        DisplayConfigurationManager::getDefaultConfigPath() : path;
    
    return configManager_->saveToFile(configPath);
}

bool DRMBackend::loadConfiguration(const std::string& path) {
    if (!configManager_) {
        return false;
    }
    
    std::string configPath = path.empty() ? 
        DisplayConfigurationManager::getDefaultConfigPath() : path;
    
    if (!configManager_->loadFromFile(configPath)) {
        return false;
    }
    
    // Apply loaded configuration
    return setResolutionMode(configManager_->getResolutionPolicyString());
}

void DRMBackend::initEGLExtensions() {
#ifdef HAVE_EGL
    DRMSurface* primary = getPrimarySurface();
    if (!primary) {
        return;
    }
    
    EGLDisplay display = primary->getDisplay();
    if (display == EGL_NO_DISPLAY) {
        return;
    }
    
    // Get extension function pointers
    eglCreateImageKHR_ = (PFNEGLCREATEIMAGEKHRPROC)
        eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR_ = (PFNEGLDESTROYIMAGEKHRPROC)
        eglGetProcAddress("eglDestroyImageKHR");
    glEGLImageTargetTexture2DOES_ = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    // Desktop GL extension for EGL image binding (mpv approach for DRM/KMS)
    glEGLImageTargetTexStorageEXT_ = (PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC)
        eglGetProcAddress("glEGLImageTargetTexStorageEXT");
    
    if (eglCreateImageKHR_ && eglDestroyImageKHR_) {
        if (glEGLImageTargetTexStorageEXT_) {
            LOG_INFO << "DRMBackend: EGL image extensions available (TexStorageEXT for Desktop GL)";
        } else if (glEGLImageTargetTexture2DOES_) {
            LOG_INFO << "DRMBackend: EGL image extensions available (Texture2DOES fallback)";
        } else {
            LOG_WARNING << "DRMBackend: No EGL image target function available";
        }
    } else {
        LOG_WARNING << "DRMBackend: EGL image extensions not available";
    }
#endif
}

void DRMBackend::initVAAPI() {
#ifdef HAVE_VAAPI_INTEROP
    if (!outputManager_) {
        return;
    }
    
    int fd = outputManager_->getFd();
    if (fd < 0) {
        return;
    }
    
    vaDisplay_ = vaGetDisplayDRM(fd);
    if (!vaDisplay_) {
        LOG_WARNING << "DRMBackend: Failed to get VAAPI display";
        return;
    }
    
    int major, minor;
    VAStatus status = vaInitialize(vaDisplay_, &major, &minor);
    if (status != VA_STATUS_SUCCESS) {
        LOG_WARNING << "DRMBackend: Failed to initialize VAAPI: " << vaErrorStr(status);
        vaDisplay_ = nullptr;
        return;
    }
    
    LOG_INFO << "DRMBackend: VAAPI initialized (version " << major << "." << minor << ")";
#endif
}

bool DRMBackend::initVirtualCanvas() {
    if (surfaces_.empty()) {
        LOG_ERROR << "DRMBackend: No surfaces for Virtual Canvas";
        return false;
    }
    
    DRMSurface* primary = getPrimarySurface();
    if (!primary) {
        LOG_ERROR << "DRMBackend: No primary surface for Virtual Canvas";
        return false;
    }
    
    // Create MultiOutputRenderer with EGL context
    multiRenderer_ = std::make_unique<MultiOutputRenderer>();
    
#ifdef HAVE_EGL
    if (!multiRenderer_->init(primary->getDisplay(), primary->getContext())) {
        LOG_ERROR << "DRMBackend: Failed to initialize MultiOutputRenderer";
        multiRenderer_.reset();
        return false;
    }
#else
    LOG_ERROR << "DRMBackend: Virtual Canvas requires EGL";
    multiRenderer_.reset();
    return false;
#endif
    
    // Build default output regions based on kernel enumeration order.
    buildOutputRegions();
    
    // Try to load a startup display config. If present, it defines the
    // canvas layout (connector → region + per-output resolution) used
    // from the very first frame — no engine re-send needed. If absent,
    // buildOutputRegions() above already populated DRM-detected defaults
    // (side-by-side, kernel enumeration order).
    //
    // Producer today: nobody writes this file in the current shipping
    // stack (the previous cuems-generate-display-conf was retired
    // 2026-04-23 after the project_mappings canvas_region was
    // repurposed from a layout directive into a UI-template hint, so
    // reading it as pixels became incorrect). Operators can still hand-
    // author the file for custom arrangements.
    //
    // FUTURE (PHASE A — urgent): this try-load block should become a
    // try-load-or-generate-and-save block so videocomposer itself owns
    // display.conf generation on first boot and preserves operator
    // edits on subsequent boots. See memory project_videocomposer_
    // display_conf_phases for the plan.
    static const std::string startupConfPath = "/run/cuems/display.conf";
    if (configManager_ && configManager_->loadFromFile(startupConfPath)) {
        if (configManager_->getCanvasLayout() == CanvasLayout::CUSTOM) {
            std::vector<OutputInfo> outputInfos;
            for (const auto& name : getSortedOutputNames()) {
                outputInfos.push_back(surfaces_.at(name)->getOutputInfo());
            }
            auto loadedRegions = configManager_->generateOutputRegions(outputInfos);
            if (!loadedRegions.empty()) {
                outputRegions_ = loadedRegions;
                LOG_INFO << "DRMBackend: Applied startup display config from " << startupConfPath;
            }
        }
    }
    
    // Apply per-output resolutions from display.conf (if -r was not explicitly passed)
    if (resolutionExplicit_) {
        LOG_WARNING << "DRMBackend: Ignoring per-output resolutions from display.conf"
                    << " — overridden by explicit -r flag";
    } else if (configManager_) {
        for (const auto& name : getSortedOutputNames()) {
            const auto* outConf = configManager_->getOutputConfig(name);
            if (outConf && outConf->width > 0 && outConf->height > 0) {
                auto& surface = surfaces_.at(name);
                int curW = static_cast<int>(surface->getWidth());
                int curH = static_cast<int>(surface->getHeight());
                if (outConf->width != curW || outConf->height != curH) {
                    LOG_INFO << "DRMBackend: Per-output resolution for " << name
                             << ": " << curW << "x" << curH
                             << " -> " << outConf->width << "x" << outConf->height;
                    setOutputMode(name, outConf->width, outConf->height,
                                  outConf->refreshRate);
                }
            }
        }
    }

    // Configure MultiOutputRenderer with surfaces and regions (matching order)
    std::vector<OutputSurface*> surfacePtrs;
    for (const auto& region : outputRegions_) {
        surfacePtrs.push_back(surfaces_.at(region.name).get());
    }

    multiRenderer_->configureOutputs(outputRegions_, surfacePtrs);
    
    LOG_INFO << "DRMBackend: Virtual Canvas initialized with " 
             << outputRegions_.size() << " output(s)";
    
    return true;
}

std::vector<std::string> DRMBackend::getSortedOutputNames() const {
    // Return outputs in kernel enumeration order so that auto-detected canvas
    // regions match the order returned by getOutputs() (which the engine sees).
    // This makes output ordering consistent and predictable across machines;
    // the mappings file mapped_to fields can then be adjusted when connector
    // names differ between machines.
    return outputOrder_;
}

void DRMBackend::buildOutputRegions() {
    outputRegions_.clear();
    
    int canvasX = 0;
    
    for (const auto& outputName : getSortedOutputNames()) {
        const OutputInfo& info = surfaces_.at(outputName)->getOutputInfo();
        
        OutputRegion region = OutputRegion::createDefault(
            info.name,
            info.width,
            info.height,
            canvasX,
            0  // All outputs at Y=0 (horizontal arrangement)
        );
        
        outputRegions_.push_back(region);
        
        // Next output starts after this one
        canvasX += info.width;
        
        LOG_INFO << "DRMBackend: Output region (" << info.name << "): "
                 << region.canvasX << "," << region.canvasY << " "
                 << region.canvasWidth << "x" << region.canvasHeight;
    }
}


void DRMBackend::autoConfigureOutputs(const std::string& arrangement, int overlap) {
    outputRegions_.clear();
    
    int canvasX = 0;
    int canvasY = 0;
    
    for (const auto& outputName : getSortedOutputNames()) {
        const OutputInfo& info = surfaces_.at(outputName)->getOutputInfo();
        
        OutputRegion region;
        region.name = info.name;
        region.canvasWidth = info.width;
        region.canvasHeight = info.height;
        region.physicalWidth = info.width;
        region.physicalHeight = info.height;
        region.enabled = true;
        
        if (arrangement == "vertical") {
            region.canvasX = 0;
            region.canvasY = canvasY;
            
            // Blend with previous output
            if (!outputRegions_.empty() && overlap > 0) {
                region.canvasY -= overlap;
                region.blend.top = static_cast<float>(overlap);
                outputRegions_.back().blend.bottom = static_cast<float>(overlap);
            }
            
            canvasY += info.height;
        } else {
            // Default: horizontal
            region.canvasX = canvasX;
            region.canvasY = 0;
            
            // Blend with previous output
            if (!outputRegions_.empty() && overlap > 0) {
                region.canvasX -= overlap;
                region.blend.left = static_cast<float>(overlap);
                outputRegions_.back().blend.right = static_cast<float>(overlap);
            }
            
            canvasX += info.width;
        }
        
        outputRegions_.push_back(region);
    }
    
    // Reconfigure if already initialized
    if (multiRenderer_ && multiRenderer_->isInitialized()) {
        // Make GL context current before any GL operations (FBO creation, etc.)
        if (!surfaces_.empty()) {
            surfaces_.begin()->second->makeCurrent();
        }
        
        std::vector<OutputSurface*> surfacePtrs;
        for (const auto& region : outputRegions_) {
            surfacePtrs.push_back(surfaces_.at(region.name).get());
        }
        multiRenderer_->configureOutputs(outputRegions_, surfacePtrs);
    }
    
    LOG_INFO << "DRMBackend: Auto-configured " << outputRegions_.size() 
             << " outputs (" << arrangement << ", overlap=" << overlap << ")";
}

bool DRMBackend::configureOutputRegion(const std::string& outputName, const OutputRegion& region) {
    // Find the output
    for (auto& r : outputRegions_) {
        if (r.name == outputName) {
            r = region;
            r.name = outputName;  // Preserve name
            
            // Reconfigure if initialized
            if (multiRenderer_ && multiRenderer_->isInitialized()) {
                // Make GL context current before any GL operations (FBO creation, etc.)
                if (!surfaces_.empty()) {
                    surfaces_.begin()->second->makeCurrent();
                }
                
                std::vector<OutputSurface*> surfacePtrs;
                for (const auto& reg : outputRegions_) {
                    surfacePtrs.push_back(surfaces_.at(reg.name).get());
                }
                multiRenderer_->configureOutputs(outputRegions_, surfacePtrs);
            }
            
            LOG_INFO << "DRMBackend: Configured output region " << outputName;
            return true;
        }
    }
    
    LOG_WARNING << "DRMBackend: Output not found: " << outputName;
    return false;
}

int64_t DRMBackend::getTotalDroppedFrames() const {
    int64_t total = 0;
    for (const auto& [name, surface] : surfaces_) {
        if (surface) {
            total += surface->getPresentationTiming().getTotalDroppedFrames();
        }
    }
    return total;
}

void DRMBackend::setVideoFramerate(double fps) {
    // Set video framerate on all surfaces' presentation timing
    // This tells them to expect vsync skips (e.g., 25fps on 60Hz display)
    for (auto& [name, surface] : surfaces_) {
        if (surface) {
            surface->getPresentationTiming().setVideoFramerate(fps);
        }
    }
}

int DRMBackend::measureDisplayLatencyMs(int warmupFrames, int sampleFrames,
                                        StartupSplash* splash) {
    constexpr int kPanelResponseMs = 5;
    constexpr int kFallbackMs = 33;
    constexpr double kWallClockTimeoutSec = 5.0;
    constexpr int64_t kVsyncP95MultiplierLimit = 6;
    constexpr size_t kMinAcceptableSamples = 30;
    constexpr int kMaxPreFill = 6;

    if (surfaces_.empty()) {
        LOG_INFO << "DisplayLatency: no surfaces, falling back to " << kFallbackMs << " ms";
        return kFallbackMs;
    }

    // Helper: 2 × vsync derived from a surface's actual refresh rate.
    auto refreshFallbackMs = [](const DRMSurface* s) -> int {
        if (!s) {
            return kFallbackMs;
        }
        double hz = s->getOutputInfo().refreshRate;
        if (hz <= 0.0) {
            return kFallbackMs;
        }
        return static_cast<int>(2.0 * (1000.0 / hz) + 0.5);
    };

    DRMSurface* primary = getPrimarySurface();
    if (primary) {
        primary->makeCurrent();
    }

    if (splash) {
        splash->loadFromEmbedded();  // safe to call repeatedly
    }

    // Reset & enable capture per-surface
    for (auto& [name, surface] : surfaces_) {
        if (!surface || !surface->isInitialized()) {
            continue;
        }
        auto& pt = surface->getPresentationTiming();
        pt.reset();
        // Capacity = warmup + pre-fill + steady-state + drain (generous)
        pt.enableLatencyCapture(static_cast<size_t>(warmupFrames + kMaxPreFill + sampleFrames + 8));
    }

    const int totalFrames = warmupFrames + sampleFrames;
    auto loopStart = std::chrono::steady_clock::now();

    auto wallClockExceeded = [&]() -> bool {
        return std::chrono::steady_clock::now() - loopStart >
               std::chrono::duration<double>(kWallClockTimeoutSec);
    };

    auto renderOne = [&](DRMSurface* s, int frameIdx) -> void {
        int w = static_cast<int>(s->getWidth());
        int h = static_cast<int>(s->getHeight());
        if (splash) {
            splash->renderMeasurementFrame(w, h, frameIdx, totalFrames);
        } else {
            float t = static_cast<float>(frameIdx) / static_cast<float>(std::max(1, totalFrames));
            glViewport(0, 0, w, h);
            glClearColor(0.5f + 0.5f * std::sin(t * 6.28318f),
                         0.5f + 0.5f * std::sin(t * 6.28318f + 2.094f),
                         0.5f + 0.5f * std::sin(t * 6.28318f + 4.188f),
                         1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    };

    // ===== Phase A — Warmup (synchronous, frame-outer; unchanged from Phase 1) =====
    bool extendedWarmupApplied = false;
    int extraWarmup = 0;
    int frame = 0;
    while (frame < warmupFrames + extraWarmup) {
        if (wallClockExceeded()) {
            LOG_WARNING << "DisplayLatency: wall-clock timeout in warmup at frame " << frame;
            break;
        }
        for (auto& [name, surface] : surfaces_) {
            if (!surface || !surface->isInitialized()) continue;
            if (!surface->beginFrame()) continue;
            renderOne(surface.get(), frame);
            surface->endFrame();
            if (surface->isFlipPending()) surface->waitForFlip();
            surface->schedulePageFlip();
        }
        for (auto& [name, surface] : surfaces_) {
            if (surface && surface->isFlipPending()) surface->waitForFlip();
        }

        // Extended-warmup detection: at the end of warmup, if any surface
        // has produced zero captured submit→flip pairings, we're stuck in
        // SetCrtc-path. Extend warmup once. Lives here (frame-outer warmup
        // phase) and uses the global frame counter — semantics preserved
        // from Phase 1.
        if (frame == warmupFrames - 1 && !extendedWarmupApplied) {
            bool anyEmpty = false;
            for (auto& [name, surface] : surfaces_) {
                if (!surface || !surface->isInitialized()) continue;
                auto stats = surface->getPresentationTiming().getSwapChainLatencyStats();
                if (stats.sampleCount == 0) {
                    anyEmpty = true;
                    break;
                }
            }
            if (anyEmpty) {
                LOG_INFO << "DisplayLatency: no real page-flip events after "
                         << warmupFrames << " warmup frames, extending warmup once";
                extraWarmup = warmupFrames;
                extendedWarmupApplied = true;
            }
        }
        ++frame;
    }

    // ===== Phase B — Pre-fill (per-surface, capped, async tryAsyncFlip) =====
    // KMS only allows ONE in-flight flip per CRTC — submitting another while
    // one is pending returns EBUSY at the kernel layer (or -EINVAL via our
    // own flipPending_ guard). So in practice the pre-fill exits after the
    // first successful submit on most drivers. That's fine: the steady-state
    // phase still measures swap_chain + kernel-queue latency correctly,
    // since waitForFlip → render → tryAsyncFlip cycles through GBM buffers
    // exactly as production rendering does.
    int preFillFrameIdx = warmupFrames;
    for (auto& [name, surface] : surfaces_) {
        if (!surface || !surface->isInitialized()) continue;
        if (wallClockExceeded()) break;
        int submitted = 0;
        while (submitted < kMaxPreFill) {
            if (!surface->beginFrame()) break;
            renderOne(surface.get(), preFillFrameIdx);
            surface->endFrame();
            int ret = surface->tryAsyncFlip();
            if (ret == 0) {
                ++submitted;
                ++preFillFrameIdx;
            } else {
                // Any non-zero ret (EBUSY/ENOSPC/EINVAL/etc.) ⇒ done pre-filling.
                // Steady-state will resume from a known one-in-flight state.
                break;
            }
        }
    }

    // ===== Phase C — Steady-state (frame-outer, sampleFrames iterations) =====
    int steadyFrameIdx = preFillFrameIdx;
    for (int s_frame = 0; s_frame < sampleFrames; ++s_frame) {
        if (wallClockExceeded()) {
            LOG_WARNING << "DisplayLatency: wall-clock timeout in steady-state at frame " << s_frame;
            break;
        }
        for (auto& [name, surface] : surfaces_) {
            if (!surface || !surface->isInitialized()) continue;
            // Block for the oldest pending flip — submit→flip delta = the
            // measurement we want.
            if (surface->isFlipPending()) surface->waitForFlip();
            if (!surface->beginFrame()) continue;
            renderOne(surface.get(), steadyFrameIdx);
            surface->endFrame();
            int ret = surface->tryAsyncFlip();
            if (ret != 0 && ret != EBUSY && ret != ENOSPC) {
                LOG_DEBUG << "DisplayLatency[" << name << "]: tryAsyncFlip mid-steady-state errno="
                          << ret << " — skipping frame";
            }
            ++steadyFrameIdx;
        }
    }

    // ===== Phase D — Drain (let all in-flight flips complete) =====
    for (auto& [name, surface] : surfaces_) {
        if (!surface || !surface->isInitialized()) continue;
        if (wallClockExceeded()) break;
        while (surface->isFlipPending()) {
            surface->waitForFlip();
        }
    }

    // ===== Phase E — Fade-out (synchronous, production schedulePageFlip path) =====
    // Smoothly fade the aura + logo to black over kFadeFrames so the
    // measurement window doesn't cut hard to the engine's first cue.
    // The compensation value is already known by this point — this phase
    // is presentation-only and never affects the returned latency.
    constexpr int kFadeFrames = 60;  // ~1.0 s at 60 Hz
    int fadeBaseFrame = steadyFrameIdx;
    for (int f = 0; f < kFadeFrames; ++f) {
        if (wallClockExceeded()) {
            LOG_WARNING << "DisplayLatency: wall-clock timeout in fade phase at frame " << f;
            break;
        }
        float intensity = 1.0f - static_cast<float>(f + 1) / static_cast<float>(kFadeFrames);
        if (intensity < 0.0f) intensity = 0.0f;
        for (auto& [name, surface] : surfaces_) {
            (void)name;
            if (!surface || !surface->isInitialized()) continue;
            if (!surface->beginFrame()) continue;
            if (splash) {
                int w = static_cast<int>(surface->getWidth());
                int h = static_cast<int>(surface->getHeight());
                splash->renderMeasurementFrame(w, h, fadeBaseFrame + f, totalFrames, intensity);
            } else {
                glViewport(0, 0, static_cast<int>(surface->getWidth()),
                           static_cast<int>(surface->getHeight()));
                glClearColor(intensity * 0.3f, intensity * 0.2f, intensity * 0.4f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            surface->endFrame();
            if (surface->isFlipPending()) surface->waitForFlip();
            surface->schedulePageFlip();
        }
        for (auto& [name, surface] : surfaces_) {
            if (surface && surface->isFlipPending()) surface->waitForFlip();
        }
    }

    // ===== Aggregate: per-surface stats → per-surface total → max across surfaces =====
    int appliedMs = 0;
    int validSurfaces = 0;
    int fallbackMaxMs = 0;
    for (auto& [name, surface] : surfaces_) {
        if (!surface || !surface->isInitialized()) {
            continue;
        }
        auto& pt = surface->getPresentationTiming();
        auto stats = pt.getSwapChainLatencyStats();
        double hz = surface->getOutputInfo().refreshRate;
        if (hz <= 0.0) {
            hz = 60.0;
        }
        int64_t expectedVsyncNs = static_cast<int64_t>(1e9 / hz);
        int scanoutMs = static_cast<int>((expectedVsyncNs / 1e6) + 0.5);
        int derivedFallback = refreshFallbackMs(surface.get());
        if (derivedFallback > fallbackMaxMs) {
            fallbackMaxMs = derivedFallback;
        }

        if (!stats.valid || stats.sampleCount < kMinAcceptableSamples ||
            stats.p95Ns > kVsyncP95MultiplierLimit * expectedVsyncNs) {
            LOG_WARNING << "DisplayLatency[" << name << "]: measurement noisy (n="
                        << stats.sampleCount << ", p95="
                        << (stats.p95Ns / 1e6) << "ms / 6×vsync="
                        << (kVsyncP95MultiplierLimit * expectedVsyncNs / 1e6)
                        << "ms) — using refresh-derived fallback " << derivedFallback << "ms";
            pt.disableLatencyCapture();
            continue;
        }

        int swapChainMs = static_cast<int>((stats.medianNs / 1e6) + 0.5);
        int totalMs = swapChainMs + scanoutMs + kPanelResponseMs;
        LOG_INFO << "DisplayLatency[" << name << "]: swap_chain=median "
                 << (stats.medianNs / 1e6) << "ms (p95 " << (stats.p95Ns / 1e6)
                 << ", n=" << stats.sampleCount
                 << " / expected_vsync " << (expectedVsyncNs / 1e6)
                 << "ms) refresh=" << hz
                 << "Hz scanout=" << scanoutMs
                 << "ms panel=" << kPanelResponseMs
                 << "ms => " << totalMs << "ms";
        if (totalMs > appliedMs) {
            appliedMs = totalMs;
        }
        ++validSurfaces;
        pt.disableLatencyCapture();
    }

    if (validSurfaces == 0) {
        int derived = (fallbackMaxMs > 0) ? fallbackMaxMs : kFallbackMs;
        LOG_INFO << "DisplayLatency: applied = " << derived
                 << "ms (refresh-rate-derived fallback)";
        return derived;
    }
    LOG_INFO << "DisplayLatency: applied = " << appliedMs << "ms (auto)";
    return appliedMs;
}

} // namespace videocomposer

