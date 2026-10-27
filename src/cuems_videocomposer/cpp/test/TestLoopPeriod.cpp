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

// The shared loop period (plan cuems-RELATIONS
// Plans/2026-10-19-audio-loop-varispeed-869ejxng3-1amfwm7e6cw.md, section
// 4.4, Phase B step 9): the band arithmetic (layer/LoopPeriod.h), a VideoLayer
// wrapping on the engine's period, the files' own frame counts, the caps
// file, and the undecodable-timecode hold (D13) driven through the real
// mtcreceiver decoder. GL-free; the file tests need the ffmpeg CLI and are
// skipped without it.

#include "TestFramework.h"
#include "../layer/LoopPeriod.h"
#include "../layer/VideoLayer.h"
#include "../input/InputSource.h"
#include "../input/VideoFileInput.h"
#include "../input/HAPVideoInput.h"
#include "../sync/SyncSource.h"
#include "../sync/MtcInvalidHold.h"
#include "../sync/QfSeqlock.h"
#include "../utils/CapsFile.h"
#include "../video/FrameBuffer.h"
#if defined(HAVE_MTCRECEIVER) && defined(MTCRECV_TESTING)
#include "../../mtcreceiver/mtcreceiver.h"
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using namespace videocomposer;
using namespace videocomposer::test;
namespace lp = videocomposer::loopperiod;

namespace {

// P = 96480 / 48000 s = 2.010 s (the step-7 ramp), 25 fps: Pf = 50.25 frames
const lp::Period P2010{96480, 48000, 50};
const lp::Rate R25{25, 1};

// Reference: the same rule in long double, for positions where it is exact.
int64_t refShown(int64_t F, double pf, int64_t R, int passes, bool stretch) {
    if (F < 0) return 0;
    const long double k = std::floor((long double)F / pf);
    if (passes > 0 && k >= passes) return R - 1;
    const long double f = (long double)F - k * pf;
    const long double s = stretch ? std::floor(f * R / pf) : std::floor(f);
    return std::min<int64_t>((int64_t)s, R - 1);
}

class PeriodInput : public InputSource {
public:
    bool open(const std::string&) override { return true; }
    void close() override {}
    bool seek(int64_t) override { return true; }
    bool readFrame(int64_t frameNumber, FrameBuffer&) override { last_ = frameNumber; return true; }
    FrameInfo getFrameInfo() const override {
        FrameInfo info;
        info.width = 1920;
        info.height = 1080;
        info.framerate = 25.0;
        info.totalFrames = totalFrames_;
        return info;
    }
    bool isReady() const override { return true; }
    int64_t getCurrentFrame() const override { return last_; }
    CodecType detectCodec() const override { return CodecType::SOFTWARE; }
    bool supportsDirectGPUTexture() const override { return false; }
    DecodeBackend getOptimalBackend() const override { return DecodeBackend::CPU_SOFTWARE; }
    bool getFrameRateRational(int64_t& num, int64_t& den) const override { num = 25; den = 1; return true; }
    int64_t realFrames(const char** rule) const override { if (rule) *rule = "stream"; return own_; }
    int64_t last_ = -1;
    int64_t totalFrames_ = 51;   // the container estimate: the longest stream
    int64_t own_ = 50;
};

class PeriodSync : public SyncSource {
public:
    bool connect(const char* = nullptr) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    int64_t pollFrame(uint8_t* rolling) override { if (rolling) *rolling = 1; return frame_; }
    int64_t getCurrentFrame() const override { return frame_; }
    const char* getName() const override { return "PeriodSync"; }
    int64_t frame_ = 0;
};

struct Rig {
    std::unique_ptr<VideoLayer> layer;
    PeriodInput* input = nullptr;
    PeriodSync* sync = nullptr;
    int64_t show(int64_t F) {
        sync->frame_ = F;
        layer->update();
        return input->last_;
    }
};

Rig makeRig(int64_t own = 50, int64_t total = 51) {
    Rig r;
    r.layer = std::make_unique<VideoLayer>();
    auto in = std::make_unique<PeriodInput>();
    in->own_ = own;
    in->totalFrames_ = total;
    r.input = in.get();
    r.layer->setInputSource(std::move(in));
    auto s = std::make_unique<PeriodSync>();
    r.sync = s.get();
    r.layer->setSyncSource(std::move(s));
    r.layer->setCueId("loop-period-test-cue_0");
    r.layer->setMtcFollow(true);
    return r;
}

bool haveFfmpeg() {
    return std::system("ffmpeg -version >/dev/null 2>&1 && ffprobe -version >/dev/null 2>&1") == 0;
}

std::string tempDir() {
    char tmpl[] = "/tmp/vc-loopperiod-XXXXXX";
    const char* d = mkdtemp(tmpl);
    return d ? std::string(d) : std::string();
}

bool ffmpeg(const std::string& args) {
    const std::string cmd = "ffmpeg -nostdin -hide_banner -loglevel error -y " + args;
    return std::system(cmd.c_str()) == 0;
}

} // namespace

// Band 1: |Pf - realFrames| <= 2. A period longer than the video holds its
// last frame until the wrap; a video longer than the period is cut there.
bool test_LoopPeriod_Band1HoldAndCut() {
    TEST_ASSERT_TRUE(lp::band(P2010, R25, 50) == lp::Band::Hold);
    TEST_ASSERT_EQ(lp::shownFrame(49, P2010, R25, 50, -1).frame, 49);
    TEST_ASSERT_EQ(lp::shownFrame(50, P2010, R25, 50, -1).frame, 49);   // held: Pf = 50.25
    TEST_ASSERT_EQ(lp::shownFrame(50, P2010, R25, 50, -1).pass, 0);
    TEST_ASSERT_EQ(lp::shownFrame(51, P2010, R25, 50, -1).frame, 0);    // pass 1 from 50.25
    TEST_ASSERT_EQ(lp::shownFrame(51, P2010, R25, 50, -1).pass, 1);
    // video 52 frames, period 50.25: cut at the wrap
    TEST_ASSERT_TRUE(lp::band(P2010, R25, 52) == lp::Band::Hold);
    TEST_ASSERT_EQ(lp::shownFrame(50, P2010, R25, 52, -1).frame, 50);
    TEST_ASSERT_EQ(lp::shownFrame(51, P2010, R25, 52, -1).frame, 0);
    // three frames off is no longer band 1
    TEST_ASSERT_TRUE(lp::band(P2010, R25, 53) != lp::Band::Hold);
    for (int64_t F = 0; F < 5000; ++F) {
        TEST_ASSERT_EQ(lp::shownFrame(F, P2010, R25, 50, -1).frame, refShown(F, 50.25, 50, -1, false));
    }
    return true;
}

// Band 2: > 2 frames and <= 1 %: the pass is stretched over Pf, never past
// the last frame, frame 0 at every wrap (sintel: 106 ms over 888 s).
bool test_LoopPeriod_Band2Stretch() {
    const int64_t R = 22200;                        // 888 s at 25 fps
    const lp::Period p{(R * 40 + 106) * 48, 48000, R};   // P = 888.106 s
    TEST_ASSERT_TRUE(lp::band(p, R25, R) == lp::Band::Stretch);
    const double pf = lp::pfFrames(p, R25);
    int64_t prev = -1;
    int repeats = 0;
    for (int64_t F = 0; F < (int64_t)(2 * pf) + 2; ++F) {
        const lp::Shown s = lp::shownFrame(F, p, R25, R, -1);
        TEST_ASSERT_TRUE(s.frame >= 0 && s.frame <= R - 1);
        // the long double reference can round across an exact integer
        TEST_ASSERT_TRUE(std::llabs(s.frame - refShown(F, pf, R, -1, true)) <= 1);
        if (s.pass == 0 && s.frame == prev) ++repeats;
        if (s.pass == 0) prev = s.frame;
    }
    TEST_ASSERT_EQ(lp::shownFrame((int64_t)std::ceil(pf), p, R25, R, -1).frame, 0);
    TEST_ASSERT_TRUE(repeats >= 1 && repeats <= 3);   // ~2.65 frames spread over the pass
    return true;
}

// A finite loop ends: passes 3 -> from 3 Pf on, the last frame, ended.
bool test_LoopPeriod_FinitePassesEnd() {
    const lp::Shown before = lp::shownFrame(150, P2010, R25, 50, 3);   // 150 < 150.75
    TEST_ASSERT_FALSE(before.ended);
    TEST_ASSERT_EQ(before.pass, 2);
    TEST_ASSERT_EQ(before.frame, 49);
    const lp::Shown after = lp::shownFrame(151, P2010, R25, 50, 3);
    TEST_ASSERT_TRUE(after.ended);
    TEST_ASSERT_EQ(after.frame, 49);
    TEST_ASSERT_TRUE(lp::shownFrame(100000, P2010, R25, 50, 3).ended);
    TEST_ASSERT_FALSE(lp::shownFrame(100000, P2010, R25, 50, -1).ended);
    TEST_ASSERT_FALSE(lp::shownFrame(100000, P2010, R25, 50, 0).ended);   // 0 = forever
    return true;
}

// Fractional pass starts, and k x Pf integer exactly (K14): 4 x 50.25 = 201
// is frame 0 of pass 4, not frame 50 of pass 3.
bool test_LoopPeriod_FractionalPassStart() {
    TEST_ASSERT_EQ(lp::shownFrame(100, P2010, R25, 50, -1).pass, 1);   // pass 2 starts at 100.5
    TEST_ASSERT_EQ(lp::shownFrame(100, P2010, R25, 50, -1).frame, 49);
    TEST_ASSERT_EQ(lp::shownFrame(101, P2010, R25, 50, -1).pass, 2);
    TEST_ASSERT_EQ(lp::shownFrame(101, P2010, R25, 50, -1).frame, 0);
    TEST_ASSERT_EQ(lp::shownFrame(201, P2010, R25, 50, -1).pass, 4);
    TEST_ASSERT_EQ(lp::shownFrame(201, P2010, R25, 50, -1).frame, 0);
    // 29.97 fps, P = 1001 / 300 s -> Pf = 100 exactly; k x Pf integer everywhere
    const lp::Rate ntsc{30000, 1001};
    const lp::Period p{1001, 300, 100};
    for (int64_t k = 0; k < 1000; ++k) {
        TEST_ASSERT_EQ(lp::shownFrame(k * 100, p, ntsc, 100, -1).frame, 0);
        TEST_ASSERT_EQ(lp::shownFrame(k * 100, p, ntsc, 100, -1).pass, k);
        TEST_ASSERT_EQ(lp::shownFrame(k * 100 + 99, p, ntsc, 100, -1).frame, 99);
    }
    return true;
}

// Before the start the layer holds frame 0 (G21), never a wrapped tail.
bool test_LoopPeriod_NegativeRegionHoldsFrameZero() {
    TEST_ASSERT_EQ(lp::shownFrame(-1, P2010, R25, 50, -1).frame, 0);
    TEST_ASSERT_EQ(lp::shownFrame(-1, P2010, R25, 50, -1).pass, -1);
    TEST_ASSERT_EQ(lp::shownFrame(-1000000, P2010, R25, 50, 3).frame, 0);
    TEST_ASSERT_FALSE(lp::shownFrame(-1000000, P2010, R25, 50, 3).ended);
    return true;
}

// A period beyond the stretch bound (> 2 frames and > 1 %) is applied as
// band 1: it wraps on Pf and holds at the cut, never stretches (H9, I4).
bool test_LoopPeriod_BeyondBoundHoldsNeverStretches() {
    const lp::Period p{60 * 48000 / 25, 48000, 50};   // Pf = 60, video 50: +20 %
    TEST_ASSERT_TRUE(lp::band(p, R25, 50) == lp::Band::Beyond);
    TEST_ASSERT_EQ(lp::shownFrame(30, p, R25, 50, -1).frame, 30);   // 1:1, not 25
    TEST_ASSERT_EQ(lp::shownFrame(55, p, R25, 50, -1).frame, 49);   // held at the cut
    TEST_ASSERT_EQ(lp::shownFrame(60, p, R25, 50, -1).frame, 0);    // wraps on Pf
    // and shorter by > 1 %: cut
    const lp::Period q{40 * 48000 / 25, 48000, 50};
    TEST_ASSERT_TRUE(lp::band(q, R25, 50) == lp::Band::Beyond);
    TEST_ASSERT_EQ(lp::shownFrame(39, q, R25, 50, -1).frame, 39);
    TEST_ASSERT_EQ(lp::shownFrame(40, q, R25, 50, -1).frame, 0);
    // a short loop never stretches: 3 frames off on 50 is beyond, not band 2
    const lp::Period r{53 * 48000 / 25, 48000, 50};
    TEST_ASSERT_TRUE(lp::band(r, R25, 50) == lp::Band::Beyond);
    return true;
}

// No overflow at 1001-denominator rates past many hours (L12d): band 2 on a
// 3 h file at 30000/1001, positions 20 h in, against a long double reference
// at positions where it is exact.
bool test_LoopPeriod_Int128NoOverflow() {
    const lp::Rate ntsc{30000, 1001};
    const int64_t R = 323676;                           // 3 h at 29.97
    // P a little longer than R frames: (R + 5) x 1001 / 30000 s, written over 48 kHz
    const lp::Period p{(R + 5) * 1001 * 48000 / 30000 + 7, 48000, R};
    TEST_ASSERT_TRUE(lp::band(p, ntsc, R) == lp::Band::Stretch);
    const double pf = lp::pfFrames(p, ntsc);
    for (int64_t F = 2000000; F < 2000000 + 20000; F += 97) {
        const lp::Shown s = lp::shownFrame(F, p, ntsc, R, -1);
        TEST_ASSERT_TRUE(s.frame >= 0 && s.frame <= R - 1);
        const int64_t ref = refShown(F, pf, R, -1, true);
        TEST_ASSERT_TRUE(std::llabs(s.frame - ref) <= 1);   // the double reference rounds
    }
    // int64 would overflow: F b x R ~ 2e6 x 48000 x 1001 x 3.2e5 > 9.2e18
    TEST_ASSERT_TRUE((long double)2000000 * 48000 * 1001 * R > 9.3e18L);
    return true;
}

// The layer wraps on the engine's period with the ENGINE's frame count, and
// keeps its own only to compare (I10); the period survives the async load
// completing after it (it arrives right after /load), passes are counted.
bool test_LoopPeriod_LayerUsesEngineFrames() {
    Rig r = makeRig(53, 60);              // own count 53 (> 2 off): ERROR once, engine rules
    r.layer->setWraparound(true);
    r.layer->setLoopPasses(-1);
    r.layer->setLoopPeriod(96480, 48000, 50);
    TEST_ASSERT_EQ(r.show(10), 10);
    TEST_ASSERT_EQ(r.show(50), 49);       // engine count 50: held, not 50 (own) nor 50 % 60
    TEST_ASSERT_EQ(r.show(51), 0);        // wrapped on Pf = 50.25
    TEST_ASSERT_EQ(r.show(20), 20);
    r.layer->setTimeOffset(-100);
    TEST_ASSERT_EQ(r.show(30), 0);        // position -70, before the start: frame 0, not a tail
    r.layer->setTimeOffset(0);
    // a new input source (the async load completing) keeps the period
    auto in = std::make_unique<PeriodInput>();
    PeriodInput* p = in.get();
    r.layer->setInputSource(std::move(in));
    r.input = p;
    r.layer->setMtcFollow(true);
    TEST_ASSERT_EQ(r.show(102), 1);       // pass 2: 102 - 100.5 = 1.5
    // the per-pass re-send is idempotent
    r.layer->setLoopPeriod(96480, 48000, 50);
    TEST_ASSERT_EQ(r.show(103), 2);
    return true;
}

// Two ints: the layer's own frame count (J12); a finite loop holds its last
// frame after the last pass.
bool test_LoopPeriod_LayerTwoIntsUsesOwnCount() {
    Rig r = makeRig(50, 62);
    r.layer->setWraparound(true);
    r.layer->setLoopPasses(2);
    r.layer->setLoopPeriod(96480, 48000, 0);
    TEST_ASSERT_EQ(r.show(50), 49);
    TEST_ASSERT_EQ(r.show(60), 9);
    TEST_ASSERT_EQ(r.show(101), 49);      // 2 passes done at 100.5: last frame held
    TEST_ASSERT_EQ(r.show(5000), 49);
    return true;
}

// No period: the totalFrames wrap is unchanged; clearing (num = den = 0,
// /loop 0, a new load) goes back to it.
bool test_LoopPeriod_LayerNoPeriodWrapsOnTotalFrames() {
    Rig r = makeRig(50, 62);
    r.layer->setWraparound(true);
    TEST_ASSERT_EQ(r.show(61), 61);
    TEST_ASSERT_EQ(r.show(63), 1);        // 63 % 62
    r.layer->setLoopPeriod(96480, 48000, 50);
    TEST_ASSERT_EQ(r.show(61), 10);       // 61 - 50.25
    r.layer->setLoopPeriod(0, 0, 0);
    TEST_ASSERT_EQ(r.show(64), 2);
    r.layer->setLoopPeriod(96480, 48000, 50);
    r.layer->clearLoopPeriod();
    TEST_ASSERT_EQ(r.show(65), 3);
    // a period without wraparound does nothing (the router clears it on /loop 0)
    r.layer->setLoopPeriod(96480, 48000, 50);
    r.layer->setWraparound(false);
    TEST_ASSERT_EQ(r.show(70), 61);       // non-loop: hold the last frame
    return true;
}

// The files' own counts (log and compare only): the stream's duration on
// r_frame_rate, not the container's (the longest stream, H3/H5); a
// stream-copy trim's edit list applied; HAP; all-intra; an mkv without a
// stream duration falls back to the index count.
bool test_LoopPeriod_FileInputRealFrames() {
    if (!haveFfmpeg()) {
        std::cout << "(skipped: no ffmpeg/ffprobe) ";
        return true;
    }
    const std::string d = tempDir();
    TEST_ASSERT_FALSE(d.empty());
    const std::string src = "-f lavfi -i testsrc2=size=320x240:rate=25:duration=4 ";
    const std::string aud = "-f lavfi -i sine=frequency=440:sample_rate=48000:duration=4.5 ";
    // A/V mp4: video 4.000 s (100 frames), audio 4.5 s -> container 4.5 s
    TEST_ASSERT_TRUE(ffmpeg(src + aud + "-map 0:v -map 1:a -c:v libx264 -g 25 -pix_fmt yuv420p -c:a aac -t 4.5 " + d + "/av.mp4") ||
                     ffmpeg(src + aud + "-map 0:v -map 1:a -c:v mpeg4 -g 25 -c:a aac " + d + "/av.mp4"));
    // all-intra
    TEST_ASSERT_TRUE(ffmpeg(src + "-c:v mjpeg -q:v 5 " + d + "/intra.mov"));
    // stream-copy trim from 1 s: 3 s = 75 frames, edit list on the copied GOP
    TEST_ASSERT_TRUE(ffmpeg("-ss 1 -i " + d + "/av.mp4 -map 0:v -c copy -t 3 " + d + "/trim.mp4"));
    // mkv: no stream duration (ffmpeg writes a DURATION tag, not the field)
    TEST_ASSERT_TRUE(ffmpeg(src + "-c:v mjpeg -q:v 5 " + d + "/nodur.mkv"));
    const bool haveHap = ffmpeg(src + "-c:v hap " + d + "/hap.mov");

    auto openFile = [](const std::string& path, bool index, int64_t& frames, std::string& rule,
                       int64_t& total, int64_t& rn, int64_t& rd) {
        VideoFileInput in;
        in.setNoIndex(!index);
        in.setHardwareDecodePreference(VideoFileInput::HardwareDecodePreference::SOFTWARE_ONLY);
        if (!in.open(path)) return false;
        const char* r = "none";
        frames = in.realFrames(&r);
        rule = r;
        total = in.getFrameInfo().totalFrames;
        if (!in.getFrameRateRational(rn, rd)) return false;
        in.close();
        return true;
    };
    int64_t f = 0, total = 0, rn = 0, rd = 0;
    std::string rule;
    TEST_ASSERT_TRUE(openFile(d + "/av.mp4", false, f, rule, total, rn, rd));
    TEST_ASSERT_EQ(rule, std::string("stream"));
    TEST_ASSERT_EQ(f, 100);
    TEST_ASSERT_TRUE(total > 100);                 // the container estimate: audio's 4.5 s
    TEST_ASSERT_EQ(rn, 25);
    TEST_ASSERT_EQ(rd, 1);
    TEST_ASSERT_TRUE(openFile(d + "/intra.mov", false, f, rule, total, rn, rd));
    TEST_ASSERT_EQ(rule, std::string("stream"));
    TEST_ASSERT_EQ(f, 100);
    TEST_ASSERT_TRUE(openFile(d + "/trim.mp4", false, f, rule, total, rn, rd));
    TEST_ASSERT_EQ(rule, std::string("stream"));
    TEST_ASSERT_TRUE(std::llabs(f - 75) <= 1);     // the edit list applied (not the copied GOP)
    TEST_ASSERT_TRUE(openFile(d + "/nodur.mkv", true, f, rule, total, rn, rd));
    TEST_ASSERT_EQ(rule, std::string("index"));
    TEST_ASSERT_TRUE(std::llabs(f - 100) <= 1);
    if (haveHap) {
        HAPVideoInput hap;
        TEST_ASSERT_TRUE(hap.open(d + "/hap.mov"));
        const char* r = "none";
        TEST_ASSERT_EQ(hap.realFrames(&r), 100);
        TEST_ASSERT_EQ(std::string(r), std::string("stream"));
        hap.close();
    } else {
        std::cout << "(HAP part skipped: no hap encoder) ";
    }
    const std::string rm = "rm -rf " + d;
    (void)std::system(rm.c_str());
    return true;
}

// The caps file: pid first, one token per line, written atomically; removed
// only while it is still ours.
bool test_CapsFile_WrittenAndUnlinked() {
    const std::string d = tempDir();
    TEST_ASSERT_FALSE(d.empty());
    const std::string path = d + "/videocomposer.caps";
    std::string err;
    const long pid = static_cast<long>(getpid());
    TEST_ASSERT_TRUE(capsfile::write(path, pid, capsfile::tokens(), err));
    std::ifstream f(path);
    std::string l1, l2, l3;
    std::getline(f, l1);
    std::getline(f, l2);
    TEST_ASSERT_EQ(l1, "pid=" + std::to_string(pid));
    TEST_ASSERT_EQ(l2, std::string("loop_period"));
    TEST_ASSERT_FALSE(static_cast<bool>(std::getline(f, l3)));
    TEST_ASSERT_EQ(capsfile::readPid(path), pid);
    TEST_ASSERT_FALSE(capsfile::removeIfOurs(path, pid + 1));   // a newer instance's file stays
    TEST_ASSERT_EQ(access(path.c_str(), F_OK), 0);
    TEST_ASSERT_TRUE(capsfile::removeIfOurs(path, pid));
    TEST_ASSERT_NE(access(path.c_str(), F_OK), 0);
    TEST_ASSERT_FALSE(capsfile::write(d + "/missing-dir/videocomposer.caps", pid, capsfile::tokens(), err));
    TEST_ASSERT_FALSE(err.empty());
    rmdir(d.c_str());
    return true;
}

// --- D13: the hold on undecodable timecode ---------------------------------

namespace {

constexpr int64_t MS = 1000000;

TickSample tick(int64_t monoNs, long head, bool anchored, uint64_t seq) {
    TickSample t;
    t.monoNs = monoNs;
    t.headMs = head;
    t.anchored = anchored;
    t.seq = seq;
    return t;
}

} // namespace

// A frozen head (dropout, STOP) never holds: today's path.
bool test_MtcHold_FrozenHeadNeverHolds() {
    MtcInvalidHold h;
    h.start(0, 1000);
    TickSample none;
    for (int64_t t = 0; t < 5000 * MS; t += 16 * MS) {
        const auto e = h.update(t, 1000, none);
        TEST_ASSERT_FALSE(e.entered);
    }
    TEST_ASSERT_FALSE(h.held());
    return true;
}

// The pure rule: a moving head without anchors holds 800 ms after it began
// moving; an anchored tick leaves; the hold survives a freeze; a moving head
// with anchors never holds.
bool test_MtcHold_RuleEdges() {
    MtcInvalidHold h;
    h.start(0, 0);
    TickSample t;
    uint64_t seq = 0;
    long head = 0;
    int64_t enteredAt = -1;
    for (int64_t now = 0; now < 2000 * MS; now += 10 * MS) {
        head += 10;                        // moving, no anchored tick
        t = tick(now, head, false, ++seq);
        const auto e = h.update(now, head, t);
        if (e.entered) {
            enteredAt = now;
            TEST_ASSERT_TRUE(e.invalidForMs >= 800);
        }
    }
    TEST_ASSERT_TRUE(enteredAt >= 800 * MS && enteredAt <= 820 * MS);
    TEST_ASSERT_TRUE(h.held());
    // the head freezes: still held
    for (int64_t now = 2000 * MS; now < 3000 * MS; now += 10 * MS) {
        TEST_ASSERT_FALSE(h.update(now, head, t).left);
    }
    TEST_ASSERT_TRUE(h.held());
    // an anchored tick while frozen does not leave; while moving it does
    t = tick(3000 * MS, head, true, ++seq);
    TEST_ASSERT_FALSE(h.update(3000 * MS, head, t).left);
    head += 10;
    t = tick(3010 * MS, head, true, ++seq);
    const auto e = h.update(3010 * MS, head, t);
    TEST_ASSERT_TRUE(e.left);
    TEST_ASSERT_FALSE(h.held());
    // anchored ticks every 80 ms on a moving head: never held again
    for (int64_t now = 3020 * MS; now < 10000 * MS; now += 10 * MS) {
        head += 10;
        const bool anchored = (now / (10 * MS)) % 8 == 0;
        if (anchored) t = tick(now, head, true, ++seq);
        TEST_ASSERT_FALSE(h.update(now, head, t).entered);
    }
    return true;
}

#if defined(HAVE_MTCRECEIVER) && defined(MTCRECV_TESTING)

namespace {

using Msg = std::vector<unsigned char>;
int64_t g_simNowNs = 0;

void timeVector(int64_t mtcMs, int v[4]) {
    const int64_t frames = mtcMs / 40;
    v[0] = (int)(frames % 25);
    v[1] = (int)((frames / 25) % 60);
    v[2] = (int)((frames / 1500) % 60);
    v[3] = (int)((frames / 90000) % 24);
}

// libmtcmaster's quarter frame `type` of the group starting at `groupMs`
Msg quarterFrame(int type, int64_t groupMs) {
    int v[4];
    timeVector(groupMs, v);
    unsigned char c;
    if ((type & 1) == 0) c = (unsigned char)((type << 4) | (v[type / 2] & 0x0f));
    else if (type < 7) c = (unsigned char)((type << 4) | ((v[type / 2] & 0xf0) >> 4));
    else c = (unsigned char)(0x70 | (((0x20 | v[3]) & 0xf0) >> 4));
    return {0xF1, c};
}

Msg fullFrame(int64_t mtcMs) {
    int v[4];
    timeVector(mtcMs, v);
    return {0xF0, 0x7F, 0x7F, 0x01, 0x01, (unsigned char)(0x20 | (v[3] & 0x1f)), (unsigned char)v[2],
            (unsigned char)v[1], (unsigned char)v[0], 0xF7};
}

struct Delivery { int64_t at; Msg msg; };

// MTC from `mtc0` for `durNs`, a full frame every 2 s, every message
// delivered twice in [dblFrom, dblTo) (a doubled rtpmidid subscription).
std::vector<Delivery> stream(int64_t mtc0, int64_t durNs, int64_t dblFrom, int64_t dblTo) {
    std::vector<Delivery> out;
    for (int64_t n = 0;; ++n) {
        const int64_t emit = 5 * MS + n * 10 * MS;
        if (emit > durNs) break;
        const int type = (int)(n % 8);
        const int64_t truth = mtc0 + n * 10;
        out.push_back({emit + MS, quarterFrame(type, truth - type * 10)});
        if (emit >= dblFrom && emit < dblTo) out.push_back({emit + MS + 100000, out.back().msg});
        if (type == 7 && (n + 1) % 200 == 0) out.push_back({emit + MS + 200000, fullFrame(truth + 10)});
    }
    return out;
}

struct HoldRun {
    std::vector<int64_t> entered, left;
};

// The real decoder + the seqlock + the rule, polled like the render loop.
HoldRun runHold(const std::vector<Delivery>& d, int64_t durNs) {
    HoldRun r;
    MtcReceiver::resetStaticStateForTesting();
    std::unique_ptr<MtcReceiver> rx(new MtcReceiver(MtcReceiver::SkipPortOpenTag{}));
    rx->resetDecoderStateForTesting();
    TickSeqlock seq;
    MtcReceiver::setTickCallback([&seq](long head, bool complete) { seq.onTick(head, complete, g_simNowNs); });
    MtcInvalidHold hold;
    TickSample sample;
    size_t i = 0;
    for (int64_t now = 0; now < durNs; now += 16666667) {
        while (i < d.size() && d[i].at <= now) {
            g_simNowNs = d[i].at;
            Msg m = d[i].msg;
            if (m[0] == 0xF1) rx->decodeQuarterFrameForTesting(m);
            else rx->decodeFullFrameForTesting(m);
            ++i;
        }
        seq.read(sample);
        const auto e = hold.update(now, MtcReceiver::mtcHead.load(), sample);
        if (e.entered) r.entered.push_back(now);
        if (e.left) r.left.push_back(now);
    }
    MtcReceiver::setTickCallback(nullptr);
    rx.reset();
    MtcReceiver::resetStaticStateForTesting();
    return r;
}

} // namespace

// A healthy stream through the real decoder never holds (60 s, full frames).
bool test_MtcHold_HealthyStreamNeverHolds() {
    const int64_t dur = 60000 * MS;
    const HoldRun r = runHold(stream(3600000, dur, -1, -1), dur);
    TEST_ASSERT_EQ(r.entered.size(), (size_t)0);
    return true;
}

// Doubled quarter frames through the real decoder: held within 800 ms + a
// group of the doubling (no hunting), left once it stops.
bool test_MtcHold_DoubledQuarterFramesHoldThenRecover() {
    const int64_t dur = 30000 * MS;
    const HoldRun r = runHold(stream(3600000, dur, 10000 * MS, 20000 * MS), dur);
    TEST_ASSERT_EQ(r.entered.size(), (size_t)1);
    const int64_t dt = r.entered[0] - 10000 * MS;
    TEST_ASSERT_TRUE(dt >= 700 * MS && dt <= 1100 * MS);
    TEST_ASSERT_EQ(r.left.size(), (size_t)1);
    TEST_ASSERT_TRUE(r.left[0] >= 20000 * MS && r.left[0] <= 20600 * MS);
    return true;
}

#endif // HAVE_MTCRECEIVER && MTCRECV_TESTING
