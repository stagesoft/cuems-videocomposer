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

// The DRM PRIME export holder (1amfwm7gq3x, plan cuems-RELATIONS
// Plans/2026-11-18-vc-va-export-off-render-thread-1amfwm7gq3x.md, AC1): the
// only owner of an exported surface's fds. Pipe fds stand in for DRM PRIME
// fds; "closed" is checked with fcntl and with the count of /proc/self/fd.

#include "TestFramework.h"
#include "../hwdec/ExportedSurface.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <unistd.h>

using namespace videocomposer;

namespace {

int openFdCount() {
    int n = 0;
    DIR* d = opendir("/proc/self/fd");
    if (!d) return -1;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] != '.') ++n;
    }
    closedir(d);
    return n - 1;   // the directory's own fd
}

bool fdOpen(int fd) { return fcntl(fd, F_GETFD) != -1 || errno != EBADF; }

// A descriptor whose two objects are the two ends of a fresh pipe.
bool pipeDescriptor(VADRMPRIMESurfaceDescriptor& d, int fds[2]) {
    std::memset(&d, 0, sizeof(d));
    if (pipe2(fds, O_CLOEXEC) != 0) return false;
    d.fourcc = VA_FOURCC_NV12;
    d.width = 1920;
    d.height = 1088;
    d.num_objects = 2;
    d.objects[0].fd = fds[0];
    d.objects[0].size = 1920 * 1088;
    d.objects[1].fd = fds[1];
    d.objects[1].size = 1920 * 544;
    d.num_layers = 2;
    d.layers[0].num_planes = 1;
    d.layers[0].pitch[0] = 1920;
    d.layers[1].num_planes = 1;
    d.layers[1].object_index[0] = 1;
    d.layers[1].pitch[0] = 1920;
    return true;
}

} // namespace

// The holder closes each fd it owns when it goes, and only then.
bool test_ExportedSurface_ClosesOwnedFds() {
    const int start = openFdCount();
    VADRMPRIMESurfaceDescriptor d;
    int fds[2];
    TEST_ASSERT_TRUE(pipeDescriptor(d, fds));
    {
        auto h = std::make_shared<const ExportedSurface>(7u, d);
        TEST_ASSERT_EQ(h->surface(), 7u);
        TEST_ASSERT_EQ(h->descriptor().num_objects, 2u);
        TEST_ASSERT_EQ(h->descriptor().objects[1].fd, fds[1]);
        TEST_ASSERT_EQ(h->descriptor().layers[1].object_index[0], 1u);
        TEST_ASSERT_TRUE(fdOpen(fds[0]));
        TEST_ASSERT_TRUE(fdOpen(fds[1]));
        TEST_ASSERT_EQ(openFdCount(), start + 2);
    }
    TEST_ASSERT_FALSE(fdOpen(fds[0]));
    TEST_ASSERT_FALSE(fdOpen(fds[1]));
    TEST_ASSERT_EQ(openFdCount(), start);
    return true;
}

// Shared between a queue entry and the render thread's borrowed copy: the
// fds stay open until the last copy goes.
bool test_ExportedSurface_LastCopyCloses() {
    const int start = openFdCount();
    VADRMPRIMESurfaceDescriptor d;
    int fds[2];
    TEST_ASSERT_TRUE(pipeDescriptor(d, fds));
    std::shared_ptr<const ExportedSurface> entry = std::make_shared<const ExportedSurface>(3u, d);
    std::shared_ptr<const ExportedSurface> borrowed = entry;
    entry.reset();
    TEST_ASSERT_TRUE(fdOpen(fds[0]));
    TEST_ASSERT_TRUE(fdOpen(fds[1]));
    // A consumer works on dups and closes them itself; the holder's fds are untouched.
    const int dup0 = dup(borrowed->descriptor().objects[0].fd);
    TEST_ASSERT_TRUE(dup0 >= 0);
    close(dup0);
    TEST_ASSERT_TRUE(fdOpen(fds[0]));
    borrowed.reset();
    TEST_ASSERT_FALSE(fdOpen(fds[0]));
    TEST_ASSERT_FALSE(fdOpen(fds[1]));
    TEST_ASSERT_EQ(openFdCount(), start);
    return true;
}

// An object with fd -1 is skipped, and a count beyond the 4-object array is
// clamped instead of read past it.
bool test_ExportedSurface_SkipsInvalidAndClamps() {
    const int start = openFdCount();
    int fds[2];
    TEST_ASSERT_TRUE(pipe2(fds, O_CLOEXEC) == 0);
    VADRMPRIMESurfaceDescriptor d;
    std::memset(&d, 0, sizeof(d));
    d.num_objects = 6;
    d.objects[0].fd = fds[0];
    d.objects[1].fd = -1;
    d.objects[2].fd = fds[1];
    d.objects[3].fd = -1;
    {
        ExportedSurface h(1u, d);
        TEST_ASSERT_EQ(h.descriptor().num_objects, ExportedSurface::kMaxObjects);
        TEST_ASSERT_EQ(ExportedSurface::kMaxObjects, 4u);
    }
    TEST_ASSERT_FALSE(fdOpen(fds[0]));
    TEST_ASSERT_FALSE(fdOpen(fds[1]));
    TEST_ASSERT_EQ(openFdCount(), start);
    return true;
}

// isFor: the export belongs to a frame only if that frame is a VAAPI frame on
// the same surface (the render thread's check before it uses the export).
bool test_ExportedSurface_IsForChecksSurface() {
    VADRMPRIMESurfaceDescriptor d;
    std::memset(&d, 0, sizeof(d));   // no objects: nothing to close
    const ExportedSurface h(42u, d);

    AVFrame* f = av_frame_alloc();
    TEST_ASSERT_TRUE(f != nullptr);
    f->format = AV_PIX_FMT_VAAPI;
    f->data[3] = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(42));
    TEST_ASSERT_TRUE(h.isFor(f));
    f->data[3] = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(43));
    TEST_ASSERT_FALSE(h.isFor(f));
    f->data[3] = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(42));
    f->format = AV_PIX_FMT_NV12;
    TEST_ASSERT_FALSE(h.isFor(f));
    TEST_ASSERT_FALSE(h.isFor(nullptr));
    f->data[3] = nullptr;   // not a buffer: nothing for av_frame_free to release
    av_frame_free(&f);
    return true;
}
