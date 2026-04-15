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

#include <string>
#include <cstddef>

namespace videocomposer {

class DisplayBackend;
class DisplayManager;

/**
 * StartupSplash - Displays an embedded PNG logo centered on every output
 * for a configurable duration at application startup.
 *
 * Image is embedded at build time from resources/splash.png (xxd -i).
 * Primary path: DRM/KMS (render to each surface). Fallback: X11 (single window, per-monitor regions).
 */
class StartupSplash {
public:
    static constexpr double SPLASH_DURATION_SECONDS = 5.0;

    StartupSplash();
    ~StartupSplash();

    /** Load image from embedded PNG data (generated header). Returns true on success. */
    bool loadFromEmbedded();

    /** Show splash on the given backend for durationSeconds. No-op if load failed. */
    void show(DisplayBackend* backend, DisplayManager* displayManager, double durationSeconds);

private:
    unsigned char* imageData_ = nullptr;
    int imageWidth_ = 0;
    int imageHeight_ = 0;
    int imageChannels_ = 0;

    unsigned int textureId_ = 0;
    unsigned int shaderProgram_ = 0;
    unsigned int quadVAO_ = 0;
    unsigned int quadVBO_ = 0;

    bool initGL();
    void cleanupGL();
    void renderCenteredQuad(int viewportWidth, int viewportHeight);
    void showDRM(DisplayBackend* backend, double durationSeconds);
    void showX11(DisplayBackend* backend, DisplayManager* displayManager, double durationSeconds);
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_STARTUPSPLASH_H
