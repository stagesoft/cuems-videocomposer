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

/**
 * @file TestVisibleAt.cpp
 * @brief The self-gated reveal `/videocomposer/layer/<id>/visible_at` (869feynrj step 1b, plan §4.6, §7.1 VC rows).
 *
 * A real VideoLayer over the real FramerateConverterSyncSource, driven through the vblank tick seams (PresentTick.h)
 * exactly as TestFramerateConverter replays the 1amfwm7gpz7 model: per poll the clock at the presentation vblank, that
 * vblank (tgt) and the tick's validity. The model's chosen frames are the oracle: with the layer's start at absolute
 * frame S (offset -S), the reveal must come on the first poll whose chosen frame is >= S, in that same update (so it
 * rides the same submission as the frame), showing frame frames[i] - S, which is 0 unless the cadence itself never
 * presents frame S (drop regime). Fixtures: test/fixtures/cadence/ (the model's own export) and
 * test/fixtures/visible_at/ (25 and 30 Hz outputs, gen_fixtures.py from the same model).
 */

#include "TestFramework.h"
#include "../layer/VideoLayer.h"
#include "../sync/FramerateConverterSyncSource.h"
#include "../sync/PresentTick.h"
#include "../sync/SyncSource.h"
#include "../input/InputSource.h"
#include "../remote/JournalContract.h"
#include "../utils/CapsFile.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace videocomposer;

namespace {

// ---- fakes ----------------------------------------------------------------------------------------------------------

/// The MTC clock (25 fps) whose value is whatever the harness set before the poll.
class GateClock : public SyncSource {
public:
    double valueMs = 0.0;
    bool noFrame = false;                  // no MTC: the converter's poll gives syncFrame < 0
    bool connect(const char*) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    int64_t pollFrame(uint8_t* rolling) override { if (rolling) *rolling = noFrame ? 0 : 1; return noFrame ? -1 : 999; }
    int64_t getCurrentFrame() const override { return noFrame ? -1 : 999; }
    const char* getName() const override { return "gate-clock"; }
    double getFramerate() const override { return 25.0; }
    long getTimeMs() const override { return noFrame ? -1 : static_cast<long>(std::floor(valueMs)); }
    double getTimeMsAt(int64_t) const override { return noFrame ? -1.0 : valueMs; }
};

/// A file of `fps` and `total` frames; records the last frame loaded; `fail` = a decode miss. With `holdOnMiss` a miss
/// reports success while the buffer keeps the previous frame (the async decode queue's hold, VideoFileInput /
/// HAPVideoInput): `delivered` is what the pixels are.
class GateInput : public InputSource {
public:
    GateInput(double fps, int64_t total) : fps_(fps), total_(total) {}
    bool open(const std::string&) override { return true; }
    void close() override {}
    bool seek(int64_t) override { return true; }
    bool readFrame(int64_t frameNumber, FrameBuffer&) override {
        if (fail) return holdOnMiss && delivered >= 0;
        last = frameNumber;
        delivered = frameNumber;
        ++loads;
        return true;
    }
    int64_t deliveredFrame(int64_t) const override { return delivered; }
    FrameInfo getFrameInfo() const override {
        FrameInfo info;
        info.width = 1920;
        info.height = 1080;
        info.framerate = fps_;
        info.totalFrames = total_;
        return info;
    }
    bool isReady() const override { return true; }
    int64_t getCurrentFrame() const override { return last; }
    CodecType detectCodec() const override { return CodecType::SOFTWARE; }
    bool supportsDirectGPUTexture() const override { return false; }
    DecodeBackend getOptimalBackend() const override { return DecodeBackend::CPU_SOFTWARE; }
    int64_t last = -1;
    int64_t delivered = -1;
    int64_t loads = 0;
    bool fail = false;
    bool holdOnMiss = false;
private:
    double fps_;
    int64_t total_;
};

// ---- fixtures (the format of run_scenarios.py --export) -------------------------------------------------------------

struct Fixture {
    std::string name;
    double fps = 0;
    int64_t periodNs = 0;
    std::vector<double> pos;
    std::vector<int64_t> msc;
    std::vector<bool> valid;
    std::vector<int64_t> frames;
    bool hasOffset = false;                // 'O' line (va_res_*): the engine offset and its split
    int64_t offset = 0;
    int64_t whole = 0;
    double phaseMs = 0.0;
    bool loaded = false;
};

std::string fixtureDir() {
    const char* env = std::getenv("VC_TEST_FIXTURE_DIR");
    if (env && *env) return env;
#ifdef VC_TEST_FIXTURE_DIR
    return VC_TEST_FIXTURE_DIR;
#else
    return "src/cuems_videocomposer/cpp/test/fixtures";
#endif
}

/// `sub/name`: cadence/<model fixture> or visible_at/<va_ fixture>.
Fixture loadFixture(const std::string& sub, const std::string& name) {
    Fixture f;
    f.name = name;
    const std::string path = fixtureDir() + "/" + sub + "/" + name + ".txt";
    std::ifstream in(path);
    if (!in) {
        std::cerr << "fixture not found: " << path << " (set VC_TEST_FIXTURE_DIR)\n";
        return f;
    }
    std::string line;
    if (!std::getline(in, line)) return f;
    {
        std::istringstream h(line);
        h >> f.fps >> f.periodNs;
    }
    while (std::getline(in, line)) {
        std::istringstream s(line);
        char tag = 0;
        s >> tag;
        if (tag == 'P') {
            double p; int64_t m; int v; int64_t fr;
            s >> p >> m >> v >> fr;
            f.pos.push_back(p); f.msc.push_back(m); f.valid.push_back(v != 0); f.frames.push_back(fr);
        } else if (tag == 'O') {
            s >> f.offset >> f.whole >> f.phaseMs;
            f.hasOffset = true;
        }
    }
    f.loaded = !f.pos.empty();
    return f;
}

constexpr int64_t kMscOffset = 1000;

/// The presentation target of the poll is `msc` exactly (as in TestFramerateConverter): tick = msc − 3,
/// now = msc − 2, previous landing = msc − 1 → tgt = max(prev, now) + 1 = msc.
void publishForTarget(int64_t msc, int64_t periodNs, bool valid) {
    PresentTick tick;
    tick.msc = msc - 3 + kMscOffset;
    tick.ustNs = valid ? tick.msc * periodNs : 0;
    tick.periodNs = periodNs;
    publishPresentTick(tick);
    VblankNow now;
    now.msc = msc - 2 + kMscOffset;
    now.ustNs = now.msc * periodNs;
    now.prevLanding = msc - 1 + kMscOffset;
    now.sampledAtNs = now.ustNs + 300000;
    now.fromIoctl = true;
    publishVblankNow(now);
}

// ---- the rig --------------------------------------------------------------------------------------------------------

/// The engine's offset (25 fps MTC frames) that puts a layer's frame 0 at absolute content frame `S`. For 30 fps only
/// S multiple of 6 (offset multiple of 5) is exact whatever the rounding of the 25 -> 30 conversion.
int64_t mtcOffsetFor(double fps, int64_t S) {
    if (std::abs(fps - 25.0) < 0.01) return -S;
    return -static_cast<int64_t>(std::llround(static_cast<double>(S) * 25.0 / fps));
}

struct Rig {
    std::unique_ptr<GateClock> clock;      // declared first: outlives the converter the layer owns
    std::unique_ptr<VideoLayer> layer;
    GateInput* input = nullptr;
    const Fixture* fx = nullptr;
    size_t next = 0;                       // the next poll of the fixture

    /// One main-loop iteration: the clock at tgt, the tick, then the layer's update(). Returns the poll's index.
    size_t step() {
        const size_t i = next++;
        clock->valueMs = fx->pos[i] * 1000.0 / fx->fps;
        publishForTarget(fx->msc[i], fx->periodNs, fx->valid[i]);
        layer->update();
        return i;
    }
    bool visible() const { return layer->properties().visible; }
};

Rig makeRigOffset(const Fixture& f, int64_t mtcOffset, int64_t totalFrames, bool wrap) {
    Rig r;
    r.fx = &f;
    r.clock = std::make_unique<GateClock>();
    r.layer = std::make_unique<VideoLayer>();
    auto in = std::make_unique<GateInput>(f.fps, totalFrames);
    r.input = in.get();
    r.layer->setInputSource(std::move(in));
    r.layer->setSyncSource(std::make_unique<FramerateConverterSyncSource>(r.clock.get(), r.input));
    r.layer->setCueId("visible-at-test-cue");
    r.layer->setWraparound(wrap);
    r.layer->setTimeOffset(mtcOffset);
    r.layer->setMtcFollow(true);
    return r;
}

Rig makeRig(const Fixture& f, int64_t S, int64_t totalFrames, bool wrap) {
    return makeRigOffset(f, mtcOffsetFor(f.fps, S), totalFrames, wrap);
}

/// The oracle: the first poll whose model frame reaches S (-1: none in the fixture).
int64_t oracleOnset(const Fixture& f, int64_t S) {
    for (size_t i = 0; i < f.frames.size(); ++i) {
        if (f.frames[i] >= S) return static_cast<int64_t>(i);
    }
    return -1;
}

/// The shown frame of the first visible poll: 0 unless the cadence drops frame S (drop regime only).
bool frameIsZeroOrDropped(const Fixture& f, size_t i, int64_t S, bool dropRegime) {
    const int64_t shown = f.frames[i] - S;
    if (shown == 0) return true;
    return dropRegime && shown == 1 && (i == 0 || f.frames[i - 1] < S);
}

/// For each start in `starts`: a fresh layer, visible_at 1 before the first poll, the fixture replayed until a few
/// polls past the onset. Asserts the onset poll, the frame shown with it, the same update, and nothing earlier.
bool onsetRows(const char* sub, const char* name, const std::vector<int64_t>& startsAfterFirst, bool dropRegime,
               int64_t totalFrames = 1000000000LL, bool wrap = false) {
    TickScope scope;
    const Fixture f = loadFixture(sub, name);
    TEST_ASSERT(f.loaded);
    for (const int64_t ds : startsAfterFirst) {
        const int64_t S = f.frames[0] + ds;
        const int64_t onset = oracleOnset(f, S);
        TEST_ASSERT(onset > 0);
        Rig r = makeRig(f, S, totalFrames, wrap);
        r.layer->setVisibleAt(true);
        TEST_ASSERT_TRUE(r.layer->visibleAtPending());
        TEST_ASSERT_FALSE(r.visible());
        const size_t end = std::min(f.pos.size(), static_cast<size_t>(onset) + 6);
        int64_t revealedAt = -1;
        while (r.next < end) {
            const size_t i = r.step();
            if (revealedAt < 0 && r.visible()) {
                revealedAt = static_cast<int64_t>(i);
                // in the same update: the frame chosen at this poll is the one loaded, and it is frame 0
                TEST_ASSERT_EQ(r.layer->getCurrentFrame(), f.frames[i] - S);
                TEST_ASSERT_TRUE(frameIsZeroOrDropped(f, i, S, dropRegime));
                TEST_ASSERT_FALSE(r.layer->properties().awaitingFrame);
                TEST_ASSERT_FALSE(r.layer->visibleAtPending());
            }
            if (revealedAt >= 0) {
                // stays visible and keeps following the model's frames
                TEST_ASSERT_TRUE(r.visible());
                TEST_ASSERT_EQ(r.layer->getCurrentFrame(), f.frames[i] - S);
            }
        }
        if (revealedAt != onset) {
            std::cerr << "  " << name << " S=" << S << ": revealed at poll " << revealedAt << " (tgt "
                      << (revealedAt >= 0 ? f.msc[static_cast<size_t>(revealedAt)] : -1) << "), model onset poll "
                      << onset << " (tgt " << f.msc[static_cast<size_t>(onset)] << ")\n";
        }
        TEST_ASSERT_EQ(revealedAt, onset);
    }
    return true;
}

std::vector<int64_t> range(int64_t from, int64_t to, int64_t step) {
    std::vector<int64_t> v;
    for (int64_t s = from; s <= to; s += step) v.push_back(s);
    return v;
}

/// 30 fps starts: absolute frame multiple of 6 (exact MTC offsets), so the deltas depend on the fixture's first frame.
std::vector<int64_t> starts30(const char* sub, const char* name, int64_t from, int64_t to) {
    const Fixture f = loadFixture(sub, name);
    std::vector<int64_t> v;
    if (!f.loaded) return v;
    for (int64_t S = f.frames[0] + from; S <= f.frames[0] + to; ++S) {
        if (S % 6 == 0) v.push_back(S - f.frames[0]);
    }
    return v;
}

} // namespace

// ---- onset on the vblank the cadence picks for content time 0 (25/30/50/60 Hz, 25 and 30 fps files) ----------------

bool test_VisibleAt_Onset_30at60()  { return onsetRows("cadence", "a_Exact", starts30("cadence", "a_Exact", 20, 90), false); }
bool test_VisibleAt_Onset_25at50()  { return onsetRows("cadence", "b_Exact", range(20, 45, 1), false); }
bool test_VisibleAt_Onset_25at60()  { return onsetRows("cadence", "c_Bresenham", range(20, 45, 1), false); }
bool test_VisibleAt_Onset_30at50()  { return onsetRows("cadence", "e_Bresenham", starts30("cadence", "e_Bresenham", 20, 90), false); }
bool test_VisibleAt_Onset_30at30()  { return onsetRows("cadence", "k_Warmup", starts30("cadence", "k_Warmup", 20, 90), false); }
bool test_VisibleAt_Onset_25at25()  { return onsetRows("visible_at", "va_25at25", range(20, 45, 1), false); }
bool test_VisibleAt_Onset_25at30()  { return onsetRows("visible_at", "va_25at30", range(20, 45, 1), false); }
bool test_VisibleAt_Onset_30at25()  { return onsetRows("visible_at", "va_30at25", starts30("visible_at", "va_30at25", 20, 90), true); }

// The two-deep loop after a delay before the poll (fixture g'': the poll that would target vblank 24 comes late, after
// the previous flip landed, and targets 25: tgt = max(prevLanding, now) + 1, not a fixed v+2). A start whose frame 0 is
// chosen at that delayed poll is revealed there; one whose frame 0 is chosen just before it, there. 30 fps at 60 Hz.
// Exact 30 fps offsets need a start multiple of 6, so the second case shifts the clock by 5 whole frames (the cadence
// is invariant to a whole-frame shift: the fixture's positions are exact quarters).
bool test_VisibleAt_Onset_DelayedPollAfterLandedFlip() {
    TickScope scope;
    const Fixture base = loadFixture("cadence", "g''_PrePollDelay");
    TEST_ASSERT(base.loaded);
    size_t delayed = 0;                         // the poll whose tgt skips a vblank
    for (size_t i = 1; i < base.msc.size(); ++i) {
        if (base.msc[i] - base.msc[i - 1] > 1) { delayed = i; break; }
    }
    TEST_ASSERT(delayed > 0);
    struct Case { int64_t shift; int64_t S; int64_t onset; };
    const Case cases[] = {
        {0, base.frames[delayed - 1], static_cast<int64_t>(delayed - 1)},   // frame 0 chosen right before the delay
        {0, 0, 0},                                                          // filled below: frame 0 at the delayed poll
    };
    for (int c = 0; c < 2; ++c) {
        Fixture f = base;
        int64_t S = cases[c].S;
        int64_t onset = cases[c].onset;
        if (c == 1) {
            const int64_t want = base.frames[delayed];          // the frame chosen at the delayed poll
            const int64_t shift = (6 - want % 6) % 6;
            for (double& p : f.pos) p += static_cast<double>(shift);
            for (int64_t& fr : f.frames) fr += shift;
            S = want + shift;
            onset = static_cast<int64_t>(delayed);
        }
        if (S % 6 != 0) {
            // the first case's start is not an exact 30 fps offset: move it to the next exact one, same rule
            S += (6 - S % 6) % 6;
            onset = oracleOnset(f, S);
        }
        TEST_ASSERT_EQ(oracleOnset(f, S), onset);
        Rig r = makeRig(f, S, 1000000000LL, false);
        r.layer->setVisibleAt(true);
        int64_t revealedAt = -1;
        while (r.next < f.pos.size()) {
            const size_t i = r.step();
            if (revealedAt < 0 && r.visible()) {
                revealedAt = static_cast<int64_t>(i);
                TEST_ASSERT_EQ(r.layer->getCurrentFrame(), static_cast<int64_t>(0));
            }
            if (revealedAt >= 0) {
                TEST_ASSERT_EQ(r.layer->getCurrentFrame(), f.frames[i] - S);   // the replay follows the model
            }
        }
        if (revealedAt != onset) {
            std::cerr << "  g'' case " << c << " S=" << S << ": revealed at poll " << revealedAt << ", model " << onset
                      << " (tgt " << f.msc[static_cast<size_t>(onset)] << ")\n";
        }
        TEST_ASSERT_EQ(revealedAt, onset);
    }
    return true;
}

// ---- the clamp-parked layer and the wrap-mode layer (F2) ------------------------------------------------------------

// Not looping, a 100-frame file, parked at frame 0 by the clamp before the start: invisible until the raw frame reaches
// 0, then visible on frame 0 (not frame 1: the flip does not wait for a frame change, unlike awaitingFrame).
bool test_VisibleAt_ClampParkedLayer_FirstFrameIsZero() {
    return onsetRows("cadence", "a_Exact", starts30("cadence", "a_Exact", 20, 60), false, 100, false);
}

// Looping without a period: before the start the wrap shows the file's tail; the reveal still waits for the raw frame
// to reach 0 and shows frame 0, never the tail.
bool test_VisibleAt_WrapModeLayer_NoTailFlash() {
    TickScope scope;
    const Fixture f = loadFixture("cadence", "b_Exact");
    TEST_ASSERT(f.loaded);
    for (const int64_t ds : range(20, 30, 1)) {
        const int64_t S = f.frames[0] + ds;
        const int64_t onset = oracleOnset(f, S);
        Rig r = makeRig(f, S, 100, true);
        r.layer->setVisibleAt(true);
        bool sawTail = false;
        int64_t revealedAt = -1;
        while (r.next < static_cast<size_t>(onset) + 4) {
            const size_t i = r.step();
            if (!r.visible()) {
                if (r.layer->getCurrentFrame() >= 50) sawTail = true;   // the tail was decoded, hidden
                continue;
            }
            if (revealedAt < 0) {
                revealedAt = static_cast<int64_t>(i);
                TEST_ASSERT_EQ(r.layer->getCurrentFrame(), static_cast<int64_t>(0));
            }
        }
        TEST_ASSERT_TRUE(sawTail);
        TEST_ASSERT_EQ(revealedAt, onset);
    }
    return true;
}

// Looping on the engine's period (869ejxng3): a negative position holds frame 0; the gate is read before the period,
// so the reveal is still on the raw frame reaching 0.
bool test_VisibleAt_LoopPeriodLayer_RevealAtRawZero() {
    TickScope scope;
    const Fixture f = loadFixture("cadence", "b_Exact");
    TEST_ASSERT(f.loaded);
    const int64_t S = f.frames[0] + 25;
    const int64_t onset = oracleOnset(f, S);
    Rig r = makeRig(f, S, 100, true);
    r.layer->setLoopPasses(-1);
    r.layer->setLoopPeriod(4 * 48000, 48000, 100);   // P = 4 s = 100 frames at 25 fps
    r.layer->setVisibleAt(true);
    int64_t revealedAt = -1;
    while (r.next < static_cast<size_t>(onset) + 4) {
        const size_t i = r.step();
        if (!r.visible()) {
            TEST_ASSERT_EQ(r.layer->getCurrentFrame(), static_cast<int64_t>(0));   // held at 0 before the start
            continue;
        }
        if (revealedAt < 0) {
            revealedAt = static_cast<int64_t>(i);
            TEST_ASSERT_EQ(r.layer->getCurrentFrame(), static_cast<int64_t>(0));
        }
    }
    TEST_ASSERT_EQ(revealedAt, onset);
    return true;
}

// A decode miss on the onset poll: the frame chosen there is not the one on screen, so the reveal waits for the poll
// that loads its frame (a wrap-mode layer would otherwise flash the tail it is still showing).
bool test_VisibleAt_DecodeMissAtOnset_WaitsForTheFrame() {
    TickScope scope;
    const Fixture f = loadFixture("cadence", "b_Exact");
    TEST_ASSERT(f.loaded);
    const int64_t S = f.frames[0] + 25;
    const int64_t onset = oracleOnset(f, S);
    Rig r = makeRig(f, S, 100, true);
    r.layer->setVisibleAt(true);
    while (r.next < static_cast<size_t>(onset)) {
        r.step();
        TEST_ASSERT_FALSE(r.visible());
    }
    r.input->fail = true;
    r.step();                                   // the onset poll: its frame 0 is not loaded
    TEST_ASSERT_FALSE(r.visible());
    TEST_ASSERT_TRUE(r.layer->visibleAtPending());
    r.input->fail = false;
    const size_t i = r.step();
    TEST_ASSERT_TRUE(r.visible());
    TEST_ASSERT_EQ(r.layer->getCurrentFrame(), f.frames[i] - S);
    TEST_ASSERT(r.layer->getCurrentFrame() < 50);   // never the tail
    return true;
}

// ---- cancel (D5): before the onset never visible; after it hidden at once; nothing pending → ignored -----------------

bool test_VisibleAt_CancelBeforeOnset_NeverVisible() {
    TickScope scope;
    const Fixture f = loadFixture("cadence", "b_Exact");
    TEST_ASSERT(f.loaded);
    const int64_t S = f.frames[0] + 25;
    const int64_t onset = oracleOnset(f, S);
    Rig r = makeRig(f, S, 1000000000LL, false);
    r.layer->setVisibleAt(true);
    while (r.next + 2 < static_cast<size_t>(onset)) r.step();
    TEST_ASSERT_TRUE(r.layer->setVisibleAt(false) == VideoLayer::VisibleAtOutcome::Dropped);
    TEST_ASSERT_FALSE(r.layer->visibleAtPending());
    while (r.next < static_cast<size_t>(onset) + 20) {
        r.step();
        TEST_ASSERT_FALSE(r.visible());
    }
    return true;
}

bool test_VisibleAt_CancelAfterOnset_HiddenNextVsync() {
    TickScope scope;
    const Fixture f = loadFixture("cadence", "b_Exact");
    TEST_ASSERT(f.loaded);
    const int64_t S = f.frames[0] + 25;
    const int64_t onset = oracleOnset(f, S);
    Rig r = makeRig(f, S, 1000000000LL, false);
    r.layer->setVisibleAt(true);
    while (r.next <= static_cast<size_t>(onset) + 3) r.step();
    TEST_ASSERT_TRUE(r.visible());
    TEST_ASSERT_TRUE(r.layer->setVisibleAt(false) == VideoLayer::VisibleAtOutcome::Hidden);
    TEST_ASSERT_FALSE(r.visible());             // the next render (the next vsync's submission) draws nothing
    for (int k = 0; k < 10; ++k) {
        r.step();
        TEST_ASSERT_FALSE(r.visible());
    }
    // a second cancel has nothing left to do
    TEST_ASSERT_TRUE(r.layer->setVisibleAt(false) == VideoLayer::VisibleAtOutcome::Ignored);
    return true;
}

bool test_VisibleAt_CancelNothingPending_Ignored() {
    TickScope scope;
    const Fixture f = loadFixture("cadence", "b_Exact");
    TEST_ASSERT(f.loaded);
    Rig r = makeRig(f, f.frames[0] + 25, 1000000000LL, false);
    TEST_ASSERT_TRUE(r.layer->setVisibleAt(false) == VideoLayer::VisibleAtOutcome::Ignored);
    TEST_ASSERT_FALSE(r.visible());
    // a layer shown by /visible 1 (no gate) is not the gate's to hide
    r.layer->setVisible(true);
    TEST_ASSERT_TRUE(r.layer->setVisibleAt(false) == VideoLayer::VisibleAtOutcome::Ignored);
    TEST_ASSERT_TRUE(r.visible());
    return true;
}

// ---- /visible next to the gate (N12) and without it (today's behaviour) --------------------------------------------

bool test_VisibleAt_Visible1WhilePending_NoEarlyFlipNoAwaitingFrame() {
    TickScope scope;
    const Fixture f = loadFixture("cadence", "b_Exact");
    TEST_ASSERT(f.loaded);
    const int64_t S = f.frames[0] + 25;
    const int64_t onset = oracleOnset(f, S);
    Rig r = makeRig(f, S, 1000000000LL, false);
    r.layer->setVisibleAt(true);
    r.step();
    r.layer->setVisible(true);                  // the engine's healer, early
    TEST_ASSERT_FALSE(r.visible());
    TEST_ASSERT_FALSE(r.layer->properties().awaitingFrame);
    TEST_ASSERT_TRUE(r.layer->visibleAtPending());
    int64_t revealedAt = -1;
    while (r.next < static_cast<size_t>(onset) + 3) {
        const size_t i = r.step();
        if (revealedAt < 0 && r.visible()) {
            revealedAt = static_cast<int64_t>(i);
            TEST_ASSERT_EQ(r.layer->getCurrentFrame(), static_cast<int64_t>(0));
        }
    }
    TEST_ASSERT_EQ(revealedAt, onset);
    return true;
}

bool test_VisibleAt_Visible1AfterReveal_NoChange() {
    TickScope scope;
    const Fixture f = loadFixture("cadence", "b_Exact");
    TEST_ASSERT(f.loaded);
    const int64_t S = f.frames[0] + 25;
    const int64_t onset = oracleOnset(f, S);
    Rig r = makeRig(f, S, 1000000000LL, false);
    r.layer->setVisibleAt(true);
    while (r.next <= static_cast<size_t>(onset)) r.step();
    TEST_ASSERT_TRUE(r.visible());
    const int64_t frame = r.layer->getCurrentFrame();
    r.layer->setVisible(true);                  // the healer at the engine's polled reveal
    TEST_ASSERT_TRUE(r.visible());
    TEST_ASSERT_FALSE(r.layer->properties().awaitingFrame);
    TEST_ASSERT_EQ(r.layer->getCurrentFrame(), frame);
    // a repeated visible_at 1 on a revealed layer changes nothing either
    r.layer->setVisibleAt(true);
    TEST_ASSERT_TRUE(r.visible());
    TEST_ASSERT_FALSE(r.layer->visibleAtPending());
    return true;
}

// /visible 0 while the gate is pending drops it: an explicit hide is never undone by a later reveal.
bool test_VisibleAt_Visible0WhilePending_DropsTheGate() {
    TickScope scope;
    const Fixture f = loadFixture("cadence", "b_Exact");
    TEST_ASSERT(f.loaded);
    const int64_t S = f.frames[0] + 25;
    const int64_t onset = oracleOnset(f, S);
    Rig r = makeRig(f, S, 1000000000LL, false);
    r.layer->setVisibleAt(true);
    r.step();
    r.layer->setVisible(false);
    TEST_ASSERT_FALSE(r.layer->visibleAtPending());
    while (r.next < static_cast<size_t>(onset) + 10) {
        r.step();
        TEST_ASSERT_FALSE(r.visible());
    }
    return true;
}

// Without visible_at nothing changes: polls never touch visibility, /visible 1 sets awaitingFrame as before.
bool test_VisibleAt_WithoutCommand_TodaysBehaviour() {
    TickScope scope;
    const Fixture f = loadFixture("cadence", "b_Exact");
    TEST_ASSERT(f.loaded);
    const int64_t S = f.frames[0] + 25;
    const int64_t onset = oracleOnset(f, S);
    Rig r = makeRig(f, S, 1000000000LL, false);
    while (r.next < static_cast<size_t>(onset) + 10) {
        r.step();
        TEST_ASSERT_FALSE(r.visible());
        TEST_ASSERT_FALSE(r.layer->visibleAtPending());
    }
    r.layer->setVisible(true);
    TEST_ASSERT_TRUE(r.visible());
    TEST_ASSERT_TRUE(r.layer->properties().awaitingFrame);
    const int64_t before = r.layer->getCurrentFrame();
    for (int k = 0; k < 4 && r.layer->getCurrentFrame() == before; ++k) {
        r.step();                               // held frames keep it set; a fresh frame clears it, as before
        TEST_ASSERT_EQ(r.layer->properties().awaitingFrame, r.layer->getCurrentFrame() == before);
    }
    TEST_ASSERT_FALSE(r.layer->properties().awaitingFrame);
    r.layer->setVisible(false);
    TEST_ASSERT_FALSE(r.visible());
    return true;
}

// ---- review fixes (2026-11-10) ------------------------------------------------------------------------------------

// X4: the five residues of a 30 fps start on the 25 fps MTC grid (1amfwm7h924's offset split: whole frames + a phase
// the converter adds to its clock), at 60 and 50 Hz. Expectations: the model run on the phase-shifted clock
// (gen_fixtures.py, va_res_*); the reveal is the first poll whose model frame + whole >= 0, showing frame 0.
bool test_VisibleAt_Onset_30p_OffsetResidues() {
    TickScope scope;
    int residuesSeen[5] = {0, 0, 0, 0, 0};
    for (const char* tag : {"30at60_", "30at50_"}) {
        const int first = std::string(tag) == "30at60_" ? 290 : 310;
        for (int n = first; n < first + 10; ++n) {
            const Fixture f = loadFixture("visible_at", std::string("va_res_") + tag + std::to_string(n));
            TEST_ASSERT(f.loaded);
            TEST_ASSERT(f.hasOffset);
            ++residuesSeen[(-f.offset) % 5];
            int64_t onset = -1;
            for (size_t i = 0; i < f.frames.size(); ++i) {
                if (f.frames[i] + f.whole >= 0) { onset = static_cast<int64_t>(i); break; }
            }
            TEST_ASSERT(onset > 0);
            Rig r = makeRigOffset(f, f.offset, 1000000000LL, false);
            r.layer->setVisibleAt(true);
            int64_t revealedAt = -1;
            const size_t end = std::min(f.pos.size(), static_cast<size_t>(onset) + 6);
            while (r.next < end) {
                const size_t i = r.step();
                if (revealedAt < 0 && r.visible()) {
                    revealedAt = static_cast<int64_t>(i);
                    TEST_ASSERT_EQ(r.layer->getCurrentFrame(), static_cast<int64_t>(0));
                }
                if (revealedAt >= 0) {
                    TEST_ASSERT_EQ(r.layer->getCurrentFrame(), f.frames[i] + f.whole);   // the phased replay follows the model
                }
            }
            if (revealedAt != onset) {
                std::cerr << "  va_res_" << tag << n << " (phase " << f.phaseMs << " ms): revealed at poll " << revealedAt
                          << ", model " << onset << "\n";
            }
            TEST_ASSERT_EQ(revealedAt, onset);
        }
    }
    for (int k = 0; k < 5; ++k) TEST_ASSERT(residuesSeen[k] > 0);
    return true;
}

// X1: a decode miss that reports success while holding the previous frame (the async queue's hold) does not reveal: on
// a wrap layer the held frame is the file's tail. The next poll that delivers the chosen frame does.
bool test_VisibleAt_HeldFrameOnMiss_DoesNotReveal() {
    TickScope scope;
    const Fixture f = loadFixture("cadence", "b_Exact");
    TEST_ASSERT(f.loaded);
    const int64_t S = f.frames[0] + 25;
    const int64_t onset = oracleOnset(f, S);
    Rig r = makeRig(f, S, 100, true);
    r.input->holdOnMiss = true;
    r.layer->setVisibleAt(true);
    while (r.next < static_cast<size_t>(onset)) r.step();
    const int64_t tail = r.input->delivered;
    TEST_ASSERT(tail >= 50);
    r.input->fail = true;
    r.step();                                   // the onset poll: "loaded" (currentFrame 0), pixels still the tail
    TEST_ASSERT_EQ(r.layer->getCurrentFrame(), static_cast<int64_t>(0));
    TEST_ASSERT_EQ(r.input->delivered, tail);
    TEST_ASSERT_FALSE(r.layer->playback().contentStarted());
    TEST_ASSERT_FALSE(r.visible());
    TEST_ASSERT_TRUE(r.layer->visibleAtPending());
    // still held on the next poll (same frame 0 chosen, no new load attempted? the frame number did not change, so the
    // layer does not reload): the gate keeps waiting until a frame is really delivered
    r.input->fail = false;
    size_t i = r.step();
    while (!r.visible() && r.next < static_cast<size_t>(onset) + 6) i = r.step();
    TEST_ASSERT_TRUE(r.visible());
    TEST_ASSERT_EQ(r.input->delivered, r.layer->getCurrentFrame());
    TEST_ASSERT_EQ(r.layer->getCurrentFrame(), f.frames[i] - S);
    TEST_ASSERT(r.layer->getCurrentFrame() < 50);
    return true;
}

// X1, shared decoder: a secondary layer shows the driver's cached frame. When the driver's decode holds the tail, the
// secondary does not reveal either.
bool test_VisibleAt_SharedSecondary_DriverHold_DoesNotReveal() {
    TickScope scope;
    const Fixture f = loadFixture("cadence", "b_Exact");
    TEST_ASSERT(f.loaded);
    const int64_t S = f.frames[0] + 25;
    const int64_t onset = oracleOnset(f, S);
    auto clock = std::make_unique<GateClock>();
    auto input = std::make_shared<GateInput>(f.fps, 100);
    input->holdOnMiss = true;
    std::shared_ptr<SyncSource> sync = std::make_shared<FramerateConverterSyncSource>(clock.get(), input.get());
    VideoLayer driver, secondary;
    driver.playback().setInputSource(input, true, true);
    driver.playback().setSyncSource(sync);
    secondary.playback().setInputSource(input, true, false);
    secondary.playback().setSyncSource(sync);
    for (VideoLayer* l : {&driver, &secondary}) {
        l->setCueId(l == &driver ? "drv" : "sec");
        l->setWraparound(true);
        l->setTimeOffset(-S);
        l->setMtcFollow(true);
        l->setVisibleAt(true);
    }
    auto step = [&](size_t i) {
        clock->valueMs = f.pos[i] * 1000.0 / f.fps;
        publishForTarget(f.msc[i], f.periodNs, f.valid[i]);
        driver.update();                        // the driver first (update priority 0), as LayerManager orders them
        secondary.update();
    };
    size_t i = 0;
    for (; i < static_cast<size_t>(onset); ++i) step(i);
    TEST_ASSERT_FALSE(driver.properties().visible);
    TEST_ASSERT_FALSE(secondary.properties().visible);
    input->fail = true;
    step(i++);
    TEST_ASSERT_FALSE(driver.properties().visible);
    TEST_ASSERT_FALSE(secondary.properties().visible);
    input->fail = false;
    for (; i < static_cast<size_t>(onset) + 6 && !secondary.properties().visible; ++i) step(i);
    TEST_ASSERT_TRUE(driver.properties().visible);
    TEST_ASSERT_TRUE(secondary.properties().visible);
    TEST_ASSERT(secondary.getCurrentFrame() < 50);
    return true;
}

// S1/S4: a pending gate on a layer that has no sync frame (not following, or MTC gives none) cannot fire; the engine's
// healer /visible 1 then shows the layer at once, as today (awaitingFrame), instead of being swallowed.
bool test_VisibleAt_PendingNoMtcFollow_HealerFlips() {
    TickScope scope;
    const Fixture f = loadFixture("cadence", "b_Exact");
    TEST_ASSERT(f.loaded);
    const int64_t S = f.frames[0] + 25;
    Rig r = makeRig(f, S, 1000000000LL, false);
    r.layer->setMtcFollow(false);
    r.layer->setVisibleAt(true);
    while (r.next < static_cast<size_t>(oracleOnset(f, S)) + 10) {
        r.step();
        TEST_ASSERT_FALSE(r.visible());         // never fires: no poll
    }
    TEST_ASSERT_TRUE(r.layer->visibleAtPending());
    r.layer->setVisible(true);                  // the healer
    TEST_ASSERT_TRUE(r.visible());
    TEST_ASSERT_TRUE(r.layer->properties().awaitingFrame);   // today's 0->1 semantics
    TEST_ASSERT_FALSE(r.layer->visibleAtPending());
    return true;
}

bool test_VisibleAt_PendingNoSyncFrame_HealerFlips() {
    TickScope scope;
    const Fixture f = loadFixture("cadence", "b_Exact");
    TEST_ASSERT(f.loaded);
    const int64_t S = f.frames[0] + 25;
    Rig r = makeRig(f, S, 1000000000LL, false);
    r.clock->noFrame = true;
    r.layer->setVisibleAt(true);
    for (int k = 0; k < 20; ++k) {
        r.step();
        TEST_ASSERT_FALSE(r.visible());
        TEST_ASSERT_FALSE(r.layer->playback().hasContentFrame());
    }
    r.layer->setVisible(true);
    TEST_ASSERT_TRUE(r.visible());
    TEST_ASSERT_FALSE(r.layer->visibleAtPending());
    return true;
}

// S1: a gate that stays pending longer than the bound says so, once per gate.
bool test_VisibleAt_PendingTooLong_WarnsOnce() {
    TickScope scope;
    const Fixture f = loadFixture("cadence", "b_Exact");
    TEST_ASSERT(f.loaded);
    TEST_ASSERT_EQ(VideoLayer::kVisibleAtPendingWarnMs, static_cast<int64_t>(6000));
    const int64_t S = f.frames[0] + 25;
    Rig r = makeRig(f, S, 1000000000LL, false);
    r.layer->setMtcFollow(false);
    r.layer->setVisibleAtWarnAfterMsForTesting(0);
    r.layer->setVisibleAt(true);
    TEST_ASSERT_EQ(r.layer->visibleAtPendingWarnings(), 0);
    r.step();
    TEST_ASSERT_EQ(r.layer->visibleAtPendingWarnings(), 1);
    r.step();
    r.step();
    TEST_ASSERT_EQ(r.layer->visibleAtPendingWarnings(), 1);  // once per gate
    r.layer->setVisibleAt(false);
    r.layer->setVisibleAt(true);                // a new gate may warn again
    r.step();
    TEST_ASSERT_EQ(r.layer->visibleAtPendingWarnings(), 2);
    return true;
}

// S4: the gate reads the raw frame before the clamp: a layer parked at frame 0 by the clamp before its start has
// currentFrame 0 but contentStarted() false and a negative contentFrame().
bool test_VisibleAt_ContentFrameReadBeforeClamp() {
    TickScope scope;
    const Fixture f = loadFixture("cadence", "b_Exact");
    TEST_ASSERT(f.loaded);
    const int64_t S = f.frames[0] + 25;
    const int64_t onset = oracleOnset(f, S);
    Rig r = makeRig(f, S, 100, false);
    while (r.next < static_cast<size_t>(onset)) {
        r.step();
        TEST_ASSERT_EQ(r.layer->getCurrentFrame(), static_cast<int64_t>(0));
        TEST_ASSERT(r.layer->playback().contentFrame() < 0);
        TEST_ASSERT_FALSE(r.layer->playback().contentStarted());
    }
    r.step();
    TEST_ASSERT_TRUE(r.layer->playback().contentStarted());
    TEST_ASSERT_EQ(r.layer->playback().contentFrame(), static_cast<int64_t>(0));
    return true;
}

// S3: a layer shown by /visible 1 after a gate reveal and a hide is not the gate's any more: visible_at 0 leaves it.
bool test_VisibleAt_RevealedFlagClearedByLaterShow() {
    TickScope scope;
    const Fixture f = loadFixture("cadence", "b_Exact");
    TEST_ASSERT(f.loaded);
    const int64_t S = f.frames[0] + 25;
    const int64_t onset = oracleOnset(f, S);
    Rig r = makeRig(f, S, 1000000000LL, false);
    r.layer->setVisibleAt(true);
    while (r.next <= static_cast<size_t>(onset)) r.step();
    TEST_ASSERT_TRUE(r.visible());
    r.layer->properties().visible = false;      // a hide that bypasses setVisible
    r.layer->setVisible(true);                  // then a plain show
    TEST_ASSERT_TRUE(r.layer->setVisibleAt(false) == VideoLayer::VisibleAtOutcome::Ignored);
    TEST_ASSERT_TRUE(r.visible());
    // and arming a new gate clears it too
    r.layer->properties().visible = false;
    TEST_ASSERT_TRUE(r.layer->setVisibleAt(true) == VideoLayer::VisibleAtOutcome::Armed);
    TEST_ASSERT_TRUE(r.layer->setVisibleAt(false) == VideoLayer::VisibleAtOutcome::Dropped);
    TEST_ASSERT_TRUE(r.layer->setVisibleAt(false) == VideoLayer::VisibleAtOutcome::Ignored);
    return true;
}

// ---- the journal lines and the caps token ---------------------------------------------------------------------------

bool test_VisibleAt_JournalContract() {
    TEST_ASSERT_EQ(journal::visibleAtContent("abc_0", 0), std::string("layer abc_0 visible at content 0 ms"));
    TEST_ASSERT_EQ(journal::visibleAtContent("abc_0", 40), std::string("layer abc_0 visible at content 40 ms"));
    TEST_ASSERT_EQ(journal::visibleAtCancelledAfter("abc_0", 37),
                   std::string("layer abc_0 visible_at cancelled after 37 ms visible"));
    TEST_ASSERT_EQ(journal::visibleAtCancelledBefore("abc_0"),
                   std::string("layer abc_0 visible_at cancelled before the reveal"));
    TEST_ASSERT_EQ(journal::visibleAtCancelIgnored("abc_0"),
                   std::string("layer abc_0 visible_at 0 ignored: no reveal pending or done"));
    TEST_ASSERT_EQ(journal::visibleAtDroppedByVisible0("abc_0"),
                   std::string("layer abc_0 visible_at dropped by visible 0"));
    TEST_ASSERT_EQ(journal::visibleAtHealerShows("abc_0"),
                   std::string("layer abc_0 visible_at pending without an MTC position: visible 1 shows it now"));
    TEST_ASSERT_EQ(journal::visibleAtPendingLong("abc_0", 6012, true, false),
                   std::string("layer abc_0 visible_at pending for 6012 ms without a frame at content time >= 0"
                               " (following 1, MTC position 0)"));
    return true;
}

bool test_VisibleAt_CapsToken() {
    const std::vector<std::string> t = capsfile::tokens();
    TEST_ASSERT_TRUE(std::find(t.begin(), t.end(), "loop_period") != t.end());
    TEST_ASSERT_TRUE(std::find(t.begin(), t.end(), "visible_at") != t.end());
    return true;
}
