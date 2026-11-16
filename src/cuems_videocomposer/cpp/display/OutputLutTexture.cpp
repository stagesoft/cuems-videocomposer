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
 * OutputLutTexture.cpp - GL 3D texture of an output's LUT, state-safe upload and
 * deferred deletion (programme phase 3, plan 2.3).
 */

#include "OutputLutTexture.h"
#include "CubeLut.h"

#include <GL/glew.h>
#include <GL/gl.h>

#include <cstdio>
#include <mutex>
#include <vector>

namespace videocomposer {

namespace {

// Everything below runs on the render thread today; the mutex only makes the
// deferred list safe should a region copy ever be dropped elsewhere.
std::mutex g_mutex;
std::vector<GLuint> g_deferred;       // names waiting for a context to delete them
GLuint g_placeholder = 0;             // 1x1x1 texture of the current context, 0 = none
unsigned int g_generation = 1;        // bumped by endContext()

// Pending GL errors are sticky until read; drop the stale ones so the check after
// glTexImage3D only reports what the upload itself raised. Bounded: a lost context
// may keep returning an error.
void clearStaleGlErrors() {
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
    }
}

/**
 * Create and fill one GL_TEXTURE_3D of n^3 RGB float texels (red fastest). Saves and
 * restores the active unit, unit 2's GL_TEXTURE_3D binding, the pixel-unpack buffer
 * binding and the unpack alignment. Returns 0 and sets `error` on failure, keeping
 * no texture.
 */
GLuint uploadTexture3D(int n, const float* rgb, std::string& error) {
    GLint prevActive = GL_TEXTURE0;
    GLint prevTex3D = 0;
    GLint prevUnpackBuffer = 0;
    GLint prevAlignment = 4;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prevUnpackBuffer);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlignment);

    // Upload on the LUT unit (2), which nothing else uses
    glActiveTexture(GL_TEXTURE2);
    glGetIntegerv(GL_TEXTURE_BINDING_3D, &prevTex3D);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (tex == 0) {
        error = "glGenTextures returned no name";
    } else {
        glBindTexture(GL_TEXTURE_3D, tex);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, 0);

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);  // `rgb` is client memory
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

        clearStaleGlErrors();
        // width = red, height = green, depth = blue: the .cube order (red fastest)
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA16F, n, n, n, 0, GL_RGB, GL_FLOAT, rgb);
        const GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            char code[16];
            std::snprintf(code, sizeof(code), "0x%04X", static_cast<unsigned>(err));
            error = std::string("glTexImage3D failed (GL ") + code + ")";
            glDeleteTextures(1, &tex);  // the context is current here
            tex = 0;
        }
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlignment);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(prevUnpackBuffer));
    glBindTexture(GL_TEXTURE_3D, static_cast<GLuint>(prevTex3D));
    glActiveTexture(static_cast<GLenum>(prevActive));
    return tex;
}

} // namespace

OutputLutTexture::~OutputLutTexture() {
    if (texture_ == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    // A name from a context that has since ended is dropped, never queued: the next
    // context's drain would delete whatever texture now carries that name.
    if (generation_ == g_generation) {
        g_deferred.push_back(texture_);
    }
}

std::shared_ptr<OutputLutTexture> OutputLutTexture::create(const CubeLut& lut, std::string& error) {
    if (lut.size < CubeLut::kMinSize || lut.size > CubeLut::kMaxSize ||
        lut.rgb.size() != lut.entryCount() * 3) {
        error = "LUT data does not match its size";
        return nullptr;
    }

    const GLuint tex = uploadTexture3D(lut.size, lut.rgb.data(), error);
    if (tex == 0) {
        return nullptr;
    }

    std::shared_ptr<OutputLutTexture> out(new OutputLutTexture());
    out->texture_ = tex;
    out->size_ = lut.size;
    for (int c = 0; c < 3; ++c) {
        out->domainMin_[c] = lut.domainMin[c];
        out->domainMax_[c] = lut.domainMax[c];
    }
    out->transform_ = LutTransform::make(lut.size, lut.domainMin, lut.domainMax);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        out->generation_ = g_generation;
    }
    return out;
}

unsigned int OutputLutTexture::placeholderTexture() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_placeholder != 0) {
            return g_placeholder;
        }
    }
    // One black texel; never sampled (uLutEnabled is 0 whenever it is bound), it only
    // keeps a 3D texture on the sampler3D's unit.
    const float black[3] = {0.0f, 0.0f, 0.0f};
    std::string error;
    const GLuint tex = uploadTexture3D(1, black, error);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_placeholder = tex;
    return g_placeholder;
}

void OutputLutTexture::endContext() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_placeholder != 0) {
        g_deferred.push_back(g_placeholder);
        g_placeholder = 0;
    }
    ++g_generation;
}

void OutputLutTexture::drainDeferred(bool contextCurrent) {
    std::vector<GLuint> names;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_deferred.empty()) {
            return;
        }
        names.swap(g_deferred);
    }
    if (contextCurrent) {
        glDeleteTextures(static_cast<GLsizei>(names.size()), names.data());
    }
}

} // namespace videocomposer
