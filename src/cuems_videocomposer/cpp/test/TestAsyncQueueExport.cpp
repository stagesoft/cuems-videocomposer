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

// The decode thread's export travels with its frame through AsyncDecodeQueue
// (1amfwm7gq3x, plan cuems-RELATIONS
// Plans/2026-11-18-vc-va-export-off-render-thread-1amfwm7gq3x.md, AC1).
//
// Every export here is a pipe standing in for the DRM PRIME fds, made by the
// test exporter (setExporterForTesting), whose surface id is the frame's
// timestamp. So "frame N paired with frame N's export" is checked as
// export->surface() == the timestamp of the frame getFrame() returned, and
// "no fd leaked" as the count of /proc/self/fd back at its start value.
//
// Two halves: entries put in the queue by hand (the five getFrame() return
// sites, a miss, the deque shifts, the dedup replace, a borrowed export
// outliving its entry), and a real software decode of an mpeg4 clip with
// B-frames and a 25-frame GOP made with the ffmpeg CLI (the drain path, the
// post-seek catch-up skip, seek(), the backward and forward jump clears, the
// loop boundary's virtual -> real conversion, close(), a reopen). The second
// half is skipped without ffmpeg.

#include "TestFramework.h"
#include "../input/AsyncDecodeQueue.h"
#include "../hwdec/ExportedSurface.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

namespace videocomposer {

// Friend of AsyncDecodeQueue (CUEMS_VC_TESTING): reaches the private entry
// type and the one sorted insert both decode paths use.
struct AsyncDecodeQueueTestAccess {
    using QF = AsyncDecodeQueue::QueuedFrame;

    static void insert(AsyncDecodeQueue& q, int64_t number, AVFrame* frame,
                       std::shared_ptr<const ExportedSurface> exported) {
        QF qf;
        qf.frameNumber = number;
        qf.frame = frame;
        qf.ready = true;
        qf.exported = std::move(exported);
        q.insertSorted(std::move(qf));
    }

    // (frame number, frame timestamp, export surface id or -1) per entry, in order
    static std::vector<std::tuple<int64_t, int64_t, int64_t>> entries(AsyncDecodeQueue& q) {
        std::lock_guard<std::mutex> lock(q.queueMutex_);
        std::vector<std::tuple<int64_t, int64_t, int64_t>> out;
        for (const auto& qf : q.frameQueue_) {
            const AVFrame* f = qf.frame;
            const int64_t ts = f ? (f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts) : -1;
            out.emplace_back(qf.frameNumber, ts, qf.exported ? static_cast<int64_t>(qf.exported->surface()) : -1);
        }
        return out;
    }
};

} // namespace videocomposer

using namespace videocomposer;
using Access = AsyncDecodeQueueTestAccess;

namespace {

int fdCount() {
    int n = 0;
    DIR* d = opendir("/proc/self/fd");
    if (!d) return -1;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] != '.') ++n;
    }
    closedir(d);
    return n - 1;
}

bool fdOpen(int fd) { return fcntl(fd, F_GETFD) != -1 || errno != EBADF; }

int64_t tagOf(const AVFrame* f) {
    return f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
}

// An export standing in for vaExportSurfaceHandle's: two pipe fds, surface id = tag.
std::shared_ptr<const ExportedSurface> pipeExport(int64_t tag) {
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return nullptr;
    VADRMPRIMESurfaceDescriptor d;
    std::memset(&d, 0, sizeof(d));
    d.num_objects = 2;
    d.objects[0].fd = fds[0];
    d.objects[1].fd = fds[1];
    return std::make_shared<const ExportedSurface>(static_cast<VASurfaceID>(tag), d);
}

// A 2x2 GRAY8 frame (a real buffer, so getFrame()'s av_frame_ref works) tagged by its timestamp.
AVFrame* tagFrame(int64_t tag) {
    AVFrame* f = av_frame_alloc();
    if (!f) return nullptr;
    f->format = AV_PIX_FMT_GRAY8;
    f->width = 2;
    f->height = 2;
    if (av_frame_get_buffer(f, 0) < 0) {
        av_frame_free(&f);
        return nullptr;
    }
    f->pts = tag;
    f->best_effort_timestamp = tag;
    return f;
}

void entry(AsyncDecodeQueue& q, int64_t number, int64_t tag, bool exported = true) {
    Access::insert(q, number, tagFrame(tag), exported ? pipeExport(tag) : nullptr);
}

// The borrowed frame and the borrowed export name the same frame.
bool paired(const AsyncDecodeQueue& q, const AVFrame* f) {
    const auto& e = q.borrowedExport();
    return f && e && static_cast<int64_t>(e->surface()) == tagOf(f);
}

} // namespace

// The five getFrame() return sites hand out the entry's own export; a miss
// hands out none; an entry without an export (the fallback) hands out none.
bool test_ExportQueue_BorrowSitesCarryTheExport() {
    const int start = fdCount();
    {
        AsyncDecodeQueue q;   // never opened: no decode thread, entries by hand
        entry(q, 3, 3);
        entry(q, 5, 5);
        entry(q, 4, 4, /*exported=*/false);
        int64_t got = -2;

        AVFrame* f = q.getFrame(3, got);                   // site 1: exact
        TEST_ASSERT_EQ(got, 3);
        TEST_ASSERT_TRUE(paired(q, f));

        q.setLoopMode(true, 50);
        entry(q, 52, 52);                                  // loop pre-buffer: virtual 50 + 2
        f = q.getFrame(2, got);                            // site 2: virtual
        TEST_ASSERT_EQ(got, 2);
        TEST_ASSERT_TRUE(paired(q, f));
        TEST_ASSERT_EQ(tagOf(f), 52);

        std::thread late([&q] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            entry(q, 7, 7);
        });
        f = q.getFrame(7, got, 1000);                      // site 3: exact, while waiting
        late.join();
        TEST_ASSERT_EQ(got, 7);
        TEST_ASSERT_TRUE(paired(q, f));

        std::thread lateVirtual([&q] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            entry(q, 59, 59);
        });
        f = q.getFrame(9, got, 1000);                      // site 4: virtual, while waiting
        lateVirtual.join();
        TEST_ASSERT_EQ(got, 9);
        TEST_ASSERT_TRUE(paired(q, f));
        TEST_ASSERT_EQ(tagOf(f), 59);

        f = q.getFrame(6, got);                            // site 5: closest earlier (5)
        TEST_ASSERT_EQ(got, 5);
        TEST_ASSERT_TRUE(paired(q, f));

        f = q.getFrame(4, got);                            // an entry without export
        TEST_ASSERT_TRUE(f != nullptr);
        TEST_ASSERT_EQ(got, 4);
        TEST_ASSERT_TRUE(q.borrowedExport() == nullptr);

        f = q.getFrame(3, got);
        TEST_ASSERT_TRUE(paired(q, f));
        f = q.getFrame(1, got);                            // miss: nothing at or before 1
        TEST_ASSERT_TRUE(f == nullptr);
        TEST_ASSERT_EQ(got, -1);
        TEST_ASSERT_TRUE(q.borrowedExport() == nullptr);   // a miss drops the export too
        q.close();
    }
    TEST_ASSERT_EQ(fdCount(), start);
    return true;
}

// Entries inserted out of order (deque inserts shift the entries behind) and a
// dedup replace keep every export with its own frame; the replaced export's
// fds are closed at once unless the render thread still borrows it, and then
// at its next getFrame().
bool test_ExportQueue_ShiftsAndDedupKeepPairs() {
    const int start = fdCount();
    {
        AsyncDecodeQueue q;
        for (int64_t n : {5, 2, 9, 7, 3, 8}) entry(q, n, n);
        auto es = Access::entries(q);
        TEST_ASSERT_EQ(es.size(), 6u);
        int64_t prev = -1;
        for (const auto& e : es) {
            TEST_ASSERT_TRUE(std::get<0>(e) > prev);
            prev = std::get<0>(e);
            TEST_ASSERT_EQ(std::get<1>(e), std::get<0>(e));
            TEST_ASSERT_EQ(std::get<2>(e), std::get<1>(e));
        }
        TEST_ASSERT_EQ(fdCount(), start + 12);

        // a newer decode of frame 7: frame and export replaced together
        Access::insert(q, 7, tagFrame(1007), pipeExport(1007));
        TEST_ASSERT_EQ(fdCount(), start + 12);             // old export closed, new one open
        for (const auto& e : Access::entries(q)) {
            TEST_ASSERT_EQ(std::get<2>(e), std::get<1>(e));
            if (std::get<0>(e) == 7) TEST_ASSERT_EQ(std::get<1>(e), 1007);
        }

        // replaced while borrowed: the render thread's copy keeps the fds open
        int64_t got = -1;
        AVFrame* f = q.getFrame(9, got);
        TEST_ASSERT_TRUE(paired(q, f));
        const int borrowedFd = q.borrowedExport()->descriptor().objects[0].fd;
        Access::insert(q, 9, tagFrame(1009), pipeExport(1009));
        TEST_ASSERT_TRUE(fdOpen(borrowedFd));
        TEST_ASSERT_EQ(tagOf(f), 9);                       // still the frame it was handed
        TEST_ASSERT_TRUE(paired(q, f));
        f = q.getFrame(9, got);                            // next getFrame: the new pair
        TEST_ASSERT_EQ(tagOf(f), 1009);
        TEST_ASSERT_TRUE(paired(q, f));
        TEST_ASSERT_FALSE(fdOpen(borrowedFd));             // the old pair's fds are gone
        TEST_ASSERT_EQ(fdCount(), start + 12);
        q.close();
    }
    TEST_ASSERT_EQ(fdCount(), start);
    return true;
}

// The entry's move operations carry the export (std::remove_if at the loop
// boundary and deque shifts move-assign entries), and a move-assign releases
// the export the destination held.
bool test_ExportQueue_MoveCarriesExport() {
    const int start = fdCount();
    {
        Access::QF a;
        a.frameNumber = 1;
        a.frame = tagFrame(1);
        a.exported = pipeExport(1);
        const int aFd = a.exported->descriptor().objects[0].fd;

        Access::QF b(std::move(a));                         // move-construct
        TEST_ASSERT_TRUE(a.exported == nullptr && a.frame == nullptr);
        TEST_ASSERT_EQ(static_cast<int64_t>(b.exported->surface()), tagOf(b.frame));

        Access::QF c;
        c.frameNumber = 2;
        c.frame = tagFrame(2);
        c.exported = pipeExport(2);
        const int cFd = c.exported->descriptor().objects[0].fd;
        c = std::move(b);                                   // move-assign over a live pair
        TEST_ASSERT_FALSE(fdOpen(cFd));                     // c's old export released
        TEST_ASSERT_TRUE(fdOpen(aFd));
        TEST_ASSERT_EQ(c.frameNumber, 1);
        TEST_ASSERT_EQ(static_cast<int64_t>(c.exported->surface()), tagOf(c.frame));
        TEST_ASSERT_TRUE(b.exported == nullptr && b.frame == nullptr);
    }
    TEST_ASSERT_EQ(fdCount(), start);
    return true;
}

namespace {

bool haveMpeg4Encoder() {
    return std::system("ffmpeg -version >/dev/null 2>&1") == 0 &&
           std::system("ffmpeg -hide_banner -encoders 2>/dev/null | grep -q ' mpeg4 '") == 0;
}

std::string exportTempDir() {
    char tmpl[] = "/tmp/vc-export-XXXXXX";
    const char* d = mkdtemp(tmpl);
    return d ? std::string(d) : std::string();
}

// 320x240, 25 fps, 4 s = 100 frames, mpeg4 with 2 B-frames and a keyframe
// every 25 frames: frames reorder (the EOF drain path queues the last ones),
// and a seek lands up to 24 frames before its target (the catch-up).
std::string makeGopClip(const std::string& dir) {
    const std::string path = dir + "/gop25.mp4";
    const std::string cmd =
        "ffmpeg -nostdin -hide_banner -loglevel error -y "
        "-f lavfi -i testsrc=s=320x240:r=25:d=4 "
        "-c:v mpeg4 -g 25 -bf 2 -q:v 5 " + path;
    return std::system(cmd.c_str()) == 0 ? path : std::string();
}

// The test exporter: records the timestamp of each frame it exports.
struct Recorder {
    std::mutex m;
    std::vector<int64_t> tags;
    std::atomic<int> skipEvery{0};   // > 0: no export for every Nth call (the fallback)
    std::atomic<int> calls{0};

    AsyncDecodeQueue::Exporter exporter() {
        return [this](const AVFrame* f) -> std::shared_ptr<const ExportedSurface> {
            const int n = ++calls;
            {
                std::lock_guard<std::mutex> l(m);
                tags.push_back(tagOf(f));
            }
            const int k = skipEvery.load();
            if (k > 0 && n % k == 0) return nullptr;
            return pipeExport(tagOf(f));
        };
    }
    void clear() {
        std::lock_guard<std::mutex> l(m);
        tags.clear();
    }
    std::vector<int64_t> snapshot() {
        std::lock_guard<std::mutex> l(m);
        return tags;
    }
};

} // namespace

// The real decode thread: every frame handed out carries its own export, on
// the sequential walk to EOF (incl. the drained B-frames), after seek(), after
// a backward and a forward jump clear, across the loop boundary, and after a
// reopen. Frames the post-seek catch-up trims at once are never exported, and
// every fd is closed by close().
bool test_ExportQueue_DecodePathsPairAndRelease() {
    if (!haveMpeg4Encoder()) {
        std::cout << "(skipped: no ffmpeg/mpeg4 encoder) ";
        return true;
    }
    const std::string dir = exportTempDir();
    TEST_ASSERT_FALSE(dir.empty());
    const std::string clip = makeGopClip(dir);
    TEST_ASSERT_FALSE(clip.empty());

    const int start = fdCount();
    Recorder rec;
    {
        AsyncDecodeQueue q;
        q.setExporterForTesting(rec.exporter());
        TEST_ASSERT_TRUE(q.open(clip, nullptr, 0, AsyncDecodeQueue::MAX_QUEUE_SIZE));
        TEST_ASSERT_FALSE(q.isHardwareDecoding());
        const int64_t total = q.getFrameCount();
        TEST_ASSERT_EQ(total, 100);

        int64_t got = -1;
        AVFrame* f = q.getFrame(0, got, 2000);
        TEST_ASSERT_TRUE(f != nullptr && got == 0);
        TEST_ASSERT_TRUE(paired(q, f));
        const int64_t t0 = tagOf(f);
        f = q.getFrame(1, got, 2000);
        TEST_ASSERT_TRUE(f != nullptr && got == 1);
        const int64_t step = tagOf(f) - t0;
        TEST_ASSERT_TRUE(step > 0);
        auto frameOf = [&](int64_t tag) { return (tag - t0) / step; };
        // An fd bound: the queue (8) + the borrowed copy + one in flight, 2 fds each.
        const int bound = start + 2 * (static_cast<int>(AsyncDecodeQueue::MAX_QUEUE_SIZE) + 4) + 2;

        // sequential walk to the end (the last frames come out of the drain)
        for (int64_t n = 2; n < total; ++n) {
            f = q.getFrame(n, got, 2000);
            TEST_ASSERT_TRUE(f != nullptr);
            TEST_ASSERT_EQ(got, n);
            TEST_ASSERT_EQ(frameOf(tagOf(f)), n);
            TEST_ASSERT_TRUE(paired(q, f));
            TEST_ASSERT_TRUE(fdCount() <= bound);
        }

        // seek(70) lands on the keyframe at 50: 50..67 are decoded and trimmed,
        // never exported; 68.. are
        rec.clear();
        q.seek(70);
        f = q.getFrame(70, got, 3000);
        TEST_ASSERT_TRUE(f != nullptr && got == 70);
        TEST_ASSERT_TRUE(paired(q, f));
        for (int64_t tag : rec.snapshot()) TEST_ASSERT_TRUE(frameOf(tag) >= 68);
        for (int64_t n = 71; n < 76; ++n) {
            f = q.getFrame(n, got, 2000);
            TEST_ASSERT_TRUE(f != nullptr && got == n && paired(q, f));
        }

        // backward jump (no loop mode): cleared and re-seeked to 5 (keyframe 0)
        rec.clear();
        f = q.getFrame(5, got, 3000);
        TEST_ASSERT_TRUE(f != nullptr && got == 5);
        TEST_ASSERT_TRUE(paired(q, f));
        // 0..2 (catch-up from keyframe 0) never exported; a stale pre-jump decode (>= 68) may be
        for (int64_t tag : rec.snapshot()) TEST_ASSERT_TRUE(frameOf(tag) >= 3);
        TEST_ASSERT_TRUE(fdCount() <= bound);

        // forward jump: 40 is far past the decoder, cleared and seeked (keyframe 25)
        rec.clear();
        f = q.getFrame(40, got, 3000);
        TEST_ASSERT_TRUE(f != nullptr && got == 40);
        TEST_ASSERT_TRUE(paired(q, f));
        for (int64_t tag : rec.snapshot()) {
            const int64_t fn = frameOf(tag);
            TEST_ASSERT_TRUE(fn >= 38 || fn < 25);   // never 25..37, the catch-up
        }

        // loop mode: EOF pre-buffers frames 0.. as virtual (100 + n), the boundary
        // converts them in place (remove_if + renumber); pairs survive both
        q.setLoopMode(true, total);
        for (int64_t n = 41; n < total; ++n) {
            f = q.getFrame(n, got, 2000);
            TEST_ASSERT_TRUE(f != nullptr && got == n && paired(q, f));
        }
        for (int64_t n = 0; n < 30; ++n) {
            f = q.getFrame(n, got, 2000);
            TEST_ASSERT_TRUE(f != nullptr);
            TEST_ASSERT_EQ(got, n);
            TEST_ASSERT_EQ(frameOf(tagOf(f)), n);
            TEST_ASSERT_TRUE(paired(q, f));
            TEST_ASSERT_TRUE(fdCount() <= bound);
        }
        for (const auto& e : Access::entries(q)) {
            TEST_ASSERT_TRUE(std::get<2>(e) == -1 || std::get<2>(e) == std::get<1>(e));
        }

        // a reopen (recovery's path: open() closes first) releases everything
        TEST_ASSERT_TRUE(q.open(clip, nullptr, 0, AsyncDecodeQueue::MAX_QUEUE_SIZE));
        TEST_ASSERT_TRUE(q.borrowedExport() == nullptr);
        for (int64_t n = 0; n < 10; ++n) {
            f = q.getFrame(n, got, 2000);
            TEST_ASSERT_TRUE(f != nullptr && got == n && paired(q, f));
        }
        q.close();
        TEST_ASSERT_TRUE(q.borrowedExport() == nullptr);
        TEST_ASSERT_EQ(fdCount(), start);
    }
    TEST_ASSERT_EQ(fdCount(), start);
    TEST_ASSERT_TRUE(rec.calls.load() > 0);
    const std::string rm = "rm -rf " + dir;
    (void)std::system(rm.c_str());
    return true;
}

// The fallback: a frame the decode thread did not export is still queued and
// handed out, without an export; the exported ones keep their own.
bool test_ExportQueue_UnexportedFramesStillPlay() {
    if (!haveMpeg4Encoder()) {
        std::cout << "(skipped: no ffmpeg/mpeg4 encoder) ";
        return true;
    }
    const std::string dir = exportTempDir();
    TEST_ASSERT_FALSE(dir.empty());
    const std::string clip = makeGopClip(dir);
    TEST_ASSERT_FALSE(clip.empty());

    const int start = fdCount();
    Recorder rec;
    rec.skipEvery = 3;
    {
        AsyncDecodeQueue q;
        q.setExporterForTesting(rec.exporter());
        TEST_ASSERT_TRUE(q.open(clip, nullptr, 0, AsyncDecodeQueue::MAX_QUEUE_SIZE));
        int withExport = 0, without = 0;
        for (int64_t n = 0; n < 60; ++n) {
            int64_t got = -1;
            AVFrame* f = q.getFrame(n, got, 2000);
            TEST_ASSERT_TRUE(f != nullptr);
            TEST_ASSERT_EQ(got, n);
            if (q.borrowedExport()) {
                TEST_ASSERT_TRUE(paired(q, f));
                ++withExport;
            } else {
                ++without;
            }
        }
        TEST_ASSERT_TRUE(withExport > 0);
        TEST_ASSERT_TRUE(without > 0);
        q.close();
    }
    TEST_ASSERT_EQ(fdCount(), start);
    const std::string rm = "rm -rf " + dir;
    (void)std::system(rm.c_str());
    return true;
}

namespace {

bool haveX264Encoder() {
    return std::system("ffmpeg -version >/dev/null 2>&1") == 0 &&
           std::system("ffmpeg -hide_banner -encoders 2>/dev/null | grep -q ' libx264 '") == 0;
}

// 640x360, 25 fps, 3 s = 75 frames, h264 high with 2 B-frames and a 25-frame
// GOP: the codec and structure of the rig's loop clips, small.
std::string makeH264Clip(const std::string& dir) {
    const std::string path = dir + "/h264gop25.mp4";
    const std::string cmd =
        "ffmpeg -nostdin -hide_banner -loglevel error -y "
        "-f lavfi -i testsrc=s=640x360:r=25:d=3 "
        "-c:v libx264 -profile:v high -pix_fmt yuv420p -g 25 -bf 2 " + path;
    return std::system(cmd.c_str()) == 0 ? path : std::string();
}

// One walk over the clip; counts the frames that came with an export that
// belongs to them (isFor) and the VAAPI frames without one.
struct WalkResult {
    int frames = 0;
    int vaapi = 0;
    int ownExport = 0;
    int foreignExport = 0;
};

WalkResult walk(AsyncDecodeQueue& q, int64_t frames) {
    WalkResult r;
    for (int64_t n = 0; n < frames; ++n) {
        int64_t got = -1;
        AVFrame* f = q.getFrame(n, got, 2000);
        if (!f || got != n) continue;
        ++r.frames;
        if (f->format == AV_PIX_FMT_VAAPI) ++r.vaapi;
        const auto& e = q.borrowedExport();
        if (e && e->isFor(f)) ++r.ownExport;
        else if (e) ++r.foreignExport;
    }
    return r;
}

} // namespace

// The production decode-thread export on the real driver (needs a VAAPI
// render node and libx264; skipped otherwise): with the export on, the pool is
// built and warmed up in get_format and every frame handed out carries its own
// export (no later export changed a surface's buffer after its warm-up, or the
// frame would come without one); with
// it off (the default, and the indexer) nothing is exported. Every fd returns.
bool test_ExportQueue_VaapiWarmUpAndDecodeThreadExport() {
    if (!haveX264Encoder()) {
        std::cout << "(skipped: no ffmpeg/libx264) ";
        return true;
    }
    AVBufferRef* dev = nullptr;
    if (av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", nullptr, 0) < 0) {
        std::cout << "(skipped: no VAAPI device at /dev/dri/renderD128 for this user) ";
        return true;
    }
    const std::string dir = exportTempDir();
    TEST_ASSERT_FALSE(dir.empty());
    const std::string clip = makeH264Clip(dir);
    TEST_ASSERT_FALSE(clip.empty());

    const int start = fdCount();
    {
        AsyncDecodeQueue q;   // default: no decode-thread export
        TEST_ASSERT_FALSE(q.decodeThreadExport());
        TEST_ASSERT_TRUE(q.open(clip, dev, AsyncDecodeQueue::EXTRA_HW_FRAMES_FULL,
                                AsyncDecodeQueue::MAX_QUEUE_SIZE));
        TEST_ASSERT_TRUE(q.isHardwareDecoding());
        const WalkResult off = walk(q, 40);
        TEST_ASSERT_EQ(off.frames, 40);
        if (off.vaapi == 0) {
            std::cout << "(skipped: h264 decoded in software here) ";
            q.close();
            av_buffer_unref(&dev);
            return true;
        }
        TEST_ASSERT_EQ(off.ownExport + off.foreignExport, 0);
        TEST_ASSERT_EQ(q.warmUpStats().surfaces, 0);
        q.close();
    }
    TEST_ASSERT_EQ(fdCount(), start);

    {
        AsyncDecodeQueue q;
        q.setDecodeThreadExport(true);
        TEST_ASSERT_TRUE(q.open(clip, dev, AsyncDecodeQueue::EXTRA_HW_FRAMES_FULL,
                                AsyncDecodeQueue::MAX_QUEUE_SIZE));
        const WalkResult on = walk(q, 75);
        const auto ws = q.warmUpStats();
        std::cout << "(pool " << ws.surfaces << ", failed " << ws.failed << ") ";
        TEST_ASSERT_TRUE(ws.surfaces >= static_cast<int>(AsyncDecodeQueue::MAX_QUEUE_SIZE));
        TEST_ASSERT_EQ(ws.failed, 0);
        TEST_ASSERT_EQ(on.frames, 75);
        TEST_ASSERT_EQ(on.vaapi, 75);
        TEST_ASSERT_EQ(on.foreignExport, 0);
        TEST_ASSERT_EQ(on.ownExport, 75);   // incl. the drained B-frames at the end
        TEST_ASSERT_EQ(ws.builds, 1);

        // a seek flushes the h264 decoder, which asks get_format again: the
        // same pool comes back (no new surfaces, no second warm-up), and the
        // frames still carry their own export
        q.seek(10);
        int afterSeek = 0;
        for (int64_t n = 10; n < 40; ++n) {
            int64_t got = -1;
            AVFrame* f = q.getFrame(n, got, 2000);
            const auto& e = q.borrowedExport();
            if (f && got == n && e && e->isFor(f)) ++afterSeek;
        }
        TEST_ASSERT_EQ(afterSeek, 30);
        TEST_ASSERT_EQ(q.warmUpStats().builds, 1);
        TEST_ASSERT_TRUE(q.warmUpStats().reuses >= 1);

        // a reopen (recovery) keeps the switch and warms up the new pool
        TEST_ASSERT_TRUE(q.open(clip, dev, AsyncDecodeQueue::EXTRA_HW_FRAMES_FULL,
                                AsyncDecodeQueue::MAX_QUEUE_SIZE));
        TEST_ASSERT_TRUE(q.decodeThreadExport());
        const WalkResult again = walk(q, 30);
        TEST_ASSERT_EQ(again.ownExport, 30);
        TEST_ASSERT_TRUE(q.warmUpStats().surfaces >= static_cast<int>(AsyncDecodeQueue::MAX_QUEUE_SIZE));
        q.close();
    }
    TEST_ASSERT_EQ(fdCount(), start);
    av_buffer_unref(&dev);
    const std::string rm = "rm -rf " + dir;
    (void)std::system(rm.c_str());
    return true;
}
