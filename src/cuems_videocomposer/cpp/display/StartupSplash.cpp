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

/*
 * StartupSplash - Embedded PNG logo displayed on all outputs at startup.
 * Image: resources/splash.png (embedded at build time via xxd -i).
 */

#include "StartupSplash.h"
#include "DisplayBackend.h"
#include "DisplayManager.h"
#include "XineramaHelper.h"
#include "drm/DRMBackend.h"
#include "drm/DRMSurface.h"
#include "../utils/Logger.h"

#include "splash_png.h"  // Generated: splash_png, splash_png_len

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"

#include <GL/glew.h>
#include <GL/gl.h>

#include <chrono>
#include <thread>
#include <cmath>
#include <cstring>

namespace videocomposer {

namespace {

const char* VERT_SHADER = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
uniform mat4 uMVP;
void main() {
    gl_Position = uMVP * vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

const char* FRAG_SHADER = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D uTex;
void main() {
    fragColor = texture(uTex, vTexCoord);
}
)";

} // namespace

StartupSplash::StartupSplash() = default;

StartupSplash::~StartupSplash() {
    cleanupGL();
    if (imageData_) {
        stbi_image_free(imageData_);
        imageData_ = nullptr;
    }
}

bool StartupSplash::loadFromEmbedded() {
    if (imageData_) {
        return true;
    }
    int x = 0, y = 0, n = 0;
    imageData_ = stbi_load_from_memory(
        splash_png,
        static_cast<int>(splash_png_len),
        &x, &y, &n, 4);
    if (!imageData_) {
        LOG_WARNING << "StartupSplash: failed to decode embedded PNG: " << stbi_failure_reason();
        return false;
    }
    imageWidth_ = x;
    imageHeight_ = y;
    imageChannels_ = 4;
    return true;
}

bool StartupSplash::initGL() {
    if (shaderProgram_ != 0) {
        return true;
    }
    if (!imageData_) {
        return false;
    }

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &VERT_SHADER, nullptr);
    glCompileShader(vs);
    GLint ok = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(vs, sizeof(log), nullptr, log);
        LOG_WARNING << "StartupSplash vertex shader: " << log;
        glDeleteShader(vs);
        return false;
    }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &FRAG_SHADER, nullptr);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(fs, sizeof(log), nullptr, log);
        LOG_WARNING << "StartupSplash fragment shader: " << log;
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }

    shaderProgram_ = glCreateProgram();
    glAttachShader(shaderProgram_, vs);
    glAttachShader(shaderProgram_, fs);
    glLinkProgram(shaderProgram_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(shaderProgram_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(shaderProgram_, sizeof(log), nullptr, log);
        LOG_WARNING << "StartupSplash program: " << log;
        glDeleteProgram(shaderProgram_);
        shaderProgram_ = 0;
        return false;
    }

    glGenTextures(1, &textureId_);
    glBindTexture(GL_TEXTURE_2D, textureId_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 imageWidth_, imageHeight_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, imageData_);
    glBindTexture(GL_TEXTURE_2D, 0);

    float w = static_cast<float>(imageWidth_);
    float h = static_cast<float>(imageHeight_);
    float quad[] = {
        0, 0, 0, 1,
        w, 0, 1, 1,
        0, h, 0, 0,
        w, h, 1, 0
    };
    glGenVertexArrays(1, &quadVAO_);
    glGenBuffers(1, &quadVBO_);
    glBindVertexArray(quadVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return true;
}

void StartupSplash::cleanupGL() {
    if (quadVAO_) {
        glDeleteVertexArrays(1, &quadVAO_);
        quadVAO_ = 0;
    }
    if (quadVBO_) {
        glDeleteBuffers(1, &quadVBO_);
        quadVBO_ = 0;
    }
    if (textureId_) {
        glDeleteTextures(1, &textureId_);
        textureId_ = 0;
    }
    if (shaderProgram_) {
        glDeleteProgram(shaderProgram_);
        shaderProgram_ = 0;
    }
}

void StartupSplash::renderCenteredQuad(int viewportWidth, int viewportHeight) {
    float lw = static_cast<float>(imageWidth_);
    float lh = static_cast<float>(imageHeight_);
    float vw = static_cast<float>(viewportWidth);
    float vh = static_cast<float>(viewportHeight);
    float x = (vw - lw) * 0.5f;
    float y = (vh - lh) * 0.5f;
    // Ortho: left=0, right=vw, bottom=vh, top=0 (Y down for screen coords)
    float mvp[16] = {
        2.f/vw, 0, 0, 0,
        0, 2.f/vh, 0, 0,
        0, 0, -1, 0,
        -1, -1, 0, 1
    };
    // Translate to (x, y)
    mvp[12] = -1.f + 2.f * x / vw;
    mvp[13] = -1.f + 2.f * y / vh;

    glUseProgram(shaderProgram_);
    glUniformMatrix4fv(glGetUniformLocation(shaderProgram_, "uMVP"), 1, GL_FALSE, mvp);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureId_);
    glUniform1i(glGetUniformLocation(shaderProgram_, "uTex"), 0);
    glBindVertexArray(quadVAO_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

void StartupSplash::showDRM(DisplayBackend* backend, double durationSeconds) {
#ifdef HAVE_DRM_BACKEND
    DRMBackend* drm = dynamic_cast<DRMBackend*>(backend);
    if (!drm) {
        return;
    }
    const auto& surfaces = drm->getSurfaces();
    if (surfaces.empty()) {
        return;
    }
    DRMSurface* primary = drm->getPrimarySurface();
    if (primary) {
        primary->makeCurrent();
    }
    if (!initGL()) {
        if (primary) {
            primary->releaseCurrent();
        }
        return;
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    auto start = std::chrono::steady_clock::now();
    const auto duration = std::chrono::duration<double>(durationSeconds);

    while (std::chrono::steady_clock::now() - start < duration) {
        for (auto& [name, surface] : surfaces) {
            if (!surface || !surface->isInitialized()) {
                continue;
            }
            if (!surface->beginFrame()) {
                continue;
            }
            int w = static_cast<int>(surface->getWidth());
            int h = static_cast<int>(surface->getHeight());
            glViewport(0, 0, w, h);
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
            renderCenteredQuad(w, h);
            surface->endFrame();
            if (surface->isFlipPending()) {
                surface->waitForFlip();
            }
            surface->schedulePageFlip();
        }
        for (auto& [name, surface] : surfaces) {
            if (surface && surface->isFlipPending()) {
                surface->waitForFlip();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    if (primary) {
        primary->releaseCurrent();
    }
#else
    (void)backend;
    (void)durationSeconds;
#endif
}

void StartupSplash::showX11(DisplayBackend* backend, DisplayManager* displayManager, double durationSeconds) {
    if (!backend || !displayManager) {
        return;
    }
    backend->makeCurrent();
    if (!initGL()) {
        backend->clearCurrent();
        return;
    }

    unsigned int winW = 0, winH = 0;
    backend->getWindowSize(&winW, &winH);
    if (winW == 0 || winH == 0) {
        return;
    }
    int winX = 0, winY = 0;
    backend->getWindowPos(&winX, &winY);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    auto start = std::chrono::steady_clock::now();
    const auto duration = std::chrono::duration<double>(durationSeconds);

    while (std::chrono::steady_clock::now() - start < duration) {
        backend->makeCurrent();
        glViewport(0, 0, winW, winH);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);

        size_t n = displayManager->getDisplayCount();
        for (size_t i = 0; i < n; i++) {
            const DisplayInfo& info = displayManager->getDisplay(i);
            int dx = info.x - winX;
            int dy = info.y - winY;
            // GL viewport: origin bottom-left of window
            int glY = static_cast<int>(winH) - dy - info.height;
            glViewport(dx, glY, info.width, info.height);
            glScissor(dx, glY, info.width, info.height);
            glEnable(GL_SCISSOR_TEST);
            renderCenteredQuad(info.width, info.height);
            glDisable(GL_SCISSOR_TEST);
        }

        glFlush();
        backend->swapBuffers();
        backend->clearCurrent();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

void StartupSplash::show(DisplayBackend* backend, DisplayManager* displayManager, double durationSeconds) {
    if (!loadFromEmbedded()) {
        return;
    }
    if (!backend) {
        return;
    }

#ifdef HAVE_DRM_BACKEND
    if (dynamic_cast<DRMBackend*>(backend)) {
        showDRM(backend, durationSeconds);
        return;
    }
#endif
    showX11(backend, displayManager, durationSeconds);
}

} // namespace videocomposer
