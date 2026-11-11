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

// The software-decode prime at load (1amfwm7hk6j, plan cuems-RELATIONS
// Plans/2026-11-11-vc-prime-software-decode-at-load.md, AC1): a software
// VideoFileInput loaded through AsyncVideoLoader has frame 0 decoded on the
// loader's worker, so the first follow poll on the render thread takes
// readFrame()'s same-frame early return instead of a seek + pipeline fill.
// Real files: a short prores_ks clip made with the ffmpeg CLI whose frame 0
// is white and every other frame black; skipped without ffmpeg/prores_ks.

#include "TestFramework.h"
#include "../input/AsyncVideoLoader.h"
#include "../input/InputSource.h"
#include "../input/VideoFileInput.h"
#include "../layer/VideoLayer.h"
#include "../sync/SyncSource.h"
#include "../video/FrameBuffer.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace videocomposer;
using namespace videocomposer::test;

namespace {

bool haveProresEncoder() {
    return std::system("ffmpeg -version >/dev/null 2>&1") == 0 &&
           std::system("ffmpeg -hide_banner -encoders 2>/dev/null | grep -q prores_ks") == 0;
}

std::string primeTempDir() {
    char tmpl[] = "/tmp/vc-prime-XXXXXX";
    const char* d = mkdtemp(tmpl);
    return d ? std::string(d) : std::string();
}

// 320x240, 25 fps, 2 s (50 frames), ProRes 422 HQ (10-bit 4:2:2, the codec of
// the rig's fixture, frame-threaded software decode): frame 0 white, frames
// 1.. black.
std::string makeWhiteZeroClip(const std::string& dir) {
    const std::string path = dir + "/white0.mov";
    const std::string cmd =
        "ffmpeg -nostdin -hide_banner -loglevel error -y "
        "-f lavfi -i color=c=black:s=320x240:r=25:d=2 "
        "-vf \"drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable='eq(n,0)'\" "
        "-c:v prores_ks -profile:v 3 " + path;
    return std::system(cmd.c_str()) == 0 ? path : std::string();
}

void removeDir(const std::string& dir) {
    const std::string rm = "rm -rf " + dir;
    (void)std::system(rm.c_str());
}

// Centre pixel of a BGRA buffer: white (frame 0) or black (any later frame).
bool centreIs(const FrameBuffer& buf, bool white) {
    if (!buf.isValid()) return false;
    const FrameInfo& info = buf.info();
    const int x = info.width / 2;
    const int y = info.height / 2;
    const uint8_t* p = buf.data() + (static_cast<size_t>(y) * info.width + x) * 4;
    if (white) return p[0] > 200 && p[1] > 200 && p[2] > 200;
    return p[0] < 50 && p[1] < 50 && p[2] < 50;
}

// WARNING lines reach stderr unless the binary runs under the journal.
bool primeLogsVisible() { return std::getenv("JOURNAL_STREAM") == nullptr; }

class PrimeStderrCapture {
public:
    PrimeStderrCapture() : old_(std::cerr.rdbuf(buf_.rdbuf())) {}
    ~PrimeStderrCapture() { std::cerr.rdbuf(old_); }
    std::string text() const { return buf_.str(); }

private:
    std::ostringstream buf_;
    std::streambuf* old_;
};

int countLines(const std::string& text, const std::string& needle) {
    int n = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.find(needle) != std::string::npos) ++n;
    }
    return n;
}

// One load through the real loader (worker pool, createInputSourceAsync, the
// result queue, the callback on this thread): the OSC /layer/load path.
struct Loaded {
    bool called = false;
    bool success = false;
    std::unique_ptr<InputSource> input;
    VideoFileInput* file() { return dynamic_cast<VideoFileInput*>(input.get()); }
};

Loaded loadThroughLoader(const std::string& path) {
    Loaded out;
    AsyncVideoLoader loader;
    loader.initialize(nullptr, nullptr);
    loader.requestLoad("prime-test-cue_0", path,
                       [&out](const std::string&, const std::string&, std::unique_ptr<InputSource> in, bool ok) {
                           out.called = true;
                           out.success = ok;
                           out.input = std::move(in);
                       });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!out.called && std::chrono::steady_clock::now() < deadline) {
        loader.pollCompleted();
        if (!out.called) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    loader.shutdown();
    return out;
}

class FrameSync : public SyncSource {
public:
    bool connect(const char* = nullptr) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    int64_t pollFrame(uint8_t* rolling) override { if (rolling) *rolling = 1; return frame_; }
    int64_t getCurrentFrame() const override { return frame_; }
    const char* getName() const override { return "FrameSync"; }
    int64_t frame_ = 0;
};

} // namespace

// AC1, part 1: after a load through the loader, the decoded frame kept by the
// input is frame 0 (white), and handing it out again costs no demuxer seek.
// The frame counter alone proves nothing (seek() sets it before any decode):
// the pass is the pixels plus the seek count. Today (no prime) the count is 0
// after the load and the read below seeks.
bool test_Prime_LoaderPrimesFrameZeroPixels() {
    if (!haveProresEncoder()) {
        std::cout << "(skipped: no ffmpeg/prores_ks) ";
        return true;
    }
    const std::string d = primeTempDir();
    TEST_ASSERT_FALSE(d.empty());
    const std::string clip = makeWhiteZeroClip(d);
    TEST_ASSERT_FALSE(clip.empty());

    Loaded l = loadThroughLoader(clip);
    TEST_ASSERT_TRUE(l.called);
    TEST_ASSERT_TRUE(l.success);
    VideoFileInput* in = l.file();
    TEST_ASSERT_TRUE(in != nullptr);
    TEST_ASSERT_TRUE(in->getOptimalBackend() == InputSource::DecodeBackend::CPU_SOFTWARE);
    TEST_ASSERT_TRUE(in->lastPrimeResult() == VideoFileInput::PrimeResult::Primed);
    TEST_ASSERT_EQ(in->demuxSeekCount(), 1);          // the prime's own seek, on the loader thread
    TEST_ASSERT_EQ(in->getCurrentFrame(), 0);

    FrameBuffer buf;
    TEST_ASSERT_TRUE(in->readFrame(0, buf));          // the same-frame early return
    TEST_ASSERT_EQ(in->demuxSeekCount(), 1);
    TEST_ASSERT_TRUE(centreIs(buf, true));            // frame 0's pixels, not an empty buffer

    l.input.reset();
    removeDir(d);
    return true;
}

// AC1, part 2: the first update() after setMtcFollow(true) at content frame 0
// performs no seek; frame 1 then follows sequentially, still without one.
bool test_Prime_FirstFollowUpdateDoesNotSeek() {
    if (!haveProresEncoder()) {
        std::cout << "(skipped: no ffmpeg/prores_ks) ";
        return true;
    }
    const std::string d = primeTempDir();
    TEST_ASSERT_FALSE(d.empty());
    const std::string clip = makeWhiteZeroClip(d);
    TEST_ASSERT_FALSE(clip.empty());

    Loaded l = loadThroughLoader(clip);
    TEST_ASSERT_TRUE(l.success);
    VideoFileInput* in = l.file();
    TEST_ASSERT_TRUE(in != nullptr);
    const int64_t seeksAtLoad = in->demuxSeekCount();
    TEST_ASSERT_EQ(seeksAtLoad, 1);

    auto layer = std::make_unique<VideoLayer>();
    layer->setInputSource(std::move(l.input));
    auto sync = std::make_unique<FrameSync>();
    FrameSync* s = sync.get();
    layer->setSyncSource(std::move(sync));
    layer->setCueId("prime-test-cue_0");
    s->frame_ = 0;
    layer->setMtcFollow(true);

    layer->update();                                  // the first follow poll
    TEST_ASSERT_EQ(in->demuxSeekCount(), seeksAtLoad);
    TEST_ASSERT_EQ(in->getCurrentFrame(), 0);

    s->frame_ = 1;
    layer->update();                                  // sequential: decode on, no seek
    TEST_ASSERT_EQ(in->demuxSeekCount(), seeksAtLoad);
    TEST_ASSERT_EQ(in->getCurrentFrame(), 1);
    FrameBuffer buf;
    TEST_ASSERT_TRUE(in->readFrame(1, buf));          // same frame again: frame 1 is black
    TEST_ASSERT_TRUE(centreIs(buf, false));

    layer.reset();
    removeDir(d);
    return true;
}

// AC1, part 3: a prime whose decode fails after its seek (test seam: the
// decode loop runs and consumes packets, then reports failure, as a real
// failure would) logs one WARNING naming the file, leaves the input with no
// current frame (seek() had already set it to 0) and the load completes. The
// next readFrame(0) then seeks again and returns frame 0's pixels; without the
// reset it would take no seek and continue from where the failed decode left
// the stream.
bool test_Prime_FailedPrimeResetsAndNextReadIsFrameZero() {
    if (!haveProresEncoder()) {
        std::cout << "(skipped: no ffmpeg/prores_ks) ";
        return true;
    }
    const std::string d = primeTempDir();
    TEST_ASSERT_FALSE(d.empty());
    const std::string clip = makeWhiteZeroClip(d);
    TEST_ASSERT_FALSE(clip.empty());

    VideoFileInput::failNextPrimeForTesting();
    Loaded l;
    std::string log;
    {
        PrimeStderrCapture capture;
        l = loadThroughLoader(clip);
        log = capture.text();
    }
    TEST_ASSERT_TRUE(l.called);
    TEST_ASSERT_TRUE(l.success);                      // the load still completes
    VideoFileInput* in = l.file();
    TEST_ASSERT_TRUE(in != nullptr);
    TEST_ASSERT_TRUE(in->lastPrimeResult() == VideoFileInput::PrimeResult::Failed);
    TEST_ASSERT_EQ(in->getCurrentFrame(), -1);
    TEST_ASSERT_EQ(in->demuxSeekCount(), 1);
    if (primeLogsVisible()) {
        TEST_ASSERT_EQ(countLines(log, "priming frame 0 failed"), 1);
        TEST_ASSERT_TRUE(log.find("priming frame 0 failed for " + clip) != std::string::npos);
    }

    FrameBuffer buf;
    TEST_ASSERT_TRUE(in->readFrame(0, buf));
    TEST_ASSERT_EQ(in->demuxSeekCount(), 2);          // the reset made it seek again
    TEST_ASSERT_EQ(in->getCurrentFrame(), 0);
    TEST_ASSERT_TRUE(centreIs(buf, true));            // frame 0, not where the failed decode stopped

    l.input.reset();
    removeDir(d);
    return true;
}

// The prime leaves alone what it is not for: an index-only open (the
// cuems-videoindexer) decodes nothing.
bool test_Prime_IndexOnlyIsNotPrimed() {
    if (!haveProresEncoder()) {
        std::cout << "(skipped: no ffmpeg/prores_ks) ";
        return true;
    }
    const std::string d = primeTempDir();
    TEST_ASSERT_FALSE(d.empty());
    const std::string clip = makeWhiteZeroClip(d);
    TEST_ASSERT_FALSE(clip.empty());

    {
        VideoFileInput in;
        in.setIndexOnly(true);
        TEST_ASSERT_TRUE(in.open(clip));
        TEST_ASSERT_TRUE(in.primeSoftwareDecode() == VideoFileInput::PrimeResult::NotApplicable);
        TEST_ASSERT_EQ(in.demuxSeekCount(), 0);
        TEST_ASSERT_EQ(in.getCurrentFrame(), -1);
        in.close();
    }
    removeDir(d);
    return true;
}
