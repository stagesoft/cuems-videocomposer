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

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>
#include <vector>

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
uniform float uIntensity;   // 1.0 normal, 0.0 = invisible (fade-out)
void main() {
    vec4 c = texture(uTex, vTexCoord);
    fragColor = vec4(c.rgb * uIntensity, c.a * uIntensity);
}
)";

const char* PULSE_VERT_SHADER = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
out vec2 vNDC;
void main() {
    vNDC = aPos;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// Full-screen radial aura: outer color cycles through the 6-stop palette as
// the caller advances frameIndex; inner is the same color at low intensity
// so the gradient still sweeps every pixel each frame (forces a real BO swap).
const char* PULSE_FRAG_SHADER = R"(
#version 330 core
in vec2 vNDC;
out vec4 fragColor;
uniform vec4 uPalette[6];
uniform float uPhase;       // 0..1, advances per measurement frame
uniform float uAspect;      // viewportWidth / viewportHeight
uniform float uIntensity;   // 1.0 normal, 0.0 = solid black (fade-out tail)
void main() {
    float t = fract(uPhase);
    float scaled = t * 6.0;
    int idx = int(floor(scaled));
    float local = scaled - float(idx);
    int next = (idx + 1) % 6;
    vec4 outerC = mix(uPalette[idx], uPalette[next], local);
    vec4 innerC = outerC * 0.18 + vec4(0.0, 0.0, 0.0, 1.0) * 0.82;

    // Radial position normalised to corner=1.0 (everything visible)
    vec2 p = vec2(vNDC.x * uAspect, vNDC.y);
    float r = clamp(length(p) / sqrt(uAspect * uAspect + 1.0), 0.0, 1.0);
    float curve = smoothstep(0.0, 1.0, r);
    vec4 c = mix(innerC, outerC, curve);
    fragColor = vec4(c.rgb * uIntensity, 1.0);
}
)";

// Returns true iff the image has at least 3 distinct hue bins (45° each)
// each populated by ≥5% of the non-masked pixels. Logos with one dominant
// brand hue fall through to the hardcoded fallback in StartupSplash::ctor.
bool hasEnoughHueDiversity(const unsigned char* rgba, int width, int height) {
    int bins[8] = {0};
    int totalKept = 0;
    int totalPx = width * height;
    for (int i = 0; i < totalPx; ++i) {
        unsigned char r = rgba[i * 4 + 0];
        unsigned char g = rgba[i * 4 + 1];
        unsigned char b = rgba[i * 4 + 2];
        unsigned char a = rgba[i * 4 + 3];
        if (a < 16) continue;
        int luma = (r * 299 + g * 587 + b * 114) / 1000;
        if (luma < 32 || luma > 224) continue;
        int maxc = std::max({(int)r, (int)g, (int)b});
        int minc = std::min({(int)r, (int)g, (int)b});
        int delta = maxc - minc;
        if (delta < 16) continue;  // near-grey contributes no hue
        float h = 0.0f;
        float fr = static_cast<float>(r);
        float fg = static_cast<float>(g);
        float fb = static_cast<float>(b);
        if (maxc == r) {
            h = 60.0f * std::fmod((fg - fb) / float(delta), 6.0f);
        } else if (maxc == g) {
            h = 60.0f * (((fb - fr) / float(delta)) + 2.0f);
        } else {
            h = 60.0f * (((fr - fg) / float(delta)) + 4.0f);
        }
        if (h < 0.0f) h += 360.0f;
        int bin = std::min(7, std::max(0, static_cast<int>(h / 45.0f)));
        ++bins[bin];
        ++totalKept;
    }
    if (totalKept == 0) return false;
    int floorCount = totalKept * 5 / 100;
    int populated = 0;
    for (int i = 0; i < 8; ++i) {
        if (bins[i] > floorCount) ++populated;
    }
    return populated >= 3;
}

// Median-cut palette extraction over non-masked pixels. Splits boxes along
// their longest RGB axis until 6 boxes exist; each box's mean colour is one
// stop. Stops sorted by HSV hue for smooth ring traversal in the pulse shader.
void extractPaletteMedianCut(const unsigned char* rgba, int width, int height,
                             std::array<float, 24>& palette) {
    struct Pixel { unsigned char r, g, b; };
    struct Box {
        std::vector<Pixel> px;
        int rmin = 255, rmax = 0;
        int gmin = 255, gmax = 0;
        int bmin = 255, bmax = 0;
        void compute() {
            rmin = gmin = bmin = 255;
            rmax = gmax = bmax = 0;
            for (const auto& p : px) {
                rmin = std::min(rmin, static_cast<int>(p.r));
                rmax = std::max(rmax, static_cast<int>(p.r));
                gmin = std::min(gmin, static_cast<int>(p.g));
                gmax = std::max(gmax, static_cast<int>(p.g));
                bmin = std::min(bmin, static_cast<int>(p.b));
                bmax = std::max(bmax, static_cast<int>(p.b));
            }
        }
        int range() const {
            return std::max({rmax - rmin, gmax - gmin, bmax - bmin});
        }
        int axis() const {
            int rr = rmax - rmin, gr = gmax - gmin, br = bmax - bmin;
            if (rr >= gr && rr >= br) return 0;
            if (gr >= br) return 1;
            return 2;
        }
    };

    std::vector<Pixel> kept;
    kept.reserve(static_cast<size_t>(width * height) / 4);
    int totalPx = width * height;
    for (int i = 0; i < totalPx; ++i) {
        unsigned char r = rgba[i * 4 + 0];
        unsigned char g = rgba[i * 4 + 1];
        unsigned char b = rgba[i * 4 + 2];
        unsigned char a = rgba[i * 4 + 3];
        if (a < 16) continue;
        int luma = (r * 299 + g * 587 + b * 114) / 1000;
        if (luma < 32 || luma > 224) continue;
        kept.push_back({r, g, b});
    }
    if (kept.empty()) return;

    std::vector<Box> boxes(1);
    boxes[0].px = std::move(kept);
    boxes[0].compute();

    while (boxes.size() < 6) {
        int splitIdx = -1;
        int splitRange = -1;
        for (size_t i = 0; i < boxes.size(); ++i) {
            if (boxes[i].px.size() <= 1) continue;
            int r = boxes[i].range();
            if (r > splitRange) {
                splitRange = r;
                splitIdx = static_cast<int>(i);
            }
        }
        if (splitIdx < 0 || splitRange <= 0) break;
        Box& b = boxes[splitIdx];
        int ax = b.axis();
        std::sort(b.px.begin(), b.px.end(), [ax](const Pixel& a, const Pixel& c) {
            unsigned char av = (ax == 0) ? a.r : (ax == 1) ? a.g : a.b;
            unsigned char cv = (ax == 0) ? c.r : (ax == 1) ? c.g : c.b;
            return av < cv;
        });
        size_t mid = b.px.size() / 2;
        Box left, right;
        left.px.assign(b.px.begin(), b.px.begin() + mid);
        right.px.assign(b.px.begin() + mid, b.px.end());
        left.compute();
        right.compute();
        boxes.erase(boxes.begin() + splitIdx);
        boxes.push_back(std::move(left));
        boxes.push_back(std::move(right));
    }

    struct Stop { float r, g, b, hue; };
    std::vector<Stop> stops;
    stops.reserve(boxes.size());
    for (const auto& bx : boxes) {
        if (bx.px.empty()) continue;
        long sr = 0, sg = 0, sb = 0;
        for (const auto& p : bx.px) {
            sr += p.r;
            sg += p.g;
            sb += p.b;
        }
        float fn = static_cast<float>(bx.px.size());
        float fr = (sr / fn) / 255.0f;
        float fg = (sg / fn) / 255.0f;
        float fb = (sb / fn) / 255.0f;
        float maxc = std::max({fr, fg, fb});
        float minc = std::min({fr, fg, fb});
        float delta = maxc - minc;
        float h = 0.0f;
        if (delta > 0.001f) {
            if (maxc == fr) h = 60.0f * std::fmod((fg - fb) / delta, 6.0f);
            else if (maxc == fg) h = 60.0f * (((fb - fr) / delta) + 2.0f);
            else h = 60.0f * (((fr - fg) / delta) + 4.0f);
            if (h < 0.0f) h += 360.0f;
        }
        stops.push_back({fr, fg, fb, h});
    }
    if (stops.empty()) return;
    std::sort(stops.begin(), stops.end(),
              [](const Stop& a, const Stop& b) { return a.hue < b.hue; });
    while (stops.size() < 6) {
        stops.push_back(stops.back());
    }
    for (size_t i = 0; i < 6; ++i) {
        palette[i * 4 + 0] = stops[i].r;
        palette[i * 4 + 1] = stops[i].g;
        palette[i * 4 + 2] = stops[i].b;
        palette[i * 4 + 3] = 1.0f;
    }
}

std::string formatPaletteHex(const std::array<float, 24>& palette) {
    std::ostringstream oss;
    for (size_t i = 0; i < 6; ++i) {
        if (i > 0) oss << ",";
        char buf[8];
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                      static_cast<int>(palette[i * 4 + 0] * 255.0f + 0.5f),
                      static_cast<int>(palette[i * 4 + 1] * 255.0f + 0.5f),
                      static_cast<int>(palette[i * 4 + 2] * 255.0f + 0.5f));
        oss << buf;
    }
    return oss.str();
}

} // namespace

StartupSplash::StartupSplash() {
    // FormitGo brand fallback palette — extracted from
    // https://stagelab.coop/formitgo/ (page background gradient stops,
    // accent purple, button highlight). Six stops sorted by HSV hue so
    // the radial pulse traverses smoothly during measurement. RGBA;
    // alpha=1.0. loadFromEmbedded() may overwrite this with logo
    // median-cut palette when the splash PNG has ≥3 hue bins above 5%.
    static const float kBrand[24] = {
        0.165f, 0.118f, 0.243f, 1.0f,  // #2a1e3e — radial-gradient inner (deep aubergine)
        0.384f, 0.220f, 0.408f, 1.0f,  // #623868 — content surface tint (magenta-purple)
        0.357f, 0.310f, 0.694f, 1.0f,  // #5b4fb1 — gradient bottom rgb(91,79,177)
        0.396f, 0.365f, 0.776f, 1.0f,  // #655dc6 — accent purple
        0.576f, 0.380f, 0.717f, 1.0f,  // #9361b7 — gradient top rgb(147,97,183)
        0.643f, 0.553f, 1.000f, 1.0f,  // #a48dff — button highlight (lavender)
    };
    for (size_t i = 0; i < palette_.size(); ++i) {
        palette_[i] = kBrand[i];
    }
}

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

    if (hasEnoughHueDiversity(imageData_, imageWidth_, imageHeight_)) {
        extractPaletteMedianCut(imageData_, imageWidth_, imageHeight_, palette_);
        LOG_INFO << "StartupSplash: extracted palette from logo: "
                 << formatPaletteHex(palette_);
    } else {
        LOG_INFO << "StartupSplash: low logo colour count, using brand fallback palette: "
                 << formatPaletteHex(palette_);
    }
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

bool StartupSplash::ensureMeasurementGL() {
    if (pulseProgram_ != 0) {
        return true;
    }

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &PULSE_VERT_SHADER, nullptr);
    glCompileShader(vs);
    GLint ok = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(vs, sizeof(log), nullptr, log);
        LOG_WARNING << "StartupSplash pulse vertex shader: " << log;
        glDeleteShader(vs);
        return false;
    }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &PULSE_FRAG_SHADER, nullptr);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(fs, sizeof(log), nullptr, log);
        LOG_WARNING << "StartupSplash pulse fragment shader: " << log;
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }

    pulseProgram_ = glCreateProgram();
    glAttachShader(pulseProgram_, vs);
    glAttachShader(pulseProgram_, fs);
    glLinkProgram(pulseProgram_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(pulseProgram_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(pulseProgram_, sizeof(log), nullptr, log);
        LOG_WARNING << "StartupSplash pulse program: " << log;
        glDeleteProgram(pulseProgram_);
        pulseProgram_ = 0;
        return false;
    }

    // Full-screen NDC quad
    static const float kFullscreenQuad[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };
    glGenVertexArrays(1, &pulseVAO_);
    glGenBuffers(1, &pulseVBO_);
    glBindVertexArray(pulseVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, pulseVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kFullscreenQuad), kFullscreenQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), reinterpret_cast<void*>(0));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return true;
}

void StartupSplash::renderMeasurementFrame(int viewportWidth, int viewportHeight,
                                           int frameIndex, int totalFrames,
                                           float intensity) {
    if (!ensureMeasurementGL()) {
        return;
    }
    if (totalFrames <= 0) {
        totalFrames = 1;
    }
    if (intensity < 0.0f) intensity = 0.0f;
    if (intensity > 1.0f) intensity = 1.0f;

    glViewport(0, 0, viewportWidth, viewportHeight);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    glDisable(GL_BLEND);
    glUseProgram(pulseProgram_);
    GLint locPalette = glGetUniformLocation(pulseProgram_, "uPalette");
    glUniform4fv(locPalette, 6, palette_.data());
    GLint locPhase = glGetUniformLocation(pulseProgram_, "uPhase");
    glUniform1f(locPhase, static_cast<float>(frameIndex) / static_cast<float>(totalFrames));
    GLint locAspect = glGetUniformLocation(pulseProgram_, "uAspect");
    float aspect = (viewportHeight > 0)
        ? static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight)
        : 1.0f;
    glUniform1f(locAspect, aspect);
    glUniform1f(glGetUniformLocation(pulseProgram_, "uIntensity"), intensity);

    glBindVertexArray(pulseVAO_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glUseProgram(0);

    // Composite logo on top (uses splash shader / VAO from initGL)
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (initGL()) {
        renderCenteredQuad(viewportWidth, viewportHeight, intensity);
    }
}

void StartupSplash::cleanupGL() {
    if (pulseVAO_) {
        glDeleteVertexArrays(1, &pulseVAO_);
        pulseVAO_ = 0;
    }
    if (pulseVBO_) {
        glDeleteBuffers(1, &pulseVBO_);
        pulseVBO_ = 0;
    }
    if (pulseProgram_) {
        glDeleteProgram(pulseProgram_);
        pulseProgram_ = 0;
    }
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

void StartupSplash::renderCenteredQuad(int viewportWidth, int viewportHeight, float intensity) {
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
    glUniform1f(glGetUniformLocation(shaderProgram_, "uIntensity"), intensity);
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
