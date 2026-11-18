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

#ifndef VIDEOCOMPOSER_EXPORTEDSURFACE_H
#define VIDEOCOMPOSER_EXPORTEDSURFACE_H

#include <va/va.h>
#include <va/va_drmcommon.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <unistd.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace videocomposer {

/**
 * One VAAPI surface exported as DRM PRIME, and the only owner of its fds
 * (1amfwm7gq3x).
 *
 * vaExportSurfaceHandle hands the caller one fd per object, and the caller
 * must close them (libva va.h, "The exported handles are owned by the caller").
 * EGL's dma-buf import does not take them over either (EGL_EXT_image_dma_buf_import,
 * Issue 3). So exactly one object closes them: this one, in its destructor.
 * Whoever builds EGL images from it dup()s the fds it needs and closes its dups.
 *
 * Const after construction and neither copyable nor movable: it is shared as
 * std::shared_ptr<const ExportedSurface> between the queue entry of the frame
 * it was exported from and the render thread's borrowed copy, and the last of
 * the two to let go closes the fds.
 *
 * It also records the VASurfaceID it came from, so a consumer can check that
 * it really belongs to the AVFrame it travels with (isFor) before using it.
 */
class ExportedSurface {
public:
    /** Takes ownership of every fd in desc.objects[0 .. num_objects). */
    ExportedSurface(VASurfaceID surface, const VADRMPRIMESurfaceDescriptor& desc)
        : surface_(surface), desc_(desc) {
        // The descriptor has room for 4 objects; a larger count would make the
        // destructor read past the array.
        if (desc_.num_objects > kMaxObjects) desc_.num_objects = kMaxObjects;
    }

    ~ExportedSurface() {
        for (uint32_t i = 0; i < desc_.num_objects; ++i) {
            if (desc_.objects[i].fd >= 0) ::close(desc_.objects[i].fd);
        }
    }

    ExportedSurface(const ExportedSurface&) = delete;
    ExportedSurface& operator=(const ExportedSurface&) = delete;
    ExportedSurface(ExportedSurface&&) = delete;
    ExportedSurface& operator=(ExportedSurface&&) = delete;

    VASurfaceID surface() const { return surface_; }

    /** The descriptor as exported. Its fds stay owned by this object. */
    const VADRMPRIMESurfaceDescriptor& descriptor() const { return desc_; }

    /** True when frame is a VAAPI frame on the surface this was exported from. */
    bool isFor(const AVFrame* frame) const {
        return frame && frame->format == AV_PIX_FMT_VAAPI &&
               static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(frame->data[3])) == surface_;
    }

    /**
     * Export surface with the flags the render-thread import has always used
     * (DRM PRIME 2, read-only, separate layers). No sync is done here
     * (va.h: "This does not perform any synchronisation"); the caller syncs.
     * Returns nullptr on failure, with the VA status in *status if given.
     */
    static std::shared_ptr<const ExportedSurface> exportFrom(VADisplay display, VASurfaceID surface,
                                                             VAStatus* status = nullptr) {
        VADRMPRIMESurfaceDescriptor desc;
        std::memset(&desc, 0, sizeof(desc));
        const VAStatus st = vaExportSurfaceHandle(
            display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc);
        if (status) *status = st;
        if (st != VA_STATUS_SUCCESS) return nullptr;
        return std::make_shared<const ExportedSurface>(surface, desc);
    }

    static constexpr uint32_t kMaxObjects =
        sizeof(VADRMPRIMESurfaceDescriptor::objects) / sizeof(VADRMPRIMESurfaceDescriptor::objects[0]);

private:
    const VASurfaceID surface_;
    VADRMPRIMESurfaceDescriptor desc_;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_EXPORTEDSURFACE_H
