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

#ifndef VIDEOCOMPOSER_OUTPUTLUTTEXTURE_H
#define VIDEOCOMPOSER_OUTPUTLUTTEXTURE_H

#include "OutputColor.h"

#include <memory>
#include <string>

namespace videocomposer {

struct CubeLut;

/**
 * OutputLutTexture - the GPU side of one output's 3D LUT (programme phase 3, plan 2.3).
 *
 * Owns one GL_TEXTURE_3D (internal format GL_RGBA16F, GL_LINEAR, GL_CLAMP_TO_EDGE on
 * s/t/r; width = red, height = green, depth = blue, which is the .cube order) built
 * from a parsed CubeLut, plus the LUT's size and domain so the blit can derive the
 * texel-centre transform (LutTransform) from the texture object alone.
 *
 * Threading / context rules (each one is a silent-black or silent-leak failure):
 * - create() and placeholderTexture() need the GL context current (the render thread;
 *   an OSC handler must makeCurrent() first, as DRMBackend::configureOutputBlend does).
 * - The upload saves and restores the active texture unit, the GL_TEXTURE_3D binding
 *   and the GL_PIXEL_UNPACK_BUFFER binding: the layer renderer binds its textures
 *   without glActiveTexture, so leaving another unit active blacks out every layer.
 * - The destructor never calls GL (the last reference can drop anywhere, with or
 *   without a context): the texture name goes onto a static deferred-delete list,
 *   drained by drainDeferred() with the context current - once per frame in the blit
 *   pass and once in DRMBackend::closeWindow() after the last regions are gone.
 *   glDeleteTextures with no context current is a silent no-op (the ~VaapiInterop
 *   lesson, 869en65tm).
 *
 * Held by OutputRegion through a shared_ptr, so every copy of a region shares it.
 */
class OutputLutTexture {
public:
    ~OutputLutTexture();

    OutputLutTexture(const OutputLutTexture&) = delete;
    OutputLutTexture& operator=(const OutputLutTexture&) = delete;

    /**
     * Upload `lut` as a 3D texture. Needs the GL context current. On failure returns
     * nullptr, keeps no texture and sets `error` (a GL error after glTexImage3D, a
     * malformed CubeLut, or no texture name).
     */
    static std::shared_ptr<OutputLutTexture> create(const CubeLut& lut, std::string& error);

    /** GL texture name (GLuint). */
    unsigned int texture() const { return texture_; }

    /** Edge length N of the N^3 LUT. */
    int size() const { return size_; }

    const float* domainMin() const { return domainMin_; }
    const float* domainMax() const { return domainMax_; }

    /** uLutScale / uLutOffset of the color blit program. */
    const LutTransform& transform() const { return transform_; }

    /**
     * A 1x1x1 GL_TEXTURE_3D bound on the LUT unit whenever the color program draws
     * without a LUT (the sampler must always see a 3D texture). Created lazily; needs
     * the GL context current. Returns 0 if it could not be created (the default
     * texture is then bound instead, which samples as black and is never read
     * because uLutEnabled is 0).
     */
    static unsigned int placeholderTexture();

    /**
     * The GL context is about to go (DRMBackend::closeWindow, before drainDeferred).
     * No GL call here: the placeholder's name goes onto the deferred list, and every
     * OutputLutTexture created so far is marked as belonging to the old context, so
     * one destroyed later drops its name instead of queueing it for a context that
     * never owned it (a later context must never bind or delete a stale name).
     */
    static void endContext();

    /**
     * Empty the deferred-delete list. With `contextCurrent` the names are deleted;
     * without it they are only dropped (they belong to a context that is going or
     * gone, whose destruction reclaims them; deleting them later in another context
     * could delete an unrelated texture that reused the name).
     */
    static void drainDeferred(bool contextCurrent);

private:
    OutputLutTexture() = default;

    unsigned int texture_ = 0;
    int size_ = 0;
    float domainMin_[3] = {0.0f, 0.0f, 0.0f};
    float domainMax_[3] = {1.0f, 1.0f, 1.0f};
    LutTransform transform_;
    unsigned int generation_ = 0;  ///< context generation at creation (see endContext)
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_OUTPUTLUTTEXTURE_H
