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

// NDI preview during montajes (ClickUp 869ekxuez / 869few41p):
//  - live layers pull frames without MTC, and never block the render loop
//  - auto-unload leaves a source of unknown length alone
//  - fit_output placement arithmetic, re-fit on a format change
//  - the journal lines another process parses (JournalContract.h)
// No NDI SDK, no display, no network: these run in CI.

#include "TestFramework.h"
#include "../input/LiveInputSource.h"
#include "../input/NdiAddress.h"
#include "../input/NdiTransport.h"
#include "../layer/LayerManager.h"
#include "../layer/LayerPlayback.h"
#include "../layer/OutputFit.h"
#include "../layer/VideoLayer.h"
#include "../remote/JournalContract.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>

using namespace videocomposer;

namespace {

// A live source whose frames are released one by one by the test.
class FakeLiveSource : public LiveInputSource {
public:
    FakeLiveSource(int width, int height, bool provisional = false)
        : provisional_(provisional) {
        info_.width = width;
        info_.height = height;
        info_.aspect = static_cast<float>(width) / height;
        info_.framerate = 25.0;
        info_.totalFrames = 0;
        info_.format = PixelFormat::BGRA32;
    }
    ~FakeLiveSource() override { close(); }

    bool open(const std::string&) override {
        ready_ = true;
        startCaptureThread();
        return true;
    }
    void close() override {
        stopCaptureThread();
        ready_ = false;
    }
    bool isReady() const override { return ready_; }
    FrameInfo getFrameInfo() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return info_;
    }
    bool isFrameInfoProvisional() const override { return provisional_; }
    int64_t getCurrentFrame() const override { return 0; }
    CodecType detectCodec() const override { return CodecType::SOFTWARE; }
    bool supportsDirectGPUTexture() const override { return false; }
    DecodeBackend getOptimalBackend() const override { return DecodeBackend::CPU_SOFTWARE; }

    void release(int frames) { pending_ += frames; }
    void setFormat(int width, int height, bool provisional) {
        std::lock_guard<std::mutex> lock(mutex_);
        info_.width = width;
        info_.height = height;
        info_.aspect = static_cast<float>(width) / height;
        provisional_ = provisional;
    }

protected:
    bool captureFrame(FrameBuffer& buffer) override {
        for (int i = 0; i < 50 && pending_.load() <= 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (pending_.load() <= 0) {
            return false;  // like NDI's capture timeout with no frame
        }
        --pending_;
        FrameInfo frame;
        frame.width = 16;
        frame.height = 9;
        frame.format = PixelFormat::BGRA32;
        return buffer.allocate(frame);
    }
    const char* getSourceTypeName() const override { return "FakeLive"; }

private:
    mutable std::mutex mutex_;
    FrameInfo info_;
    std::atomic<bool> provisional_;
    std::atomic<bool> ready_{false};
    std::atomic<int> pending_{0};
};

// A file-like source (known length) for the "files are unchanged" checks.
class FakeFileSource : public InputSource {
public:
    bool open(const std::string&) override { return true; }
    void close() override {}
    bool isReady() const override { return true; }
    bool readFrame(int64_t, FrameBuffer&) override { return true; }
    bool seek(int64_t) override { return true; }
    FrameInfo getFrameInfo() const override {
        FrameInfo info;
        info.width = 1920;
        info.height = 1080;
        info.aspect = 16.0f / 9.0f;
        info.framerate = 25.0;
        info.totalFrames = 250;
        return info;
    }
    int64_t getCurrentFrame() const override { return 0; }
    CodecType detectCodec() const override { return CodecType::SOFTWARE; }
    bool supportsDirectGPUTexture() const override { return false; }
    DecodeBackend getOptimalBackend() const override { return DecodeBackend::CPU_SOFTWARE; }
};

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs = 2000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

// test3 (FP530): three outputs side by side on a 9600x2160 canvas.
const int kCanvasW = 9600, kCanvasH = 2160;
FitRegion region(const char* name, int x, int y, int w, int h) {
    FitRegion r;
    r.name = name;
    r.x = x;
    r.y = y;
    r.width = w;
    r.height = h;
    return r;
}

// The engine's cue-path scale (cuems-engine VideoPlayer.get_layer_scale).
double engineScale(const FitRegion& r, int canvasH, int mediaW, int mediaH) {
    return std::min({static_cast<double>(mediaH) / canvasH,
                     static_cast<double>(r.width) * mediaH / (static_cast<double>(mediaW) * canvasH),
                     static_cast<double>(r.height) / canvasH});
}

}  // namespace

// --- live pull ------------------------------------------------------------

bool test_NdiPreview_LiveLayerPullsWithoutMtc() {
    auto src = std::make_unique<FakeLiveSource>(1920, 1080);
    FakeLiveSource* raw = src.get();
    src->open("fake");
    LayerPlayback playback;
    playback.setInputSource(std::move(src));  // no sync source, mtcfollow off
    TEST_ASSERT_FALSE(playback.getMtcFollow());
    TEST_ASSERT_EQ(playback.getCurrentFrame(), -1);

    raw->release(1);
    TEST_ASSERT_TRUE(waitFor([&] { playback.update(); return playback.getCurrentFrame() == 1; }));
    raw->release(1);
    TEST_ASSERT_TRUE(waitFor([&] { playback.update(); return playback.getCurrentFrame() == 2; }));

    const FrameBuffer* cpu = nullptr;
    const GPUTextureFrameBuffer* gpu = nullptr;
    TEST_ASSERT_FALSE(playback.getFrameBuffer(cpu, gpu));  // CPU frame
    TEST_ASSERT_TRUE(cpu != nullptr && cpu->isValid());
    return true;
}

bool test_NdiPreview_LivePullNeverBlocks() {
    auto src = std::make_unique<FakeLiveSource>(1920, 1080);
    src->open("fake");
    LayerPlayback playback;
    playback.setInputSource(std::move(src));
    // No frame released: every update must return at once (the old path
    // waited up to 100 ms per call, on the render thread).
    double worstMs = 0.0;
    for (int i = 0; i < 50; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        playback.update();
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        worstMs = std::max(worstMs, ms);
    }
    TEST_ASSERT_EQ(playback.getCurrentFrame(), -1);
    if (worstMs >= 20.0) {
        std::cerr << "update() took " << worstMs << " ms with no frame available\n";
        return false;
    }
    return true;
}

bool test_NdiPreview_ReadLatestFrameZeroWaitIsNonBlocking() {
    FakeLiveSource src(1920, 1080);
    src.open("fake");
    FrameBuffer buffer;
    auto t0 = std::chrono::steady_clock::now();
    TEST_ASSERT_FALSE(src.readLatestFrame(buffer, 0));
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    TEST_ASSERT_TRUE(ms < 20.0);

    src.release(1);
    TEST_ASSERT_TRUE(waitFor([&] { return src.readLatestFrame(buffer, 0); }));
    TEST_ASSERT_TRUE(buffer.isValid());
    TEST_ASSERT_FALSE(src.readLatestFrame(buffer, 0));  // consumed: nothing new
    TEST_ASSERT_TRUE(buffer.isValid());                 // and the frame is kept
    return true;
}

bool test_NdiPreview_FileLayerUnchangedWithoutSync() {
    LayerPlayback playback;
    playback.setInputSource(std::make_unique<FakeFileSource>());
    for (int i = 0; i < 5; ++i) {
        playback.update();
    }
    TEST_ASSERT_EQ(playback.getCurrentFrame(), -1);  // a file still waits for MTC
    return true;
}

bool test_NdiPreview_AutoUnloadSkipsUnknownLength() {
    auto src = std::make_unique<FakeLiveSource>(1920, 1080);
    FakeLiveSource* raw = src.get();
    src->open("fake");
    auto layer = std::make_unique<VideoLayer>();
    layer->setInputSource(std::move(src));
    layer->properties().autoUnload = true;  // the engine sets it on every armed layer

    LayerManager manager;
    TEST_ASSERT_TRUE(manager.addLayerWithId("ndi-preview", std::move(layer)));
    // One frame at a time: the live pull takes the newest frame and drops
    // older unread ones, so frames released together count as one.
    for (int64_t n = 1; n <= 3; ++n) {
        raw->release(1);
        TEST_ASSERT_TRUE(waitFor([&] {
            manager.updateAll();
            VideoLayer* l = manager.getLayerByCueId("ndi-preview");
            return l == nullptr || l->getCurrentFrame() >= n;
        }));
        // totalFrames == 0 and currentFrame > 0: before the guard, gone here
        TEST_ASSERT_TRUE(manager.getLayerByCueId("ndi-preview") != nullptr);
    }
    return true;
}

// --- fit_output ----------------------------------------------------------

bool test_NdiPreview_FitOutputTest3Regions() {
    const FitRegion a1 = region("HDMI-A-1", 0, 0, 3840, 2160);
    const FitRegion a3 = region("HDMI-A-3", 3840, 0, 3840, 2160);
    const FitRegion a2 = region("HDMI-A-2", 7680, 0, 1920, 1080);

    FitPlacement p = computeOutputFit(a1, kCanvasW, kCanvasH, 1920, 1080, FitMode::Fill);
    TEST_ASSERT_EQ(p.x, -2880);
    TEST_ASSERT_EQ(p.y, 0);
    TEST_ASSERT_TRUE(near(p.scale, 1.0));  // a 1080p source fills the 4K output
    p = computeOutputFit(a1, kCanvasW, kCanvasH, 1920, 1080, FitMode::Native);
    TEST_ASSERT_TRUE(near(p.scale, 0.5));  // a quarter of it at native size

    p = computeOutputFit(a3, kCanvasW, kCanvasH, 1920, 1080, FitMode::Fill);
    TEST_ASSERT_EQ(p.x, 960);
    TEST_ASSERT_EQ(p.y, 0);
    TEST_ASSERT_TRUE(near(p.scale, 1.0));

    p = computeOutputFit(a2, kCanvasW, kCanvasH, 1920, 1080, FitMode::Fill);
    TEST_ASSERT_EQ(p.x, 3840);
    TEST_ASSERT_EQ(p.y, 540);
    TEST_ASSERT_TRUE(near(p.scale, 0.5));
    p = computeOutputFit(a2, kCanvasW, kCanvasH, 1920, 1080, FitMode::Native);
    TEST_ASSERT_TRUE(near(p.scale, 0.5));
    return true;
}

bool test_NdiPreview_FitOutputNativeMatchesEngine() {
    // On a canvas at least as wide as the media (every rig), native mode is
    // the engine's cue formula.
    const FitRegion regions[] = {
        region("HDMI-A-1", 0, 0, 3840, 2160),
        region("HDMI-A-2", 7680, 0, 1920, 1080),
        region("HDMI-A-1", 0, 0, 1366, 768),
    };
    const int media[][2] = {{1920, 1080}, {3840, 2160}, {1280, 720}, {1080, 1920}, {4096, 1716}};
    for (const auto& r : regions) {
        const int canvasW = (r.width == 1366) ? 1366 : kCanvasW;
        const int canvasH = (r.width == 1366) ? 768 : kCanvasH;
        for (const auto& m : media) {
            if (static_cast<double>(canvasW) / canvasH < static_cast<double>(m[0]) / m[1]) {
                // Media wider than the canvas: the renderer letterboxes it
                // against the canvas WIDTH, which the engine's formula does
                // not model (it assumes the layer fills the canvas height).
                continue;
            }
            FitPlacement p = computeOutputFit(r, canvasW, canvasH, m[0], m[1], FitMode::Native);
            if (!near(p.scale, engineScale(r, canvasH, m[0], m[1]))) {
                std::cerr << r.name << " " << m[0] << "x" << m[1] << ": " << p.scale << " vs engine "
                          << engineScale(r, canvasH, m[0], m[1]) << "\n";
                return false;
            }
        }
    }
    return true;
}

bool test_NdiPreview_FitOutputNarrowCanvasAndFallback() {
    // A 4:3 canvas letterboxes 16:9 media top/bottom: fill = whole width.
    FitRegion whole = region("VGA-1", 0, 0, 1024, 768);
    FitPlacement p = computeOutputFit(whole, 1024, 768, 1920, 1080, FitMode::Fill);
    TEST_ASSERT_EQ(p.x, 0);
    TEST_ASSERT_EQ(p.y, 0);
    TEST_ASSERT_TRUE(near(p.scale, 1.0));

    // No dimensions yet: centred on the region at region-height scale.
    p = computeOutputFit(region("HDMI-A-2", 7680, 0, 1920, 1080), kCanvasW, kCanvasH, 0, 0, FitMode::Fill);
    TEST_ASSERT_EQ(p.x, 3840);
    TEST_ASSERT_EQ(p.y, 540);
    TEST_ASSERT_TRUE(near(p.scale, 0.5));
    return true;
}

bool test_NdiPreview_FitOutputRefitsOnFormatChange() {
    VideoLayer layer;
    const FitRegion a1 = region("HDMI-A-1", 0, 0, 3840, 2160);

    // fit_output before the async load delivered an input: fallback.
    layer.setOutputFit("ndi-preview", a1, kCanvasW, kCanvasH, FitMode::Native);
    TEST_ASSERT_TRUE(layer.hasOutputFit());
    TEST_ASSERT_EQ(layer.properties().x, -2880);
    TEST_ASSERT_TRUE(near(layer.properties().scaleX, 1.0f));  // 2160/2160

    // The input arrives with the invented 1920x1080.
    auto src = std::make_unique<FakeLiveSource>(1920, 1080, /*provisional=*/true);
    FakeLiveSource* raw = src.get();
    src->open("fake");
    layer.setInputSource(std::move(src));
    layer.update();
    TEST_ASSERT_TRUE(near(layer.properties().scaleX, 0.5f));

    // The source's first real frame says 1280x720.
    raw->setFormat(1280, 720, false);
    layer.update();
    TEST_ASSERT_TRUE(std::fabs(layer.properties().scaleX - 1.0f / 3.0f) < 1e-5);
    TEST_ASSERT_TRUE(near(layer.properties().scaleX, layer.properties().scaleY));
    TEST_ASSERT_EQ(layer.properties().x, -2880);

    // An explicit scale/position (the router calls clearOutputFit) wins.
    layer.clearOutputFit();
    layer.properties().scaleX = layer.properties().scaleY = 0.25f;
    raw->setFormat(640, 360, false);
    layer.update();
    TEST_ASSERT_TRUE(near(layer.properties().scaleX, 0.25f));
    TEST_ASSERT_FALSE(layer.hasOutputFit());
    return true;
}

// --- journal contract ------------------------------------------------------
// cuems-power-bridge parses these exact strings. A failure here means the
// bridge's parser must change in the same release.

bool test_NdiPreview_JournalContract() {
    namespace j = journal;
    TEST_ASSERT_EQ(j::starting("0.1.2-8"), std::string("cuems-videocomposer 0.1.2-8 starting"));
    TEST_ASSERT_EQ(std::string(j::workerThreadRunning()), std::string("AsyncVideoLoader: Worker thread running"));
    TEST_ASSERT_EQ(j::asyncLoadComplete("ndi://LAPTOP (OBS)", "ndi-preview"),
                   std::string("Async load complete: ndi://LAPTOP (OBS) (cue ID: ndi-preview)"));
    TEST_ASSERT_EQ(j::asyncLoadFailed("ndi://LAPTOP (OBS)", "ndi-preview"),
                   std::string("Async load failed for: ndi://LAPTOP (OBS) (cue ID: ndi-preview)"));
    TEST_ASSERT_EQ(j::layerNoLongerExists("ndi-preview"),
                   std::string("Layer no longer exists for cue ID: ndi-preview"));
    TEST_ASSERT_EQ(j::skippingCancelledLoad("ndi-preview"),
                   std::string("AsyncVideoLoader: Skipping cancelled load for cue: ndi-preview"));
    TEST_ASSERT_EQ(j::discardingCancelledResult("ndi-preview"),
                   std::string("AsyncVideoLoader: Discarding result for cancelled cue: ndi-preview"));
    TEST_ASSERT_EQ(std::string(j::resetAll()),
                   std::string("Reset: removing all layers, cancelling loads, resetting master"));
    TEST_ASSERT_EQ(j::ndiConnected("LAPTOP (OBS)"), std::string("NDI: Connected to source: LAPTOP (OBS)"));
    TEST_ASSERT_EQ(j::ndiSourceNotFound("LAPTOP (OBS)"), std::string("NDI: Source not found: LAPTOP (OBS)"));
    TEST_ASSERT_EQ(j::ndiSourceFormat(1920, 1080, 25.0), std::string("NDI: Source format: 1920x1080 @ 25 fps (BGRA)"));
    TEST_ASSERT_EQ(j::ndiSourceFormat(1920, 1080, 30000.0 / 1001.0),
                   std::string("NDI: Source format: 1920x1080 @ 29.97 fps (BGRA)"));
    TEST_ASSERT_EQ(std::string(j::ndiNoVideoFrame()),
                   std::string("NDI: No video frame received, using defaults (1920x1080 @25fps)"));
    TEST_ASSERT_EQ(j::ndiSourceFormatUpdated(1280, 720, 50.0),
                   std::string("NDI: Source format updated 1280x720 @ 50 fps (was invented)"));
    TEST_ASSERT_EQ(j::ndiSourceFormatChanged(3840, 2160, 25.0),
                   std::string("NDI: Source format changed 3840x2160 @ 25 fps"));
    TEST_ASSERT_EQ(j::ndiDiscoverStarted(3), std::string("NDI discover: started (3 s)"));
    TEST_ASSERT_EQ(j::ndiDiscoverSource("A @ B (OBS)", "169.254.7.9:5961"),
                   std::string("NDI discover: A @ B (OBS) @ 169.254.7.9:5961"));
    TEST_ASSERT_EQ(j::ndiDiscoverSource("LAPTOP (OBS)", ""), std::string("NDI discover: LAPTOP (OBS) @ -"));
    TEST_ASSERT_EQ(j::ndiDiscoverDone(2), std::string("NDI discover: done (2)"));
    TEST_ASSERT_EQ(std::string(j::ndiDiscoverBusy()), std::string("NDI discover: busy"));
    TEST_ASSERT_EQ(j::region("HDMI-A-2", 7680, 0, 1920, 1080), std::string("region HDMI-A-2 7680,0,1920,1080"));
    TEST_ASSERT_EQ(j::fitOutput("ndi-preview", "HDMI-A-2", "fill", 3840, 540, 0.5, 0.5, "dims 1920x1080"),
                   std::string("fit_output: ndi-preview -> HDMI-A-2 fill pos 3840,540 scale 0.5,0.5 (dims 1920x1080)"));
    TEST_ASSERT_EQ(j::fitOutputUnknown("DP-9", {"HDMI-A-1", "HDMI-A-3"}),
                   std::string("fit_output: unknown output 'DP-9' (have: HDMI-A-1, HDMI-A-3)"));
    return true;
}

// --- rev 9: relay support ---------------------------------------------------

bool test_NdiPreview_AddressForm() {
    std::string addr, why;
    TEST_ASSERT_TRUE(parseNdiAddress("@169.254.0.1:40123", addr, why));
    TEST_ASSERT_EQ(addr, std::string("169.254.0.1:40123"));
    TEST_ASSERT_FALSE(parseNdiAddress("@", addr, why));
    TEST_ASSERT_FALSE(parseNdiAddress("@169.254.0.1", addr, why));
    TEST_ASSERT_FALSE(parseNdiAddress("@169.254.0.1:", addr, why));
    TEST_ASSERT_FALSE(parseNdiAddress("@169.254.0.1:0", addr, why));
    TEST_ASSERT_FALSE(parseNdiAddress("@169.254.0.1:70000", addr, why));
    TEST_ASSERT_FALSE(parseNdiAddress("@169.254.0.1:12a", addr, why));
    TEST_ASSERT_FALSE(parseNdiAddress("@node01.local:5961", addr, why));  // IPv4 only
    TEST_ASSERT_FALSE(parseNdiAddress("@[fe80::1]:5961", addr, why));
    TEST_ASSERT_FALSE(parseNdiAddress("LAPTOP (OBS)", addr, why));         // a name
    return true;
}

bool test_NdiPreview_TransportDecision() {
    const std::string dir = "/usr/share/cuems-videocomposer/ndi";
    NdiTransportState t = decideNdiTransport(nullptr, dir, true);
    TEST_ASSERT_TRUE(t.baseTcp);
    TEST_ASSERT_EQ(t.detail, dir);
    t = decideNdiTransport("", dir, true);
    TEST_ASSERT_TRUE(t.baseTcp);
    t = decideNdiTransport(dir.c_str(), dir, true);  // set to ours: still ours
    TEST_ASSERT_TRUE(t.baseTcp);
    t = decideNdiTransport("/var/lib/cuems/.ndi", dir, true);  // operator override wins
    TEST_ASSERT_FALSE(t.baseTcp);
    TEST_ASSERT_EQ(t.detail, std::string("NDI_CONFIG_DIR=/var/lib/cuems/.ndi"));
    t = decideNdiTransport(nullptr, dir, false);  // dev build, nothing installed
    TEST_ASSERT_FALSE(t.baseTcp);
    TEST_ASSERT_EQ(t.detail, "no config at " + dir + "/ndi-config.v1.json");
    return true;
}

bool test_NdiPreview_JournalContractRelay() {
    namespace j = journal;
    TEST_ASSERT_EQ(j::ndiTransportBaseTcp("/usr/share/cuems-videocomposer/ndi"),
                   std::string("NDI receive transport: base TCP (/usr/share/cuems-videocomposer/ndi)"));
    TEST_ASSERT_EQ(j::ndiTransportNotBaseTcp("NDI_CONFIG_DIR=/x"),
                   std::string("NDI receive transport: NOT base TCP (NDI_CONFIG_DIR=/x) - relay unavailable"));
    TEST_ASSERT_EQ(j::ndiBadAddress("@1.2.3.4", "expected @<ipv4>:<port>"),
                   std::string("NDI: bad source address '@1.2.3.4': expected @<ipv4>:<port>"));
    TEST_ASSERT_EQ(j::ndiConnected("@169.254.0.1:40123"),
                   std::string("NDI: Connected to source: @169.254.0.1:40123"));
    TEST_ASSERT_EQ(j::asyncLoadComplete("ndi://@169.254.0.1:40123", "ndi-preview"),
                   std::string("Async load complete: ndi://@169.254.0.1:40123 (cue ID: ndi-preview)"));
    return true;
}

