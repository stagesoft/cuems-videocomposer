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
 * @file TestFramerateConverter.cpp
 * @brief The cadence engine of FramerateConverterSyncSource against the fixtures of its reference model.
 *
 * Plan 1amfwm7gpz7 step 2 (cuems-RELATIONS Plans/2026-11-04-vc-cadence-exact-ratio-min-hold-1amfwm7gpz7.md).
 * Every fixture in test/fixtures/cadence/ was exported by the model's `run_scenarios.py --export`
 * (cuems-RELATIONS baselines/test3-saltitos-20261104/model/): per poll the content position at the presentation
 * vblank, that vblank, the tick's validity and the frame the model chose; then the events and the counters. The
 * test replays the inputs exactly through the real converter via the tick seams (PresentTick.h) and a fake clock,
 * and compares. Rounding is the model's: floor(x + 0.5), halves away from zero.
 *
 * Model-only rows (not replayed here): (o) centred admission (needs the engine's admission phase), (s) the
 * anchored hold = the remainder. Harness-only rows: (l) no tick, (q) frozen tick, (t) band stability with jitter,
 * (u) four converters on one tick.
 */

#include "TestFramework.h"
#include "../sync/FramerateConverterSyncSource.h"
#include "../sync/PresentTick.h"
#include "../sync/SyncSource.h"
#include "../input/InputSource.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace videocomposer;

namespace {

// ---- fakes ----------------------------------------------------------------------------------------------------------

/// A sync source whose clock is whatever the harness set before the poll (the model's `pos`, in ms of content time).
class CadenceClock : public SyncSource {
public:
    double valueMs = 0.0;
    double fps = 25.0;
    bool connect(const char*) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    int64_t pollFrame(uint8_t* rolling) override { if (rolling) *rolling = 1; return 999; }
    int64_t getCurrentFrame() const override { return 999; }
    const char* getName() const override { return "cadence-clock"; }
    double getFramerate() const override { return fps; }
    long getTimeMs() const override { return static_cast<long>(std::floor(valueMs)); }
    double getTimeMsAt(int64_t) const override { return valueMs; }
};

/// An input source of a given frame rate, long enough never to clamp.
class CadenceInput : public InputSource {
public:
    explicit CadenceInput(double f) : fps_(f) {}
    bool open(const std::string&) override { return true; }
    void close() override {}
    bool seek(int64_t) override { return true; }
    bool readFrame(int64_t, FrameBuffer&) override { return true; }
    FrameInfo getFrameInfo() const override {
        FrameInfo info;
        info.width = 1920;
        info.height = 1080;
        info.framerate = fps_;
        info.totalFrames = 1000000000LL;
        return info;
    }
    bool isReady() const override { return true; }
    int64_t getCurrentFrame() const override { return 0; }
    CodecType detectCodec() const override { return CodecType::SOFTWARE; }
    bool supportsDirectGPUTexture() const override { return false; }
    DecodeBackend getOptimalBackend() const override { return DecodeBackend::CPU_SOFTWARE; }
private:
    double fps_;
};

// ---- the tick driver ------------------------------------------------------------------------------------------------

constexpr int64_t kMscOffset = 1000;   // fixtures may start at vblank 0; a valid tick needs msc > 0

/// Publish the tick and the current-vblank sample so that the converter's presentation target is `msc` exactly:
/// tick = msc − 3, now = msc − 2, previous landing = msc − 1 → tgt = max(prev, now) + 1 = msc, excess = 0.
void publishForTarget(int64_t msc, int64_t periodNs, bool valid) {
    PresentTick tick;
    tick.msc = msc - 3 + kMscOffset;
    tick.ustNs = valid ? tick.msc * periodNs : 0;   // ust 0 = an invalid tick (the layer holds its frame)
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

struct Fixture {
    double fps = 0;
    int64_t periodNs = 0;
    double H = 0;
    int lowpass = 0;
    int spacing = 0;
    std::vector<double> pos;
    std::vector<int64_t> msc;
    std::vector<bool> valid;
    std::vector<int64_t> frames;
    std::vector<std::pair<int64_t, std::string>> events;
    int64_t corrMinus = 0, corrPlus = 0, skips = 0, drops = 0, stalls = 0, reanchors = 0;
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

Fixture loadFixture(const std::string& name) {
    Fixture f;
    std::ifstream in(fixtureDir() + "/cadence/" + name + ".txt");
    if (!in) {
        std::cerr << "fixture not found: " << fixtureDir() << "/cadence/" << name << ".txt (set VC_TEST_FIXTURE_DIR)\n";
        return f;
    }
    std::string line;
    if (!std::getline(in, line)) return f;
    {
        std::istringstream h(line);
        h >> f.fps >> f.periodNs >> f.H >> f.lowpass >> f.spacing;
    }
    while (std::getline(in, line)) {
        std::istringstream s(line);
        char tag = 0;
        s >> tag;
        if (tag == 'P') {
            double p; int64_t m; int v; int64_t fr;
            s >> p >> m >> v >> fr;
            f.pos.push_back(p); f.msc.push_back(m); f.valid.push_back(v != 0); f.frames.push_back(fr);
        } else if (tag == 'E') {
            int64_t m; std::string k;
            s >> m >> k;
            f.events.emplace_back(m, k);
        } else if (tag == 'C') {
            s >> f.corrMinus >> f.corrPlus >> f.skips >> f.drops >> f.stalls >> f.reanchors;
        }
    }
    f.loaded = !f.pos.empty();
    return f;
}

/// Replay a fixture through a fresh converter; returns the number of polls whose frame differs from the model's.
struct ReplayResult {
    size_t mismatches = 0;
    size_t polls = 0;
    int64_t firstMismatchAt = -1;
    FramerateConverterSyncSource::CadenceCounters counters;
    std::vector<std::pair<int64_t, std::string>> events;
};

ReplayResult replay(const Fixture& f) {
    ReplayResult r;
    TickScope scope;
    CadenceClock clock;
    CadenceInput input(f.fps);
    FramerateConverterSyncSource frc(&clock, &input);
    frc.setCadenceEventCapture(true);
    for (size_t i = 0; i < f.pos.size(); ++i) {
        clock.valueMs = f.pos[i] * 1000.0 / f.fps;
        publishForTarget(f.msc[i], f.periodNs, f.valid[i]);
        const int64_t got = frc.pollFrame(nullptr);
        ++r.polls;
        if (got != f.frames[i]) {
            if (r.firstMismatchAt < 0) r.firstMismatchAt = static_cast<int64_t>(i);
            ++r.mismatches;
        }
    }
    r.counters = frc.getCadenceCounters();
    r.events = frc.cadenceEvents();
    return r;
}

bool checkCounters(const Fixture& f, const ReplayResult& r, bool strictEvents) {
    const auto& c = r.counters;
    TEST_ASSERT_EQ(c.corrMinus, f.corrMinus);
    TEST_ASSERT_EQ(c.corrPlus, f.corrPlus);
    TEST_ASSERT_EQ(c.skips, f.skips);
    TEST_ASSERT_EQ(c.stalls, f.stalls);
    TEST_ASSERT_EQ(c.reanchors, f.reanchors);
    if (strictEvents) {
        // the model's event vblanks are in fixture space; the harness offsets msc by kMscOffset
        size_t n = 0;
        for (const auto& e : r.events) {
            if (e.second == "stall" || e.second == "corr-" || e.second == "corr+" || e.second == "skip") ++n;
        }
        size_t m = 0;
        for (const auto& e : f.events) {
            if (e.second == "stall" || e.second == "corr-" || e.second == "corr+") ++m;
        }
        TEST_ASSERT_EQ(n, m);
    }
    return true;
}

bool replayFixture(const char* name, bool strictEvents = true) {
    const Fixture f = loadFixture(name);
    TEST_ASSERT(f.loaded);
    const ReplayResult r = replay(f);
    if (r.mismatches) {
        std::cerr << "  " << name << ": " << r.mismatches << " of " << r.polls << " polls differ from the model, first at poll "
                  << r.firstMismatchAt << " (model frame " << f.frames[static_cast<size_t>(r.firstMismatchAt)]
                  << ", tickPath=" << (r.counters.tickPath ? "yes" : "no") << ")\n";
    }
    TEST_ASSERT_EQ(r.mismatches, static_cast<size_t>(0));
    TEST_ASSERT(r.counters.tickPath);
    return checkCounters(f, r, strictEvents);
}

} // namespace

// ---- fixture replays (the model's oracle, RESULTS.md) ---------------------------------------------------------------

bool test_Cadence_a_Exact_30at60_NoShortHolds()                 { return replayFixture("a_Exact"); }
bool test_Cadence_b_Exact_25at50_NoShortHolds()                 { return replayFixture("b_Exact"); }
bool test_Cadence_c_Bresenham_25at60_TwoThreeTwoThreeTwo()      { return replayFixture("c_Bresenham"); }
bool test_Cadence_d_Bresenham_24at60_TwoThree()                 { return replayFixture("d_Bresenham"); }
bool test_Cadence_e_Bresenham_30at50_OneTwo()                   { return replayFixture("e_Bresenham"); }
bool test_Cadence_f_NearExact_2997at60_OneLongHoldPer500()      { return replayFixture("f_NearExact"); }
bool test_Cadence_g_StallCPU_30at60_OneEventNoLag()             { return replayFixture("g_StallCPU"); }
bool test_Cadence_g2_StallGPU_30at60_WithinTwoHolds()           { return replayFixture("g'_StallGPU"); }
bool test_Cadence_g3_PrePollDelay_30at60_Clean()                { return replayFixture("g''_PrePollDelay"); }
bool test_Cadence_h_DriftFast100ppm_OneShortPerBeat()           { return replayFixture("h_Drift"); }
bool test_Cadence_h2_DriftSlow100ppm_OneLongPerBeat()           { return replayFixture("h'_Drift"); }
bool test_Cadence_i_BackwardSnap11ms_NoReset()                  { return replayFixture("i_BackwardSnap"); }
bool test_Cadence_j_RelocationTwoFrames_Reanchors()             { return replayFixture("j_Relocation"); }
bool test_Cadence_k_Warmup_30at30_FirstHoldsCorrect()           { return replayFixture("k_Warmup"); }
bool test_Cadence_m_FastContent_60at5979_SkipsAtTheSnapBeat()   { return replayFixture("m_FastContent"); }
bool test_Cadence_n_StoppedClock_HoldsThenResumesInPhase()      { return replayFixture("n_StoppedClock"); }
bool test_Cadence_p_DropRegime_60at50_and_60at30()              { return replayFixture("p_DropRegime"); }
bool test_Cadence_r_TickJumpAndInvalidTicks()                   { return replayFixture("r_TickJump"); }
bool test_Cadence_v_DeferredShorten_30at50_NoSkip()             { return replayFixture("v_DeferredShorten"); }
bool test_Cadence_x_LongHold_1at60_NoPairs()                    { return replayFixture("x_LongHold"); }
// E12: at a non-integer ratio a correction never leaves A = {floor r, ceil r} (red on the first rule)
bool test_Cadence_y_CorrInsideA_25at60_ThreeBecomesTwo()       { return replayFixture("y_CorrInsideA"); }
bool test_Cadence_z_CorrInsideA_30at50_OneBecomesTwo()         { return replayFixture("z_CorrInsideA"); }

// ---- harness-only rows ------------------------------------------------------------------------------------------------

/// (l) No tick → the legacy smoother, unchanged (the pre-existing SameRate expectations), counters say so.
bool test_Cadence_l_NoTick_FallsBackToLegacyPath() {
    TickScope scope;
    CadenceClock clock;
    CadenceInput input(25.0);
    FramerateConverterSyncSource frc(&clock, &input);
    clock.valueMs = 2000;
    TEST_ASSERT_EQ(frc.pollFrame(nullptr), static_cast<int64_t>(50));
    clock.valueMs = 2041;
    TEST_ASSERT_EQ(frc.pollFrame(nullptr), static_cast<int64_t>(50));
    TEST_ASSERT_EQ(frc.pollFrame(nullptr), static_cast<int64_t>(51));
    TEST_ASSERT_FALSE(frc.getCadenceCounters().tickPath);
    return true;
}

/// (q) A tick that stops advancing while the current vblank moves on: WARNING, the legacy path, then a re-anchor.
bool test_Cadence_q_FrozenTick_WarnsAndFallsBack() {
    TickScope scope;
    CadenceClock clock;
    CadenceInput input(30.0);
    FramerateConverterSyncSource frc(&clock, &input);
    frc.setCadenceEventCapture(true);
    const int64_t P = 16666666;
    int64_t msc = 100;
    for (int i = 0; i < 60; ++i, ++msc) {           // steady state
        clock.valueMs = (msc + 2) * (1000.0 / 60.0) / 2.0 * 2.0 / 2.0 + 0.0;  // 30p: one frame per 2 vblanks
        clock.valueMs = (msc + 2) * (1000.0 / 60.0);
        publishForTarget(msc, P, true);
        frc.pollFrame(nullptr);
    }
    TEST_ASSERT(frc.getCadenceCounters().tickPath);
    // the tick freezes at its last value while `now` keeps advancing
    PresentTick frozen = currentPresentTick();
    for (int i = 0; i < 10; ++i, ++msc) {
        clock.valueMs = (msc + 2) * (1000.0 / 60.0);
        publishPresentTick(frozen);
        VblankNow now;
        now.msc = msc - 2 + kMscOffset;
        now.ustNs = now.msc * P;
        now.prevLanding = msc - 1 + kMscOffset;
        now.sampledAtNs = now.ustNs + 300000;
        now.fromIoctl = true;
        publishVblankNow(now);
        frc.pollFrame(nullptr);
    }
    TEST_ASSERT_FALSE(frc.getCadenceCounters().tickPath);   // the legacy path while frozen
    for (int i = 0; i < 20; ++i, ++msc) {           // the tick moves again
        clock.valueMs = (msc + 2) * (1000.0 / 60.0);
        publishForTarget(msc, P, true);
        frc.pollFrame(nullptr);
    }
    TEST_ASSERT(frc.getCadenceCounters().tickPath);
    TEST_ASSERT(frc.getCadenceCounters().reanchors >= 1);
    return true;
}

/// (t) Band stability: three Bresenham ratios with 1 ms gaussian jitter and no drift → no correction at all.
bool test_Cadence_t_BandStability_NoSpuriousCorrections() {
    const struct { double fps; double hz; } rows[] = {{25.0, 60.0}, {24.0, 60.0}, {30.0, 50.0}};
    for (const auto& row : rows) {
        TickScope scope;
        CadenceClock clock;
        CadenceInput input(row.fps);
        FramerateConverterSyncSource frc(&clock, &input);
        std::mt19937 rng(12345);
        std::normal_distribution<double> jitter(0.0, 1.0);
        const int64_t P = static_cast<int64_t>(1e9 / row.hz);
        const double Pms = 1000.0 / row.hz;
        for (int64_t msc = 100; msc < 100 + 20000; ++msc) {
            clock.valueMs = 10000.0 + (msc + 2) * Pms + jitter(rng);
            publishForTarget(msc, P, true);
            frc.pollFrame(nullptr);
        }
        const auto c = frc.getCadenceCounters();
        TEST_ASSERT(c.tickPath);
        TEST_ASSERT_EQ(c.corrMinus, static_cast<int64_t>(0));
        TEST_ASSERT_EQ(c.corrPlus, static_cast<int64_t>(0));
    }
    return true;
}

/// (u) Four converters fed the same tick, the same current vblank and the same clock value choose identically
///     and correct on the same vblank.
bool test_Cadence_u_FourLayers_OneClockAdvance_IdenticalChoices() {
    TickScope scope;
    CadenceClock clock;
    CadenceInput input(30.0);
    std::vector<std::unique_ptr<FramerateConverterSyncSource>> frcs;
    for (int i = 0; i < 4; ++i) {
        frcs.emplace_back(std::make_unique<FramerateConverterSyncSource>(&clock, &input));
        frcs.back()->setCadenceEventCapture(true);
    }
    const int64_t P = 16666666;
    const double Pms = 1000.0 / 60.0;
    std::mt19937 rng(7);
    std::normal_distribution<double> jitter(0.0, 0.5);
    for (int64_t msc = 100; msc < 100 + 18000; ++msc) {           // 300 s at +100 ppm: ≥ 1 correction
        clock.valueMs = 10000.0 + (msc + 2) * Pms * (1.0 + 100e-6) + jitter(rng);
        publishForTarget(msc, P, true);
        int64_t f0 = frcs[0]->pollFrame(nullptr);
        for (int i = 1; i < 4; ++i) {
            TEST_ASSERT_EQ(frcs[i]->pollFrame(nullptr), f0);
        }
    }
    const auto c0 = frcs[0]->getCadenceCounters();
    TEST_ASSERT(c0.corrMinus >= 1);
    for (int i = 1; i < 4; ++i) {
        TEST_ASSERT_EQ(frcs[i]->getCadenceCounters().corrMinus, c0.corrMinus);
        TEST_ASSERT(frcs[i]->cadenceEvents() == frcs[0]->cadenceEvents());
    }
    return true;
}

// ---- the engine offset's phase (ClickUp 1amfwm7h924) ------------------------------------------------------------------

#include "../sync/EngineOffsetSplit.h"

/// The split is exact: whole frames + the phase on the clock give the frame of the cue's start, for every content
/// rate and every offset; the old code (floor of the offset alone, added to the floored clock frame) is up to one
/// frame late at 30p, never early, and exact at 25p.
bool test_EngineOffset_Split_ExactForEveryRate_OldFloorWasLate() {
    const double rates[] = {24.0, 24000.0 / 1001.0, 25.0, 30000.0 / 1001.0, 30.0, 50.0, 60.0};
    int64_t oldLate30 = 0;
    for (double fps : rates) {
        for (int64_t off = 0; off >= -250; --off) {              // up to 10 s of cue start, in 25 fps MTC frames
            const EngineOffsetSplit sp = splitEngineOffset(off, 25.0, fps);
            TEST_ASSERT(sp.phaseMs >= 0.0);
            TEST_ASSERT(sp.phaseMs < 1000.0 / fps + 1e-9);
            if (fps == 25.0) {
                TEST_ASSERT_EQ(sp.frames, off);
                TEST_ASSERT(sp.phaseMs == 0.0);
            }
            for (int k = 0; k < 400; ++k) {                       // 0..4 s of clock, off the 1 ms grid
                const double clockMs = 20000.0 + k * 10.0 + 0.37;
                const int64_t want = static_cast<int64_t>(std::floor((clockMs + off * 40.0) * fps / 1000.0));
                const int64_t got = static_cast<int64_t>(std::floor((clockMs + sp.phaseMs) * fps / 1000.0)) + sp.frames;
                TEST_ASSERT_EQ(got, want);
                if (fps == 30.0) {
                    const int64_t old = static_cast<int64_t>(std::floor(clockMs * fps / 1000.0)) +
                                        static_cast<int64_t>(std::floor(off * fps / 25.0));
                    TEST_ASSERT(old <= want);                     // late or right, never early
                    TEST_ASSERT(want - old <= 1);
                    oldLate30 += want - old;
                }
            }
        }
    }
    TEST_ASSERT(oldLate30 > 0);                                   // the defect existed: red on the old arithmetic
    TEST_ASSERT_EQ(splitEngineOffset(-5, 25.0, 30.0).frames, static_cast<int64_t>(-6));   // 200 ms = 6 frames at 30p
    TEST_ASSERT(splitEngineOffset(-5, 25.0, 30.0).phaseMs == 0.0);
    TEST_ASSERT_EQ(splitEngineOffset(-1, 25.0, 30.0).frames, static_cast<int64_t>(-2));   // -40 ms = -2 + 26.7 ms
    TEST_ASSERT(std::fabs(splitEngineOffset(-1, 25.0, 30.0).phaseMs - 80.0 / 3.0) < 1e-6);
    TEST_ASSERT_EQ(splitEngineOffset(-7, 0.0, 30.0).frames, static_cast<int64_t>(-7));    // unknown rate: as given
    return true;
}

/// Tick engine: a converter with phase φ chooses exactly the frames a converter with no phase chooses on a clock
/// shifted by φ, through a whole 30p@60 run with 100 ppm drift (so it corrects), with the same corrections and no
/// counted re-anchor.
bool test_Cadence_Phase_EqualsShiftedClock_30at60() {
    TickScope scope;
    CadenceClock clockA, clockB;
    CadenceInput input(30.0);
    FramerateConverterSyncSource a(&clockA, &input), b(&clockB, &input);
    const double phase = 80.0 / 3.0;                              // -1 MTC frame at 30p: the worst case, 26.7 ms
    a.setPhaseMs(phase);
    const int64_t P = 16666666;
    const double Pms = 1000.0 / 60.0;
    for (int64_t msc = 100; msc < 100 + 18000; ++msc) {
        const double t = 10000.0 + (msc + 2) * Pms * (1.0 + 100e-6);
        clockA.valueMs = t;
        clockB.valueMs = t + phase;
        publishForTarget(msc, P, true);
        TEST_ASSERT_EQ(a.pollFrame(nullptr), b.pollFrame(nullptr));
    }
    const auto ca = a.getCadenceCounters(), cb = b.getCadenceCounters();
    TEST_ASSERT(ca.tickPath);
    TEST_ASSERT(ca.corrMinus >= 1);
    TEST_ASSERT_EQ(ca.corrMinus, cb.corrMinus);
    TEST_ASSERT_EQ(ca.corrPlus, cb.corrPlus);
    TEST_ASSERT_EQ(ca.reanchors, static_cast<int64_t>(0));
    return true;
}

/// A new phase while running (a new offset for the layer) re-anchors on the next tick without counting a re-anchor,
/// and from that tick on the frames are exactly those of a converter that ran on the shifted clock all along; a
/// converter with no phase shows other frames (so the test fails if setPhaseMs does nothing). Setting the same phase
/// again changes nothing.
bool test_Cadence_PhaseChange_ReanchorsUncounted() {
    TickScope scope;
    CadenceClock clockA, clockB, clockC;
    CadenceInput input(30.0);
    FramerateConverterSyncSource a(&clockA, &input), b(&clockB, &input), c(&clockC, &input);
    const double phase = 80.0 / 3.0;                              // -1 MTC frame at 30p
    const int64_t P = 16666666;
    const double Pms = 1000.0 / 60.0;
    int differsFromUnphased = 0;
    for (int64_t msc = 100; msc < 1000; ++msc) {
        if (msc == 400) {
            a.setPhaseMs(phase);
            a.setPhaseMs(phase);
        }
        const double t = 10000.0 + (msc + 2) * Pms + 5.0;
        clockA.valueMs = t;
        clockB.valueMs = t + phase;
        clockC.valueMs = t;
        publishForTarget(msc, P, true);
        const int64_t fa = a.pollFrame(nullptr), fb = b.pollFrame(nullptr), fc = c.pollFrame(nullptr);
        if (msc >= 400) {
            TEST_ASSERT_EQ(fa, fb);
            if (fa != fc) ++differsFromUnphased;
        } else {
            TEST_ASSERT_EQ(fa, fc);
        }
    }
    TEST_ASSERT(differsFromUnphased > 0);
    TEST_ASSERT_EQ(a.getCadenceCounters().reanchors, static_cast<int64_t>(0));
    TEST_ASSERT(std::fabs(a.getPhaseMs() - phase) < 1e-9);
    return true;
}

/// Legacy smoother (no tick): the phase moves the frame boundary by the same amount.
bool test_Cadence_Phase_LegacyPath() {
    TickScope scope;
    CadenceClock clock;
    CadenceInput input(30.0);
    FramerateConverterSyncSource frc(&clock, &input);
    clock.valueMs = 2010;                                         // 60.3 frames: 60 without phase
    TEST_ASSERT_EQ(frc.pollFrame(nullptr), static_cast<int64_t>(60));
    frc.setPhaseMs(25.0);                                         // 2035 ms = 61.05 frames
    TEST_ASSERT_EQ(frc.pollFrame(nullptr), static_cast<int64_t>(61));
    TEST_ASSERT_FALSE(frc.getCadenceCounters().tickPath);
    return true;
}
