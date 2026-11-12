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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * 1amfwm7hg3u: a CPU-uploaded frame reaches the texture on the tick it is chosen, once per layer.
 * AC6 of cuems-RELATIONS Plans/2026-11-11-vc-cpu-upload-same-tick-1amfwm7hg3u.md:
 *   (a) a new identity uploads this draw's frame; (b) the same identity again (another surface in the
 *   same tick, a held frame on a later tick) skips; (c) a texture (re)creation forces an upload; (d) a new
 *   source and new prepared pixels (panorama pan on a held frame) each force one; (e) a failed map/unmap
 *   uploads straight from the CPU buffer, is not recorded, and the next draw retries the PBO; (f) the
 *   staging-failure WARNING is one per layer per 10 s.
 * Plus the generations behind the identity: LayerPlayback (setInputSource, each load, a shared
 * secondary's driver cache) and LayerDisplay (each write of the prepared buffer).
 */

#include "TestFramework.h"
#include "../display/CpuUploadPolicy.h"
#include "../input/InputSource.h"
#include "../layer/LayerDisplay.h"
#include "../layer/LayerPlayback.h"
#include "../sync/SyncSource.h"
#include "../video/FrameBuffer.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

using namespace videocomposer;
namespace cu = videocomposer::cpuupload;

namespace {

constexpr int64_t kSec = 1000LL * 1000 * 1000;

cu::Identity ident(uint64_t source, uint64_t frame, uint64_t prepared, uint64_t texture) {
    cu::Identity id;
    id.sourceGeneration = source;
    id.frameGeneration = frame;
    id.preparedGeneration = prepared;
    id.textureGeneration = texture;
    return id;
}

// One successful PBO upload of `id`, as the renderer does it when needsUpload() said yes.
bool uploadOk(cu::Policy& p, const cu::Identity& id, int64_t nowNs = 0) {
    const cu::Decision d = p.finish(cu::Staging::Ok, id, nowNs);
    return !d.directFromCpu && d.recorded && !d.warn;
}

// A file source that always delivers the frame asked for.
class FileSource : public InputSource {
public:
    bool open(const std::string&) override { return true; }
    void close() override {}
    bool isReady() const override { return true; }
    bool readFrame(int64_t, FrameBuffer&) override { ++loads; return true; }
    bool seek(int64_t) override { return true; }
    FrameInfo getFrameInfo() const override {
        FrameInfo info;
        info.width = 16;
        info.height = 8;
        info.aspect = 2.0f;
        info.framerate = 25.0;
        info.totalFrames = 250;
        info.format = PixelFormat::BGRA32;
        return info;
    }
    int64_t getCurrentFrame() const override { return 0; }
    CodecType detectCodec() const override { return CodecType::SOFTWARE; }
    bool supportsDirectGPUTexture() const override { return false; }
    DecodeBackend getOptimalBackend() const override { return DecodeBackend::CPU_SOFTWARE; }
    int loads = 0;
};

// A rolling MTC at whatever frame the harness set (25 fps, no converter: the frame is the content frame).
class FrameClock : public SyncSource {
public:
    int64_t frame = 0;
    bool connect(const char*) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    int64_t pollFrame(uint8_t* rolling) override { if (rolling) *rolling = 1; return frame; }
    int64_t getCurrentFrame() const override { return frame; }
    const char* getName() const override { return "frame-clock"; }
    double getFramerate() const override { return 25.0; }
    long getTimeMs() const override { return static_cast<long>(frame * 40); }
};

FrameBuffer bgra(int width, int height, uint8_t fill) {
    FrameInfo info;
    info.width = width;
    info.height = height;
    info.aspect = static_cast<float>(width) / height;
    info.format = PixelFormat::BGRA32;
    FrameBuffer fb;
    fb.allocate(info);
    for (size_t i = 0; i < fb.size(); ++i) fb.data()[i] = static_cast<uint8_t>(fill + i);
    return fb;
}

}  // namespace

// (a) The first draw of a layer, and a draw whose frame differs from the recorded one, upload this draw's frame.
bool test_CpuUpload_a_NewIdentityUploadsThisDraw() {
    cu::Policy p;
    const cu::Identity f1 = ident(1, 1, 0, 1);
    TEST_ASSERT_TRUE(p.needsUpload(f1));          // nothing recorded yet: no garbage, no stale first draw
    TEST_ASSERT_TRUE(uploadOk(p, f1));
    const cu::Identity f2 = ident(1, 2, 0, 1);    // the next frame chosen in a later tick
    TEST_ASSERT_TRUE(p.needsUpload(f2));
    TEST_ASSERT_TRUE(uploadOk(p, f2));
    TEST_ASSERT_TRUE(p.needsUpload(f1));          // going back to an older identity is a change too
    return true;
}

// (b) D3 (a): the same identity again (a second surface in the same tick, a held frame on a later tick) skips.
bool test_CpuUpload_b_SameIdentitySkips() {
    cu::Policy p;
    const cu::Identity f = ident(3, 10, 2, 4);
    TEST_ASSERT_TRUE(p.needsUpload(f));
    TEST_ASSERT_TRUE(uploadOk(p, f));
    TEST_ASSERT_FALSE(p.needsUpload(f));          // second surface, same tick
    TEST_ASSERT_FALSE(p.needsUpload(f));          // held frame, next tick (30p on 60 Hz)
    TEST_ASSERT_FALSE(p.needsUpload(ident(3, 10, 2, 4)));
    p.forget();
    TEST_ASSERT_TRUE(p.needsUpload(f));           // forgotten: uploads again
    return true;
}

// (c) A texture (re)creation forces an upload even for the same frame.
bool test_CpuUpload_c_TextureRecreationForcesUpload() {
    cu::Policy p;
    TEST_ASSERT_TRUE(p.needsUpload(ident(1, 5, 0, 1)));
    TEST_ASSERT_TRUE(uploadOk(p, ident(1, 5, 0, 1)));
    TEST_ASSERT_FALSE(p.needsUpload(ident(1, 5, 0, 1)));
    TEST_ASSERT_TRUE(p.needsUpload(ident(1, 5, 0, 2)));   // new texture: empty storage until uploaded
    TEST_ASSERT_TRUE(uploadOk(p, ident(1, 5, 0, 2)));
    TEST_ASSERT_FALSE(p.needsUpload(ident(1, 5, 0, 2)));
    return true;
}

// (d) A new source (a load resets the frame counters) forces an upload, even where the frame counter repeats.
bool test_CpuUpload_d_NewSourceForcesUpload() {
    cu::Policy p;
    TEST_ASSERT_TRUE(p.needsUpload(ident(1, 1, 0, 1)));
    TEST_ASSERT_TRUE(uploadOk(p, ident(1, 1, 0, 1)));
    TEST_ASSERT_TRUE(p.needsUpload(ident(2, 1, 0, 1)));
    return true;
}

// (d) A held frame whose prepared pixels change (panorama panOffset) forces an upload.
bool test_CpuUpload_d_PreparedPixelsForceUpload() {
    cu::Policy p;
    TEST_ASSERT_TRUE(p.needsUpload(ident(1, 7, 1, 1)));
    TEST_ASSERT_TRUE(uploadOk(p, ident(1, 7, 1, 1)));
    TEST_ASSERT_FALSE(p.needsUpload(ident(1, 7, 1, 1)));
    TEST_ASSERT_TRUE(p.needsUpload(ident(1, 7, 2, 1)));   // same frame, panned
    return true;
}

// (e) A failed map takes the direct path from the CPU buffer and is not recorded: the next draw retries the PBO.
bool test_CpuUpload_e_MapFailedDirectNotRecorded() {
    cu::Policy p;
    const cu::Identity f = ident(1, 1, 0, 1);
    TEST_ASSERT_TRUE(p.needsUpload(f));
    const cu::Decision d = p.finish(cu::Staging::MapFailed, f, 0);
    TEST_ASSERT_TRUE(d.directFromCpu);
    TEST_ASSERT_FALSE(d.recorded);
    TEST_ASSERT_TRUE(p.needsUpload(f));           // the same frame again: retry through the PBO
    TEST_ASSERT_TRUE(uploadOk(p, f, kSec / 60));
    TEST_ASSERT_FALSE(p.needsUpload(f));
    return true;
}

// (e) An unmap that returned GL_FALSE (store contents undefined) is the same: direct, not recorded, retried.
bool test_CpuUpload_e_UnmapFailedDirectNotRecorded() {
    cu::Policy p;
    const cu::Identity f = ident(1, 1, 0, 1);
    TEST_ASSERT_TRUE(uploadOk(p, f));
    const cu::Identity g = ident(1, 2, 0, 1);
    TEST_ASSERT_TRUE(p.needsUpload(g));
    const cu::Decision d = p.finish(cu::Staging::UnmapFailed, g, 0);
    TEST_ASSERT_TRUE(d.directFromCpu);
    TEST_ASSERT_FALSE(d.recorded);
    TEST_ASSERT_TRUE(p.needsUpload(g));
    TEST_ASSERT_TRUE(p.needsUpload(f));           // a failure also drops the older record
    return true;
}

// (e) A layer without a PBO keeps the long-standing direct path: the texture holds the frame, so it is recorded.
bool test_CpuUpload_e_NoPboDirectIsRecorded() {
    cu::Policy p;
    const cu::Identity f = ident(1, 1, 0, 1);
    TEST_ASSERT_TRUE(p.needsUpload(f));
    const cu::Decision d = p.finish(cu::Staging::NoPbo, f, 0);
    TEST_ASSERT_TRUE(d.directFromCpu);
    TEST_ASSERT_TRUE(d.recorded);
    TEST_ASSERT_FALSE(d.warn);
    TEST_ASSERT_FALSE(p.needsUpload(f));
    return true;
}

// (f) The staging-failure WARNING: one per layer per 10 s (injected clock), counting what it suppressed.
bool test_CpuUpload_f_WarningOncePerLayerPer10s() {
    cu::Policy a;
    cu::Policy b;
    const cu::Identity f = ident(1, 1, 0, 1);
    const int64_t t0 = 1000 * kSec;
    cu::Decision d = a.finish(cu::Staging::MapFailed, f, t0);
    TEST_ASSERT_TRUE(d.warn);                     // the first failure is said at once
    TEST_ASSERT_EQ(d.suppressed, 0u);
    TEST_ASSERT_FALSE(a.finish(cu::Staging::MapFailed, f, t0 + 1 * kSec).warn);
    TEST_ASSERT_FALSE(a.finish(cu::Staging::UnmapFailed, f, t0 + 5 * kSec).warn);
    TEST_ASSERT_FALSE(a.finish(cu::Staging::MapFailed, f, t0 + 10 * kSec - 1).warn);
    TEST_ASSERT_TRUE(b.finish(cu::Staging::MapFailed, f, t0 + 2 * kSec).warn);   // per layer
    d = a.finish(cu::Staging::MapFailed, f, t0 + 10 * kSec);
    TEST_ASSERT_TRUE(d.warn);
    TEST_ASSERT_EQ(d.suppressed, 3u);
    TEST_ASSERT_TRUE(uploadOk(a, f, t0 + 11 * kSec));   // successes never warn
    d = a.finish(cu::Staging::MapFailed, f, t0 + 15 * kSec);
    TEST_ASSERT_FALSE(d.warn);
    d = a.finish(cu::Staging::MapFailed, f, t0 + 20 * kSec);
    TEST_ASSERT_TRUE(d.warn);
    TEST_ASSERT_EQ(d.suppressed, 1u);
    return true;
}

// AC3 (a): the upload-cost line, once per layer every 60 s over the window, plus a final line.
bool test_CpuUpload_StatsLineEvery60s() {
    cu::Stats s;
    std::string line;
    const int64_t t0 = 50 * kSec;
    for (int i = 1; i <= 100; ++i) {
        s.add(i * 0.01, i == 100, 1920, 1080, t0 + i * (kSec / 60));   // 0.01 .. 1.00 ms, the last direct
    }
    TEST_ASSERT_FALSE(s.takeLine(4, t0 + 59 * kSec, line));
    TEST_ASSERT_TRUE(s.takeLine(4, t0 + 61 * kSec, line));
    std::cerr << "  " << line << "\n";
    TEST_ASSERT_EQ(line, std::string("CpuUploadStats: layer=4 size=1920x1080 n=100 mean=0.505ms "
                                     "p95=0.950ms max=1.000ms direct=1"));
    TEST_ASSERT_FALSE(s.takeLine(4, t0 + 62 * kSec, line));   // a new, empty window
    TEST_ASSERT_FALSE(s.takeFinalLine(4, line));
    s.add(2.0, false, 3840, 2160, t0 + 70 * kSec);
    TEST_ASSERT_TRUE(s.takeFinalLine(4, line));
    TEST_ASSERT_EQ(line, std::string("CpuUploadStats: layer=4 final size=3840x2160 n=1 mean=2.000ms "
                                     "p95=2.000ms max=2.000ms direct=0"));
    TEST_ASSERT_FALSE(s.takeFinalLine(4, line));
    return true;
}

// LayerPlayback: the source generation moves on every setInputSource (both overloads).
bool test_CpuUpload_PlaybackSourceGenerationBumpsOnSetInputSource() {
    LayerPlayback p;
    const uint64_t g0 = p.sourceGeneration();
    p.setInputSource(std::unique_ptr<InputSource>(new FileSource));
    const uint64_t g1 = p.sourceGeneration();
    TEST_ASSERT_NE(g1, g0);
    p.setInputSource(std::shared_ptr<InputSource>(new FileSource), false, true);
    const uint64_t g2 = p.sourceGeneration();
    TEST_ASSERT_NE(g2, g1);
    TEST_ASSERT_NE(g2, g0);
    return true;
}

// LayerPlayback: the frame generation moves on every load and only then; a seek back to the shown frame number
// followed by its load is a new picture although the frame number did not change.
bool test_CpuUpload_PlaybackFrameGenerationBumpsOnEachLoad() {
    LayerPlayback p;
    auto* src = new FileSource;
    p.setInputSource(std::unique_ptr<InputSource>(src));
    auto* clock = new FrameClock;
    p.setSyncSource(std::unique_ptr<SyncSource>(clock));
    p.setMtcFollow(true);

    clock->frame = 5;
    p.update();
    TEST_ASSERT_EQ(src->loads, 1);
    const uint64_t g5 = p.frameGeneration();
    p.update();                                   // held: no load
    TEST_ASSERT_EQ(src->loads, 1);
    TEST_ASSERT_EQ(p.frameGeneration(), g5);
    clock->frame = 6;
    p.update();
    TEST_ASSERT_EQ(src->loads, 2);
    const uint64_t g6 = p.frameGeneration();
    TEST_ASSERT_NE(g6, g5);

    p.seek(6);                                    // the full-file loop's seek: frame number set, no pixels
    TEST_ASSERT_EQ(p.frameGeneration(), g6);
    p.update();                                   // reloads frame 6: same number, new pixels
    TEST_ASSERT_EQ(src->loads, 3);
    TEST_ASSERT_EQ(p.getCurrentFrame(), 6);
    TEST_ASSERT_NE(p.frameGeneration(), g6);
    return true;
}

// LayerPlayback: a shared secondary's buffer is a view of its driver's, so every frame the driver caches moves
// the secondary's frame generation, whether the secondary reloaded or not.
bool test_CpuUpload_PlaybackSharedSecondaryFollowsDriverCache() {
    auto shared = std::make_shared<FileSource>();
    LayerPlayback secondary;
    secondary.setInputSource(std::shared_ptr<InputSource>(shared), true, false);
    FrameBuffer driverFrame = bgra(16, 8, 1);
    shared->setCachedFrame(10, driverFrame);
    const uint64_t g1 = secondary.frameGeneration();
    shared->setCachedFrame(11, driverFrame);      // the driver decoded the next frame into its buffer
    const uint64_t g2 = secondary.frameGeneration();
    TEST_ASSERT_NE(g2, g1);
    shared->setCachedFrame(-1, driverFrame);      // a live driver caches with -1 every frame
    TEST_ASSERT_NE(secondary.frameGeneration(), g2);
    return true;
}

// LayerDisplay: every prepareFrame that writes the prepared buffer (panorama/crop, re-run each update) moves the
// prepared generation, including a held frame whose panOffset changed; the zero-copy path does not, except when
// it takes over from the prepared buffer.
bool test_CpuUpload_DisplayPreparedGenerationBumpsOnEachWrite() {
    LayerDisplay d;
    FrameBuffer frame = bgra(16, 8, 7);
    d.setFrameInfo(frame.info());

    const uint64_t g0 = d.preparedGeneration();
    TEST_ASSERT_TRUE(d.prepareFrame(&frame, nullptr, false, false));   // zero-copy
    TEST_ASSERT_EQ(d.preparedGeneration(), g0);
    TEST_ASSERT_TRUE(d.prepareFrame(&frame, nullptr, false, false));
    TEST_ASSERT_EQ(d.preparedGeneration(), g0);

    d.getProperties().panoramaMode = true;
    d.getProperties().panOffset = 0;
    TEST_ASSERT_TRUE(d.prepareFrame(&frame, nullptr, false, false));
    const uint64_t g1 = d.preparedGeneration();
    TEST_ASSERT_NE(g1, g0);
    d.getProperties().panOffset = 4;                                   // held frame, panned
    TEST_ASSERT_TRUE(d.prepareFrame(&frame, nullptr, false, false));
    const uint64_t g2 = d.preparedGeneration();
    TEST_ASSERT_NE(g2, g1);
    const FrameBuffer* cpu = nullptr;
    const GPUTextureFrameBuffer* gpu = nullptr;
    TEST_ASSERT_FALSE(d.getPreparedFrame(cpu, gpu));
    TEST_ASSERT_TRUE(cpu != nullptr && cpu != &frame);
    TEST_ASSERT_EQ(cpu->data()[0], frame.data()[4 * 4]);              // the panned pixels

    d.getProperties().panoramaMode = false;                            // back to the source buffer
    TEST_ASSERT_TRUE(d.prepareFrame(&frame, nullptr, false, false));
    const uint64_t g3 = d.preparedGeneration();
    TEST_ASSERT_NE(g3, g2);
    TEST_ASSERT_TRUE(d.prepareFrame(&frame, nullptr, false, false));
    TEST_ASSERT_EQ(d.preparedGeneration(), g3);
    return true;
}
