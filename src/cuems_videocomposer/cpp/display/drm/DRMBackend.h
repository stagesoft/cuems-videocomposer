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
 * DRMBackend.h - DRM/KMS display backend
 * 
 * Part of the Virtual Canvas architecture for cuems-videocomposer.
 * Primary display backend for production use with lowest latency.
 * 
 * Features:
 * - Direct DRM/KMS rendering (no compositor)
 * - Multi-output support with Virtual Canvas
 * - Edge blending and warping for projection mapping
 * - Atomic modesetting
 * - GBM/EGL integration
 * - VAAPI zero-copy support
 */

#ifndef VIDEOCOMPOSER_DRMBACKEND_H
#define VIDEOCOMPOSER_DRMBACKEND_H

#include "../DisplayBackend.h"
#include "../OutputInfo.h"
#include "../OutputRegion.h"
#include "../MultiOutputRenderer.h"
#include "DRMOutputManager.h"
#include "DRMSurface.h"
#include <vector>
#include <map>
#include <memory>

namespace videocomposer {

// Forward declarations
class OpenGLRenderer;
class LayerManager;
class OSDManager;
class VirtualCanvas;
class OutputBlitShader;
class OutputSinkManager;
class DisplayConfigurationManager;
class StartupSplash;

/**
 * DRMBackend - DRM/KMS display backend for direct rendering
 * 
 * Provides:
 * - Direct GPU access via DRM/KMS
 * - Multi-output rendering with per-output surfaces
 * - Lowest possible latency (no compositor overhead)
 * - Hardware cursor support (future)
 */
class DRMBackend : public DisplayBackend {
public:
    DRMBackend();
    virtual ~DRMBackend();
    
    // ===== DisplayBackend Interface =====
    
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

    /**
     * Aggregate of DRMSurface::hasFatalModesetError across all surfaces.
     * Returns true if any surface failed cold-boot verification + retry —
     * the run loop must exit so systemd Restart=on-failure recovers.
     */
    bool hasFatalError() const override;
    
    void* getContext() override;
    void makeCurrent() override;
    void clearCurrent() override;
    
    OpenGLRenderer* getRenderer() override;
    
#ifdef HAVE_EGL
    EGLDisplay getEGLDisplay() const override;
    bool hasVaapiSupport() const override { return true; }
    PFNEGLCREATEIMAGEKHRPROC getEglCreateImageKHR() const override;
    PFNEGLDESTROYIMAGEKHRPROC getEglDestroyImageKHR() const override;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC getGlEGLImageTargetTexture2DOES() const override;
    PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC getGlEGLImageTargetTexStorageEXT() const override;
    bool isDesktopGL() const override { return true; }  // DRM/KMS always uses Desktop GL
#endif
    
#ifdef HAVE_VAAPI_INTEROP
    VADisplay getVADisplay() const override;
#endif
    
    // ===== DRM-Specific Methods =====
    
    /**
     * Get all detected outputs (override)
     */
    std::vector<OutputInfo> getOutputs() const override;
    
    /**
     * Get output count (override)
     */
    size_t getOutputCount() const override;
    
    /**
     * Configure output region (override)
     */
    bool configureOutputRegion(const std::string& outputName, int canvasX, int canvasY,
                                int canvasWidth = 0, int canvasHeight = 0) override;
    
    /**
     * Configure edge blending (override)
     */
    bool configureOutputBlend(const std::string& outputName, float left, float right,
                               float top, float bottom, float gamma = 2.2f) override;
    
    /**
     * Set output resolution/mode (override)
     */
    bool setOutputMode(const std::string& outputName, int width, int height, double refresh = 0.0) override;
    
    /**
     * Enable/disable frame capture (override)
     */
    void setCaptureEnabled(bool enabled, int width = 0, int height = 0) override;
    
    /**
     * Check if capture is enabled (override)
     */
    bool isCaptureEnabled() const override;
    
    /**
     * Set output sink manager (override)
     */
    void setOutputSinkManager(OutputSinkManager* sinkManager) override;
    
    /**
     * Set resolution mode (override)
     */
    bool setResolutionMode(const std::string& mode) override;
    void setResolutionExplicit(bool explicit_) { resolutionExplicit_ = explicit_; }
    
    /**
     * Save display configuration (override)
     */
    bool saveConfiguration(const std::string& path = "") override;
    
    /**
     * Load display configuration (override)
     */
    bool loadConfiguration(const std::string& path = "") override;
    
    /**
     * Get configuration manager
     */
    DisplayConfigurationManager* getConfigManager() { return configManager_.get(); }
    
    /**
     * Set expected video framerate for presentation timing
     * With xjadeo-style timing, video fps < display fps is normal.
     * This tells PresentationTiming to expect some vsync skips.
     * @param fps Video framerate (e.g., 25.0 for PAL)
     */
    void setVideoFramerate(double fps);
    
    /**
     * Get surface for a specific output
     */
    DRMSurface* getSurface(const std::string& name);
    
    /**
     * Get primary surface (first output)
     */
    DRMSurface* getPrimarySurface();

    /**
     * Get all surfaces (for startup splash: render logo on each output)
     */
    const std::map<std::string, std::unique_ptr<DRMSurface>>& getSurfaces() const { return surfaces_; }

    /**
     * Get DRM output manager
     */
    DRMOutputManager* getOutputManager() { return outputManager_.get(); }
    
    /**
     * Set the device path for DRM
     * Must be called before openWindow()
     */
    void setDevicePath(const std::string& path) { devicePath_ = path; }
    
    // ===== Virtual Canvas Mode =====
    
    /**
     * Enable/disable Virtual Canvas mode
     * Must be called before openWindow() or will take effect on next open.
     * 
     * Virtual Canvas mode:
     * - All layers render to a single canvas FBO
     * - Regions are blitted to outputs with blend/warp
     * - Supports layers spanning multiple outputs
     * - Required for projection mapping
     */
    void setVirtualCanvasMode(bool enabled) { useVirtualCanvas_ = enabled; }
    bool isVirtualCanvasMode() const { return useVirtualCanvas_; }
    
    /**
     * Get the multi-output renderer (for configuring output regions)
     */
    MultiOutputRenderer* getMultiOutputRenderer() { return multiRenderer_.get(); }
    
    /**
     * Get total dropped frames across all outputs (frame pacing stats)
     */
    int64_t getTotalDroppedFrames() const;

    /**
     * Measure end-to-end display latency at startup.
     *
     * Drives `warmupFrames + sampleFrames` full-screen presents through the
     * cue render path on every connected surface, measures the median
     * submit→flip latency, and returns the max-across-surfaces total
     * (swap_chain + 1 vsync scanout + 5 ms panel response) in ms.
     *
     * Returns the refresh-rate-derived 2 × vsync fallback (max across
     * surfaces) on measurement failure; returns the constructor 33 ms
     * default if no surfaces are available.
     *
     * @param warmupFrames frames to discard before sampling (covers
     *                     setCrtc warmup + GBM swap-chain ramp)
     * @param sampleFrames frames to capture for median + p95 stats
     * @param splash       optional StartupSplash for the palette-pulse
     *                     render path; nullptr falls back to a clear-only
     *                     measurement (still full-screen / forces buffer
     *                     rotation but skips the logo composite)
     */
    int measureDisplayLatencyMs(int warmupFrames, int sampleFrames,
                                StartupSplash* splash);
    
    /**
     * Configure output region in the virtual canvas
     * @param outputName Output name (e.g., "HDMI-A-1")
     * @param region Output region configuration
     */
    bool configureOutputRegion(const std::string& outputName, const OutputRegion& region);
    
    /**
     * Auto-configure output regions based on detected outputs
     * Arranges outputs side-by-side with optional overlap for blending.
     * 
     * @param arrangement "horizontal", "vertical", or "grid"
     * @param overlap Overlap in pixels (for edge blending)
     */
    void autoConfigureOutputs(const std::string& arrangement = "horizontal", int overlap = 0);
    
    /**
     * Get output regions
     */
    const std::vector<OutputRegion>& getOutputRegions() const { return outputRegions_; }
    
private:
    // DRM management
    std::unique_ptr<DRMOutputManager> outputManager_;
    std::unique_ptr<DisplayConfigurationManager> configManager_;
    std::map<std::string, std::unique_ptr<DRMSurface>> surfaces_;  // key = output name
    std::vector<std::string> outputOrder_;     // kernel enumeration order (DDI hardware order)
    std::vector<std::string> iterationOrder_;  // physical layout order: by display.conf canvas-x
                                               // ascending, alphabetical fallback for outputs not
                                               // covered or when display.conf is missing/invalid.
                                               // Drives modeset, render, flip, and cleanup
                                               // iteration so they happen in operator-intuitive
                                               // left-to-right physical order.
    
    // Rendering - Legacy mode (per-output)
    std::unique_ptr<OpenGLRenderer> renderer_;
    
    // Rendering - Virtual Canvas mode
    std::unique_ptr<MultiOutputRenderer> multiRenderer_;
    std::vector<OutputRegion> outputRegions_;
    bool useVirtualCanvas_ = true;  // Default to Virtual Canvas mode
    bool resolutionExplicit_ = false;  // True when -r was explicitly passed
    
    // Atomic modesetting
    bool atomicPageFlip();  // Returns true if successful
    
    // Configuration
    std::string devicePath_;
    int primaryOutput_ = 0;
    
    // State
    bool initialized_ = false;
    bool fullscreen_ = true;  // DRM is always "fullscreen"
    
    // EGL function pointers (for VAAPI interop)
#ifdef HAVE_EGL
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ = nullptr;
    PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC glEGLImageTargetTexStorageEXT_ = nullptr;
#endif
    
#ifdef HAVE_VAAPI_INTEROP
    VADisplay vaDisplay_ = nullptr;
#endif
    
    // Initialize EGL extensions
    void initEGLExtensions();
    
    // Initialize VAAPI
    void initVAAPI();
    
    // Initialize Virtual Canvas mode
    bool initVirtualCanvas();
    
    // Render using Virtual Canvas
    void renderVirtualCanvas(LayerManager* layerManager, OSDManager* osdManager);
    
    // Render using legacy per-output mode
    void renderLegacy(LayerManager* layerManager, OSDManager* osdManager);
    
    // Build output regions from detected outputs
    void buildOutputRegions();

    // Get surface names sorted by physical CRTC position (left-to-right, top-to-bottom)
    std::vector<std::string> getSortedOutputNames() const;

    // (Re)compute iterationOrder_ from outputRegions_ + surfaces_. Called after
    // outputRegions_ is finalized (after openWindow finishes loading display.conf,
    // and after configureOutputRegion / autoConfigureOutputs). Logs the resulting
    // order so operators can see what's driving modeset/render iteration.
    void computeIterationOrder();

    // Returns iterationOrder_ if populated, else falls back to alphabetical
    // (std::map iteration order over surfaces_) so any iteration site is safe
    // even if computeIterationOrder hasn't run yet.
    std::vector<std::string> orderedSurfaceNames() const;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_DRMBACKEND_H

