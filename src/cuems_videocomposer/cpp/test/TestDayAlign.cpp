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

// Day alignment of engine layer offsets (sync/DayAlign.h, LayerPlayback).
// Plan: cuems-RELATIONS Plans/2026-10-25-mtc-day-align-24h-wrap.md, step 6.
// GL-free: the pure helpers, then a VideoLayer with a mock input and a mock
// sync source driven through the real MtcReceiver::toLocalDay().

#include "TestFramework.h"
#include "../sync/DayAlign.h"
#include "../layer/VideoLayer.h"
#include "../input/InputSource.h"
#include "../sync/SyncSource.h"
#include "../video/FrameBuffer.h"
#ifdef HAVE_MTCRECEIVER
#include "../../mtcreceiver/mtcreceiver.h"
#endif
#include <climits>
#include <cstdlib>
#include <memory>
#include <string>

using namespace videocomposer;
using namespace videocomposer::test;
namespace da = videocomposer::dayalign;

namespace {

constexpr long DAY = 86400000L;
constexpr long HOUR = 3600000L;

// The rule of plan §3.1 as a fake resolver over a fixed head (what
// MtcReceiver::toLocalDay does once the head is authoritative).
struct FakeHead {
    long head;
    bool authoritative;
    bool operator()(long engineMs, long& localMs, long& k) const {
        if (!authoritative) return false;
        const long n = engineMs - head + DAY / 2;
        k = n / DAY;
        if (n % DAY < 0) --k;
        localMs = engineMs - k * DAY;
        return true;
    }
};

// Engine offset for a cue start at `startMs` of the ENGINE's day (25 fps).
int64_t offsetFor(long startMs) { return -static_cast<int64_t>(startMs / 40); }

class DayAlignInput : public InputSource {
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
        info.totalFrames = 1000;
        return info;
    }
    bool isReady() const override { return true; }
    int64_t getCurrentFrame() const override { return last_; }
    CodecType detectCodec() const override { return CodecType::SOFTWARE; }
    bool supportsDirectGPUTexture() const override { return false; }
    DecodeBackend getOptimalBackend() const override { return DecodeBackend::CPU_SOFTWARE; }
    int64_t last_ = -1;
};

// A sync source at the MTC head, in 25 fps frames (no framerate conversion).
class DayAlignSync : public SyncSource {
public:
    bool connect(const char* = nullptr) override { return true; }
    void disconnect() override {}
    bool isConnected() const override { return true; }
    int64_t pollFrame(uint8_t* rolling) override {
        if (rolling) *rolling = 1;
        return frame_;
    }
    int64_t getCurrentFrame() const override { return frame_; }
    const char* getName() const override { return "DayAlignSync"; }
    int64_t frame_ = 0;
};

std::unique_ptr<VideoLayer> makeLayer(DayAlignSync*& sync) {
    auto layer = std::make_unique<VideoLayer>();
    layer->setInputSource(std::make_unique<DayAlignInput>());
    auto s = std::make_unique<DayAlignSync>();
    sync = s.get();
    layer->setSyncSource(std::move(s));
    layer->setCueId("dayalign-test-cue_0");
    layer->setMtcFollow(true);
    return layer;
}

} // namespace

// Frame <-> ms and the sign of k: k = 0, +1, -1, +2. The resolved offset in
// frames equals -localMs / 40 exactly (the frame form of the ms rule).
bool test_DayAlign_FrameMsConversionAndSign() {
    TEST_ASSERT_EQ(da::FRAMES_PER_DAY, 2160000);
    TEST_ASSERT_EQ(da::engineRefMs(-15000), 600000);
    TEST_ASSERT_EQ(da::engineRefMs(25), -1000);

    struct Case { long head; long engineStart; long wantK; };
    const Case cases[] = {
        {600000L, 605000L, 0},                  // aligned: unchanged
        {600000L, DAY + 605000L, 1},            // VC born after the engine's crossing
        {DAY + 600000L, 605000L, -1},           // VC alive across midnight, engine reset
        {600000L, 2 * DAY + 605000L, 2},
        {10L, DAY - 10L, 1},                    // wrap race, fresh consumer (plan §3.1)
    };
    for (const Case& c : cases) {
        int64_t frames = 0;
        long k = 99;
        TEST_ASSERT_TRUE(da::resolve(offsetFor(c.engineStart), FakeHead{c.head, true}, frames, k));
        TEST_ASSERT_EQ(k, c.wantK);
        const long localStart = c.engineStart - k * DAY;
        TEST_ASSERT_EQ(frames, -static_cast<int64_t>(localStart / 40));
        TEST_ASSERT_EQ(frames, offsetFor(c.engineStart) + static_cast<int64_t>(k) * da::FRAMES_PER_DAY);
    }
    // Wrap race: the start lands 10 ms before the local day 0, i.e. offset +0.
    int64_t frames = 0;
    long k = 0;
    TEST_ASSERT_TRUE(da::resolve(offsetFor(DAY - 40L), FakeHead{10L, true}, frames, k));
    TEST_ASSERT_EQ(k, 1);
    TEST_ASSERT_EQ(frames, 1);   // start at local -40 ms = frame -1 -> offset +1
    return true;
}

// k = 0 is the received value, byte for byte the old std::atoll result,
// including values far outside any show and the as-received bypass.
bool test_DayAlign_KZeroIsByteIdentical() {
    const char* wires[] = {"0", "-1", "1", "-15000", "-15001", "123456789", "-2147483648",
                           "2147483647", "-2159999", "-1080000", "1080000",
                           "-2160000000000", "9223372036854775807", "-9223372036854775808"};
    for (const char* w : wires) {
        const int64_t legacy = std::atoll(w);              // what rc_1 stored
        // A head that makes every in-range value k = 0: the value's own start.
        const long head = (legacy > da::MAX_ALIGN_FRAMES || legacy < -da::MAX_ALIGN_FRAMES)
                              ? 0L : static_cast<long>(da::engineRefMs(legacy));
        int64_t frames = 12345;
        long k = 99;
        TEST_ASSERT_TRUE(da::resolve(legacy, FakeHead{head, true}, frames, k));
        TEST_ASSERT_EQ(k, 0);
        TEST_ASSERT_EQ(frames, legacy);
        TEST_ASSERT_EQ(da::localOffsetFrames(legacy, 0), legacy);
    }
    return true;
}

// Ties (plan §3.1): exactly +12 h -> k = 1, exactly -12 h -> k = 0.
bool test_DayAlign_TwelveHourTies() {
    int64_t frames = 0;
    long k = 99;
    TEST_ASSERT_TRUE(da::resolve(offsetFor(12 * HOUR), FakeHead{0L, true}, frames, k));
    TEST_ASSERT_EQ(k, 1);
    TEST_ASSERT_EQ(frames, offsetFor(12 * HOUR) + da::FRAMES_PER_DAY);
    TEST_ASSERT_TRUE(da::resolve(offsetFor(-12 * HOUR), FakeHead{0L, true}, frames, k));
    TEST_ASSERT_EQ(k, 0);
    TEST_ASSERT_EQ(frames, offsetFor(-12 * HOUR));
    // One frame inside either edge stays in the head's day.
    TEST_ASSERT_TRUE(da::resolve(offsetFor(12 * HOUR - 40), FakeHead{0L, true}, frames, k));
    TEST_ASSERT_EQ(k, 0);
    TEST_ASSERT_TRUE(da::resolve(offsetFor(-12 * HOUR - 40), FakeHead{0L, true}, frames, k));
    TEST_ASSERT_EQ(k, -1);
    return true;
}

// The latch: never resolved without an authoritative head, latest value wins,
// any other writer clears it, the position check fires once per resolution.
bool test_DayAlign_LatchPendingLatestWinsAndClear() {
    da::EngineOffsetLatch latch;
    TEST_ASSERT_TRUE(latch.tryResolve(FakeHead{0L, true}).status == da::EngineOffsetLatch::Status::None);

    latch.latch(offsetFor(DAY + 605000L), 0, "L");
    auto o = latch.tryResolve(FakeHead{600000L, false});
    TEST_ASSERT_TRUE(o.status == da::EngineOffsetLatch::Status::Pending);
    TEST_ASSERT_TRUE(latch.pending());
    TEST_ASSERT_FALSE(latch.takePositionCheck());

    latch.latch(offsetFor(DAY + 700000L), 10, "L");      // a newer value replaces it
    TEST_ASSERT_EQ(latch.raw(), offsetFor(DAY + 700000L));
    o = latch.tryResolve(FakeHead{600000L, true});
    TEST_ASSERT_TRUE(o.status == da::EngineOffsetLatch::Status::Resolved);
    TEST_ASSERT_EQ(o.k, 1);
    TEST_ASSERT_EQ(o.frames, offsetFor(700000L));
    TEST_ASSERT_FALSE(latch.pending());
    TEST_ASSERT_TRUE(latch.takePositionCheck());
    TEST_ASSERT_FALSE(latch.takePositionCheck());        // once per resolution

    latch.latch(offsetFor(605000L), 20, "L");
    latch.clear();                                        // setTimeOffset from any other writer
    TEST_ASSERT_FALSE(latch.pending());
    TEST_ASSERT_TRUE(latch.tryResolve(FakeHead{600000L, true}).status == da::EngineOffsetLatch::Status::None);
    return true;
}

// Unresolved WARNING: > 2 s with MTC running, once per latched value, re-armed
// by the next latch (a loop pass re-sends the offset).
bool test_DayAlign_UnresolvedWarningOncePerValue() {
    da::EngineOffsetLatch l2;
    int64_t age = 0;
    l2.latch(-15000, 1000, "L");
    TEST_ASSERT_FALSE(l2.unresolvedWarningDue(2900, true, age));       // 1.9 s
    TEST_ASSERT_FALSE(l2.unresolvedWarningDue(3500, false, age));      // MTC not running
    TEST_ASSERT_TRUE(l2.unresolvedWarningDue(3500, true, age));
    TEST_ASSERT_EQ(age, 2500);
    TEST_ASSERT_FALSE(l2.unresolvedWarningDue(9000, true, age));       // once
    l2.latch(-15025, 9000, "L");                                        // re-armed
    TEST_ASSERT_FALSE(l2.unresolvedWarningDue(10000, true, age));
    TEST_ASSERT_TRUE(l2.unresolvedWarningDue(11001, true, age));
    l2.tryResolve(FakeHead{0L, true});
    TEST_ASSERT_FALSE(l2.unresolvedWarningDue(20000, true, age));      // resolved: never
    return true;
}

// Implausible position: an hour before the media start or past its end.
bool test_DayAlign_ImplausiblePosition() {
    const int64_t hour25 = 25 * 3600;
    TEST_ASSERT_FALSE(da::implausiblePosition(-125, 1000, 25.0));           // run at start - 5 s
    TEST_ASSERT_FALSE(da::implausiblePosition(-hour25, 1000, 25.0));
    TEST_ASSERT_TRUE(da::implausiblePosition(-hour25 - 1, 1000, 25.0));
    TEST_ASSERT_FALSE(da::implausiblePosition(1000 + hour25, 1000, 25.0));
    TEST_ASSERT_TRUE(da::implausiblePosition(1000 + hour25 + 1, 1000, 25.0));
    TEST_ASSERT_TRUE(da::implausiblePosition(-2160000, 1000, 0.0));         // a day off, unknown fps
    TEST_ASSERT_TRUE(da::implausiblePosition(-216001, 1000, 60.0));         // 60 fps: hour = 216000
    TEST_ASSERT_FALSE(da::implausiblePosition(-216000, 1000, 60.0));
    return true;
}

// The k line: once per process per k value, every distinct k (several layers
// with different k in one pass), INFO for k >= 1 and for k < 0 past a wrap of
// our own, WARNING for k < 0 with the head in day 0. The 25 fps WARNING once.
bool test_DayAlign_LogLines() {
    da::RealignLog log;
    std::string line;
    bool warning = true;
    TEST_ASSERT_FALSE(log.onResolved(0, 605000, 600000, line, warning));
    TEST_ASSERT_TRUE(log.onResolved(1, 87005000, 600000, line, warning));
    TEST_ASSERT_EQ(line, std::string("MTC day realign k=1 (engine 87005000 ms, local head 600000 ms,"
                                     " phase=ext); 1 conversions"));
    TEST_ASSERT_FALSE(warning);
    TEST_ASSERT_TRUE(log.onResolved(2, 173405000, 600000, line, warning));      // same pass, other k
    TEST_ASSERT_EQ(line, std::string("MTC day realign k=2 (engine 173405000 ms, local head 600000 ms,"
                                     " phase=ext); 2 conversions"));
    TEST_ASSERT_FALSE(log.onResolved(1, 87005000, 600000, line, warning));      // k = 1 again: silent
    TEST_ASSERT_EQ(log.conversions(), 3UL);
    TEST_ASSERT_TRUE(log.onResolved(-1, 605000, 600000, line, warning));
    TEST_ASSERT_TRUE(warning);                                                  // head in day 0
    da::RealignLog later;
    TEST_ASSERT_TRUE(later.onResolved(-1, 605000, DAY + 600000, line, warning));
    TEST_ASSERT_FALSE(warning);                                                 // crossed a wrap itself

    TEST_ASSERT_FALSE(log.onFrameRate(25, line));
    TEST_ASSERT_TRUE(log.onFrameRate(29, line));
    TEST_ASSERT_EQ(line, std::string("MTC day-align: MTC frame rate is 29 fps, not the 25 fps of the"
                                     " CUEMS contract; engine offsets are still read as 25 fps frames"
                                     " (2160000 per day)"));
    TEST_ASSERT_FALSE(log.onFrameRate(30, line));
    TEST_ASSERT_EQ(std::string(da::banner()), std::string("MTC day-align: on (12 h rule, ext)"));
    return true;
}

#if defined(HAVE_MTCRECEIVER) && defined(MTCRECV_TESTING)
namespace {
// Pretend an mtcreceiver driver is open (LayerPlayback otherwise applies the
// value as received) and leave the receiver statics clean afterwards.
struct ReceiverScope {
    int saved;
    ReceiverScope() : saved(da::mtcReceiverDrivers().exchange(1)) { MtcReceiver::resetStaticStateForTesting(); }
    ~ReceiverScope() {
        MtcReceiver::resetStaticStateForTesting();
        da::mtcReceiverDrivers().store(saved);
    }
};
} // namespace

// The real resolver: nothing before the head is authoritative, k from the
// real dayShift once it is, and a resetWrapOffset() drops it again.
bool test_DayAlign_RealResolverWaitsForAuthoritativeHead() {
    ReceiverScope scope;
    da::EngineOffsetLatch latch;
    MtcReceiver::mtcHead.store(600000);                  // 00:10:00, day 0
    latch.latch(offsetFor(DAY + 605000L), 0, "L");
    auto o = latch.tryResolve(MtcReceiver::toLocalDay);
    TEST_ASSERT_TRUE(o.status == da::EngineOffsetLatch::Status::Pending);

    MtcReceiver::setHeadAuthoritativeForTesting(true);
    o = latch.tryResolve(MtcReceiver::toLocalDay);
    TEST_ASSERT_TRUE(o.status == da::EngineOffsetLatch::Status::Resolved);
    TEST_ASSERT_EQ(o.k, 1);
    TEST_ASSERT_EQ(o.frames, offsetFor(605000L));

    MtcReceiver::resetWrapOffset();                       // project reset: flag down
    latch.latch(offsetFor(605000L), 0, "L");
    TEST_ASSERT_TRUE(latch.tryResolve(MtcReceiver::toLocalDay).status == da::EngineOffsetLatch::Status::Pending);
    return true;
}

// THE KEY TEST. A videocomposer restarted after the engine passed 24 h: its
// head is 00:10:00 of day 0, the engine says the cue starts at 24:10:00 of
// its day. The layer must show media frame 100 when MTC is 100 frames past
// the start. rc_1 stored the raw value (15100 - 2175000 < 0 -> clamped to
// frame 0: black / frozen first frame), which fails the frame-100 assertion.
bool test_DayAlign_LayerAfterRestartShowsTheRightFrame() {
    ReceiverScope scope;
    DayAlignSync* sync = nullptr;
    auto layer = makeLayer(sync);
    MtcReceiver::mtcHead.store(600000);
    sync->frame_ = 600000 / 40 + 100;

    layer->setEngineTimeOffset(offsetFor(DAY + 600000L));
    // Not authoritative yet: fail closed, the layer does not start (no frame,
    // not even the frame-0 fallback) and the raw value is never applied.
    layer->update();
    TEST_ASSERT_EQ(layer->getCurrentFrame(), -1);
    TEST_ASSERT_TRUE(layer->hasPendingEngineOffset());

    MtcReceiver::setHeadAuthoritativeForTesting(true);
    layer->update();
    TEST_ASSERT_EQ(layer->getCurrentFrame(), 100);
    TEST_ASSERT_FALSE(layer->hasPendingEngineOffset());
    TEST_ASSERT_EQ(layer->getTimeOffset(), offsetFor(600000L));
    return true;
}

// The other direction: the videocomposer crossed midnight, the node engine
// restarted (day 0). rc_1: 2175100 - 15000 -> past the end, held on the last frame.
bool test_DayAlign_LayerAheadOfEngineShowsTheRightFrame() {
    ReceiverScope scope;
    DayAlignSync* sync = nullptr;
    auto layer = makeLayer(sync);
    MtcReceiver::mtcHead.store(DAY + 600000L);
    MtcReceiver::setHeadAuthoritativeForTesting(true);
    sync->frame_ = (DAY + 600000L) / 40 + 100;

    layer->setEngineTimeOffset(offsetFor(600000L));
    layer->update();
    TEST_ASSERT_EQ(layer->getCurrentFrame(), 100);
    TEST_ASSERT_EQ(layer->getTimeOffset(), offsetFor(600000L) - da::FRAMES_PER_DAY);
    return true;
}

// Aligned (every working case today): the value applied is the received one,
// in the same update, and the layer plays exactly as before.
bool test_DayAlign_LayerAlignedIsUnchanged() {
    ReceiverScope scope;
    DayAlignSync* sync = nullptr;
    auto layer = makeLayer(sync);
    MtcReceiver::mtcHead.store(600000);
    MtcReceiver::setHeadAuthoritativeForTesting(true);
    sync->frame_ = 600000 / 40 + 100;

    layer->setEngineTimeOffset(-15000);
    layer->update();
    TEST_ASSERT_EQ(layer->getTimeOffset(), -15000);
    TEST_ASSERT_EQ(layer->getCurrentFrame(), 100);
    return true;
}

// A later setTimeOffset (another writer) replaces the pending engine value:
// the engine value is never applied afterwards.
bool test_DayAlign_LaterWriterReplacesPending() {
    ReceiverScope scope;
    DayAlignSync* sync = nullptr;
    auto layer = makeLayer(sync);
    MtcReceiver::mtcHead.store(600000);
    sync->frame_ = 600000 / 40 + 100;

    layer->setEngineTimeOffset(offsetFor(DAY + 600000L));
    layer->update();
    TEST_ASSERT_TRUE(layer->hasPendingEngineOffset());
    layer->setTimeOffset(-14950);                       // e.g. /timescale2, SMPTE /offset
    TEST_ASSERT_FALSE(layer->hasPendingEngineOffset());
    MtcReceiver::setHeadAuthoritativeForTesting(true);
    layer->update();
    TEST_ASSERT_EQ(layer->getTimeOffset(), -14950);
    TEST_ASSERT_EQ(layer->getCurrentFrame(), 150);
    return true;
}

// No mtcreceiver driver open (MIDI fell back to another driver): nothing will
// ever make the head authoritative, so the value is applied as received.
bool test_DayAlign_NoReceiverDriverAppliesAsReceived() {
    ReceiverScope scope;
    da::mtcReceiverDrivers().store(0);
    DayAlignSync* sync = nullptr;
    auto layer = makeLayer(sync);
    sync->frame_ = 15100;
    layer->setEngineTimeOffset(-15000);
    layer->update();
    TEST_ASSERT_FALSE(layer->hasPendingEngineOffset());
    TEST_ASSERT_EQ(layer->getTimeOffset(), -15000);
    TEST_ASSERT_EQ(layer->getCurrentFrame(), 100);
    return true;
}
#endif
