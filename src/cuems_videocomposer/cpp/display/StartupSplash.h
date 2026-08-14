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

#ifndef VIDEOCOMPOSER_STARTUPSPLASH_H
#define VIDEOCOMPOSER_STARTUPSPLASH_H

#include <array>
#include <cstddef>
#include <string>

namespace videocomposer {

class DisplayBackend;
class DisplayManager;

/**
 * StartupSplash - Displays an embedded PNG logo centered on every output
 * for a configurable duration at application startup.
 *
 * Image is embedded at build time from resources/splash.png (xxd -i).
 * Primary path: DRM/KMS (render to each surface). Fallback: X11 (single window, per-monitor regions).
 *
 * After splash, the surface GL state stays alive long enough for the
 * auto display-latency measurement to render frames through the same
 * full-screen composite path real cues use. renderMeasurementFrame()
 * draws a palette-pulsing radial aura behind the centered logo.
 */
class StartupSplash {
public:
#ifdef CUEMS_PROBE_SPLASH
    // Test build: long window so the operator has time to inspect the panel,
    // photograph it, and observe link-training delays (which on some HDMI
    // monitors can take several seconds to settle after a modeset).
    static constexpr double SPLASH_DURATION_SECONDS = 60.0;
#else
    static constexpr double SPLASH_DURATION_SECONDS = 10.0;
#endif

    StartupSplash();
    ~StartupSplash();

    /** Load image from embedded PNG data (generated header). Returns true on success. */
    bool loadFromEmbedded();

    /** Show splash on the given backend for durationSeconds. No-op if load failed. */
    void show(DisplayBackend* backend, DisplayManager* displayManager, double durationSeconds);

    /**
     * Render one measurement frame: full-screen radial aura cycling through
     * the active palette + centered logo composited on top. Caller is
     * responsible for makeCurrent/swap/page-flip; this function only issues
     * GL draw calls into the currently-bound framebuffer.
     *
     * @param intensity scales the final aura + logo output. 1.0 = full
     *                  visual; 0.0 = solid black framebuffer (used by the
     *                  measurement orchestrator's fade-out phase).
     */
    void renderMeasurementFrame(int viewportWidth, int viewportHeight,
                                int frameIndex, int totalFrames,
                                float intensity = 1.0f);

    /** Read-only view of the active 6-stop RGBA palette (Commit 4 wires extraction). */
    const std::array<float, 24>& getPalette() const { return palette_; }

private:
    unsigned char* imageData_ = nullptr;
    int imageWidth_ = 0;
    int imageHeight_ = 0;
    int imageChannels_ = 0;

    unsigned int textureId_ = 0;
    unsigned int shaderProgram_ = 0;
    unsigned int quadVAO_ = 0;
    unsigned int quadVBO_ = 0;

    // Measurement-pulse GL state (compiled lazily on first renderMeasurementFrame)
    unsigned int pulseProgram_ = 0;
    unsigned int pulseVAO_ = 0;
    unsigned int pulseVBO_ = 0;

    // 6 RGBA stops; default initialized to the brand fallback palette in the constructor.
    // Commit 4 will optionally overwrite this from logo median-cut extraction.
    std::array<float, 24> palette_{};

    bool initGL();
    bool ensureMeasurementGL();
    void cleanupGL();
    void renderCenteredQuad(int viewportWidth, int viewportHeight, float intensity = 1.0f);
    void showDRM(DisplayBackend* backend, double durationSeconds);
    void showX11(DisplayBackend* backend, DisplayManager* displayManager, double durationSeconds);
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_STARTUPSPLASH_H
