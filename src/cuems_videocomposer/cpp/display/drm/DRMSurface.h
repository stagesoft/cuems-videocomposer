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
 * DRMSurface.h - Per-output rendering surface for DRM/KMS
 * 
 * Part of the Virtual Canvas architecture for cuems-videocomposer.
 * Provides GBM+EGL rendering surface for a single DRM output.
 * Inherits from OutputSurface for compatibility with MultiOutputRenderer.
 * 
 * Features:
 * - GBM surface for buffer allocation
 * - EGL surface for OpenGL rendering
 * - Double-buffered page flipping
 * - Synchronized vsync presentation
 */

#ifndef VIDEOCOMPOSER_DRMSURFACE_H
#define VIDEOCOMPOSER_DRMSURFACE_H

#include "../OutputInfo.h"
#include "../MultiOutputRenderer.h"  // For OutputSurface base class
#include "PresentationTiming.h"
#include <chrono>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <xf86drmMode.h>
#include <vector>
#include <map>
#include <cstdint>

namespace videocomposer {

class DRMOutputManager;
struct DRMPlane;  // Forward declaration for atomic modesetting

/**
 * DRMSurface - Rendering surface for a single DRM output
 * 
 * Inherits from OutputSurface for use with MultiOutputRenderer.
 * Manages:
 * - GBM surface allocation
 * - EGL context and surface creation
 * - Framebuffer management
 * - Page flipping with vsync
 */
class DRMSurface : public OutputSurface {
public:
    /**
     * Create a surface for a specific output
     * @param outputManager DRM output manager (not owned)
     * @param outputName Name of the output (e.g., "HDMI-A-1")
     */
    DRMSurface(DRMOutputManager* outputManager, const std::string& outputName);
    ~DRMSurface() override;
    
    // ===== Initialization =====
    
    /**
     * Initialize GBM + EGL for this output
     * @param sharedContext Optional EGL context to share resources with
     * @param sharedDisplay Optional shared EGL display (required for context sharing)
     * @param sharedGbmDevice Optional shared GBM device (for resource sharing)
     * @return true on success
     */
    bool init(EGLContext sharedContext = EGL_NO_CONTEXT, 
              EGLDisplay sharedDisplay = EGL_NO_DISPLAY,
              gbm_device* sharedGbmDevice = nullptr);
    
    /**
     * Cleanup all resources
     */
    void cleanup();
    
    /**
     * Reinitialize with new mode/resolution
     * @param width New width
     * @param height New height
     * @return true on success
     */
    bool resize(int width, int height);
    
    /**
     * Check if initialized
     */
    bool isInitialized() const { return initialized_; }
    
    // ===== Rendering =====
    
    /**
     * Begin a new frame
     * Makes this surface's context current
     * @return true on success
     */
    bool beginFrame();
    
    /**
     * End current frame
     * Unlocks front buffer, prepares for swap
     */
    void endFrame();
    
    // ===== Page Flipping =====
    
    /**
     * Schedule page flip (non-blocking).
     * Production entry point. Falls back to SetCrtc on EBUSY/ENOSPC after
     * 10 consecutive strikes (legacy compatibility for unstable iGPU
     * drivers).
     * @return true on success
     */
    bool schedulePageFlip() override;

    /**
     * Async measurement entry point. Like schedulePageFlip(), but returns
     * the kernel errno directly and does NOT fall back to SetCrtc on EBUSY/
     * ENOSPC, and does NOT touch the production failCount strike counter.
     * Returns -ENOTSUP if the surface is still in warmup or SetCrtc-only
     * mode (the measurement caller is expected to drive warmup via
     * schedulePageFlip first).
     *
     * @return 0 on success, positive EBUSY/ENOSPC if the GBM pool is full
     *         (caller should waitForFlip then retry), negative -<errno> on
     *         other errors.
     */
    int tryAsyncFlip();

    /**
     * Wait for pending page flip to complete
     * Blocks until flip is done (vsync)
     */
    void waitForFlip() override;
    
    /**
     * Check if a flip is pending
     */
    bool isFlipPending() const override { return flipPending_; }

    /**
     * Can this surface take a new frame in this iteration?
     *
     * Per-surface pacing asks this instead of blocking on every surface's
     * flip. A surface that produces flip events is ready as soon as its last
     * one landed and GBM still has a spare buffer. A surface that does NOT
     * produce them -- before the first modeset, during the Intel warmup, or
     * after the SetCrtc strikeout -- would otherwise read as permanently
     * ready and spin the render loop, so it is gated on its own vsync
     * interval instead.
     */
    bool isReadyToPresent(std::chrono::steady_clock::time_point now) const;

    /**
     * When the time gate above will next let this surface through, or
     * time_point::max() if it is not time-gated. Used to size the wait.
     */
    std::chrono::steady_clock::time_point timeGateDeadline() const;

    /**
     * Abandon a flip whose completion event never arrived, so one dead CRTC
     * cannot freeze the outputs that share its refresh rate. Releases the
     * buffer the event would have released -- otherwise it stays locked
     * forever and the next flip overwrites the last pointer to it.
     *
     * @return true if a stuck flip was abandoned.
     */
    bool expireStuckFlip(std::chrono::steady_clock::time_point now);

    /**
     * True if this surface can go into an atomic request (plane resolved with
     * its properties loaded, and the mode already set). Checked BEFORE
     * prepareAtomicFlip(), which locks a GBM buffer.
     */
    bool isAtomicEligible() const;

    /**
     * Vsync interval actually in force, in nanoseconds. Comes from
     * PresentationTiming, so it carries the same 60Hz fallback init() applies
     * when the connector reports no usable rate.
     */
    int64_t vsyncIntervalNs() const;

    /** Refresh rate in force, derived from vsyncIntervalNs(). */
    double effectiveRefreshHz() const;

    /**
     * Block until ANY CRTC on this DRM fd reports a completed flip, then
     * dispatch it. This is the frame clock for per-surface pacing: the
     * coupled path used the wait-for-every-surface barrier for that, which is
     * precisely what tied every output to the slowest one.
     *
     * Static because the event handlers and the crtc->surface map they route
     * through are static members of this class; the fd is shared by every
     * surface, so one poll serves all of them.
     *
     * @param timeoutMs 0 drains without blocking; >0 waits up to that long.
     * @return true if at least one event was dispatched.
     */
    static bool waitForAnyFlip(int drmFd, int timeoutMs);
    
    /**
     * Check if GBM surface has free buffers available
     * Used to determine if we need to wait for a flip
     */
    bool hasFreeBuffers() const;
    
    /**
     * Process flip events without blocking
     * Returns true if a flip completed
     */
    bool processFlipEvents();
    
    // ===== Atomic Modesetting Support =====
    
    /**
     * Prepare for atomic page flip (lock buffer, create FB)
     * Call this instead of schedulePageFlip() when using atomic commits
     * @return Framebuffer ID on success, 0 on failure
     */
    uint32_t prepareAtomicFlip();
    
    /**
     * Finalize atomic flip after successful synchronous drmModeAtomicCommit
     * Releases buffers immediately (safe because commit blocked until vsync)
     */
    void finalizeAtomicFlip();

    /**
     * Finalize atomic flip after successful non-blocking drmModeAtomicCommit
     * Sets flipPending and defers buffer release to pageFlipHandler
     */
    void finalizeAtomicFlipAsync();
    
    /**
     * Cancel atomic flip if prepare succeeded but commit failed
     * Releases the locked buffer
     */
    void cancelAtomicFlip();
    
    /**
     * Get CRTC ID for atomic property
     */
    uint32_t getCrtcId() const { return crtcId_; }
    
    /**
     * Check if mode has been set (first frame uses SetCrtc)
     */
    bool isModeSet() const { return modeSet_; }
    
    /**
     * Get the primary plane for this surface (for atomic modesetting)
     * @return Pointer to plane, or nullptr if not available
     */
    DRMPlane* getPlane() const { return plane_; }
    
    /**
     * Get the framebuffer ID of the pending buffer
     * (for building atomic requests)
     */
    uint32_t getPendingFbId() const { return pendingFbId_; }
    
    // ===== Output Information (OutputSurface interface) =====
    
    /**
     * Get output info
     */
    const OutputInfo& getOutputInfo() const override;
    
    /**
     * Get current width
     */
    uint32_t getWidth() const override { return width_; }
    
    /**
     * Get current height
     */
    uint32_t getHeight() const override { return height_; }
    
    /**
     * Get output name (e.g., "HDMI-A-1")
     */
    const std::string& getOutputName() const { return outputName_; }
    
    // ===== EGL/OpenGL Access (OutputSurface interface) =====
    
    /**
     * Make this surface's context current
     */
    void makeCurrent() override;
    
    /**
     * Release context (make none current)
     */
    void releaseCurrent() override;
    
    /**
     * Swap buffers (for non-atomic mode)
     */
    void swapBuffers() override;
    
    /**
     * Get EGL context
     */
    EGLContext getContext() const { return eglContext_; }
    
    /**
     * Get EGL display
     */
    EGLDisplay getDisplay() const { return eglDisplay_; }
    
    /**
     * Get EGL surface
     */
    EGLSurface getSurface() const { return eglSurface_; }
    
    /**
     * Get GBM device
     */
    gbm_device* getGbmDevice() const { return gbmDevice_; }
    
    /**
     * Get GBM surface
     */
    gbm_surface* getGbmSurface() const { return gbmSurface_; }

    /**
     * True iff the cold-boot first-frame modeset verifier failed even after
     * the in-process disable->re-enable retry. The render loop checks this
     * across all surfaces and exits the process so systemd Restart=on-failure
     * recovers (instead of looping forever on a broken modeset).
     */
    bool hasFatalModesetError() const { return fatalModeset_; }

private:
    // Framebuffer info
    struct Framebuffer {
        gbm_bo* bo = nullptr;
        uint32_t fbId = 0;
    };
    
    // Create a DRM framebuffer from GBM buffer
    bool createFramebuffer(gbm_bo* bo, Framebuffer& fb);

    // Destroy framebuffer
    void destroyFramebuffer(Framebuffer& fb);

    // Internal page-flip implementation shared by schedulePageFlip and
    // tryAsyncFlip. allowSetCrtcFallback=true gives production behaviour
    // (EBUSY → setCrtc fallback, failCount strikeout); =false is the
    // measurement path (no setCrtc, no failCount touched, raw errno).
    int doPageFlip(bool allowSetCrtcFallback);
    
    // Page flip handler callback (version 2 — used for non-atomic per-surface flips)
    static void pageFlipHandler(int fd, unsigned int frame,
                                unsigned int sec, unsigned int usec,
                                void* data);

    // Page flip handler2 callback (version 3 — used for atomic flips, includes crtc_id)
    static void pageFlipHandler2(int fd, unsigned int sequence,
                                 unsigned int sec, unsigned int usec,
                                 unsigned int crtc_id, void* data);

    // Map from crtc_id to DRMSurface* for atomic flip events
    static std::map<uint32_t, DRMSurface*> s_crtcSurfaceMap_;
    
    // ===== Members =====
    
    DRMOutputManager* outputManager_;  // Not owned
    std::string outputName_;           // Name of this output (e.g., "HDMI-A-1")
    uint32_t width_;
    uint32_t height_;
    
    // GBM
    gbm_device* gbmDevice_ = nullptr;
    gbm_surface* gbmSurface_ = nullptr;
    
    // EGL
    EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
    EGLContext eglContext_ = EGL_NO_CONTEXT;
    EGLSurface eglSurface_ = EGL_NO_SURFACE;
    EGLConfig eglConfig_ = nullptr;
    
    // Framebuffers for page flipping
    Framebuffer currentFb_;
    Framebuffer nextFb_;
    gbm_bo* currentBo_ = nullptr;    // Buffer currently being displayed
    gbm_bo* previousBo_ = nullptr;   // Buffer to release after flip completes
    gbm_bo* pendingBo_ = nullptr;    // Buffer pending atomic commit
    
    // Flip state
    bool flipPending_ = false;
    bool initialized_ = false;
    // Per-surface pacing state. usesFlipEvents_ says whether this surface's
    // presents generate flip events (page flip / atomic) or not (SetCrtc);
    // lastPresentTime_ paces the ones that do not, flipSubmitTime_ dates the
    // in-flight flip so a dead CRTC can be timed out.
    bool usesFlipEvents_ = false;
    std::chrono::steady_clock::time_point lastPresentTime_{};
    std::chrono::steady_clock::time_point flipSubmitTime_{};

    bool modeSet_ = false;           // True if CRTC mode has been set (initial modeset done)
    int warmupFrames_ = 0;           // Frames remaining before trying page flip (Intel quirk)
    bool useSetCrtcOnly_ = false;    // Fall back to SetCrtc if page flip consistently fails
    bool fatalModeset_ = false;      // Cold-boot first-frame verifier failed even after retry
    
    // Ownership flags (for cleanup)
    bool ownGbmDevice_ = false;      // True if we created the GBM device
    bool ownEglDisplay_ = false;     // True if we created the EGL display
    bool ownEglContext_ = false;     // True if we created the EGL context
    
    // DRM IDs (cached)
    uint32_t connectorId_ = 0;
    uint32_t crtcId_ = 0;
    
    // Plane for atomic modesetting
    DRMPlane* plane_ = nullptr;
    uint32_t pendingFbId_ = 0;   // FB ID for pending atomic commit
    
    // Presentation timing (frame pacing like mpv)
    PresentationTiming presentationTiming_;
    
public:
    /**
     * Get presentation timing information
     */
    const PresentationTiming& getPresentationTiming() const { return presentationTiming_; }
    PresentationTiming& getPresentationTiming() { return presentationTiming_; }
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_DRMSURFACE_H

