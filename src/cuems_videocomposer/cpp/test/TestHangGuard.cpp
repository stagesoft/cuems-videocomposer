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

/**
 * TestHangGuard.cpp - the cap arithmetic and the two counters behind it (G12)
 *
 * The integration proof for this feature is a rig drill: a fifth 4K layer
 * revealing while four follow, refused, with the project still running. What
 * cannot be proven there - because a hang costs a reboot and sometimes a
 * physical power cycle - is the arithmetic in every corner it will meet in a
 * show. That is what these pin.
 *
 * Three properties matter more than the rest, and each has a specific failure
 * behind it:
 *
 *  - **Fail closed.** A 4K clip whose header does not parse must not weigh
 *    zero and sail past the cap. A guard that fails silently in the direction
 *    of the hang is worse than no guard.
 *  - **Idempotency, both counters.** Release runs from more than one owner:
 *    the ledger from /mtcfollow 0 and from layer teardown, the armed count
 *    from close() on three different threads. A counter that can be
 *    decremented twice goes negative and silently shrinks the cap for the
 *    rest of the process's life.
 *  - **Re-arm is net-zero.** The reservation belongs to the layer, so
 *    re-loading a cue over a loaded one must never consume a second slot -
 *    a refused re-load would leave an output showing the previous cue's
 *    media.
 */

#include "guard/HangGuard.h"
#include "TestFramework.h"

#include <chrono>
#include <thread>

using namespace videocomposer;

namespace {

/** A guard armed exactly like the one profile that has a measured boundary. */
HangGuard armedFp530() {
    MachineProfile p;
    p.name = "fp530-8gb";
    p.cap = 4;
    p.armed = true;
    p.detail = "test";
    return HangGuard(p);
}

/** A box with no measured boundary: counts everything, refuses nothing. */
HangGuard monitorOnly() {
    MachineProfile p;   // defaults: cap 0, not armed
    return HangGuard(p);
}

constexpr int UHD_W = 3840, UHD_H = 2160;
constexpr int HD_W = 1920, HD_H = 1080;

} // namespace

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

// 1920x1080 is the exempt ceiling, not the first 4K-class size. Getting the
// boundary off by one pixel either exempts 4K content or charges a full slot
// for the HD content the campaign measured 12 of, cleanly, at once.
bool test_HangGuard_HdIsExemptAtTheBoundary() {
    const Classification hd = HangGuard::classify(HD_W, HD_H, 25.0);
    TEST_ASSERT(hd.sessionClass == SessionClass::exempt);
    TEST_ASSERT_FALSE(hd.failedClosed);

    // One pixel more is 4K-class.
    const Classification over = HangGuard::classify(HD_W + 1, HD_H, 25.0);
    TEST_ASSERT(over.sessionClass == SessionClass::fourKClass);
    return true;
}

bool test_HangGuard_UhdAndDciAreFourKClass() {
    TEST_ASSERT(HangGuard::classify(UHD_W, UHD_H, 25.0).isFourKClass());
    // DCI 4K, which is not 3840 wide and must not fall through a == test.
    TEST_ASSERT(HangGuard::classify(4096, 2160, 25.0).isFourKClass());
    // The unnamed middle band: neither exempt nor ever measured. It costs a
    // full slot deliberately - the honest treatment of the unmeasured is to
    // charge for it, not to guess it is cheap.
    TEST_ASSERT(HangGuard::classify(2560, 1440, 25.0).isFourKClass());
    TEST_ASSERT(HangGuard::classify(3440, 1440, 25.0).isFourKClass());
    return true;
}

// The fail-closed rule, in both of its forms.
bool test_HangGuard_MissingMetadataFailsClosed() {
    const Classification noDims = HangGuard::classify(0, 0, 25.0);
    TEST_ASSERT(noDims.isFourKClass());
    TEST_ASSERT_TRUE(noDims.failedClosed);

    // r_frame_rate has no fallback in the loader, so an unreadable rate is a
    // real case and not a hypothetical. Even at HD dimensions it counts.
    const Classification noFps = HangGuard::classify(HD_W, HD_H, 0.0);
    TEST_ASSERT(noFps.isFourKClass());
    TEST_ASSERT_TRUE(noFps.failedClosed);
    return true;
}

// Above 25 fps is outside every measured cell, and is admitted anyway: the fps
// ladder saturated at 2x4K60, 4x4K50 and 4x4K60 without ever hanging, so
// refusing it would refuse the unmeasured.
bool test_HangGuard_AboveEnvelopeIsFlaggedNotRefused() {
    const Classification fast = HangGuard::classify(UHD_W, UHD_H, 60.0);
    TEST_ASSERT(fast.isFourKClass());
    TEST_ASSERT_TRUE(fast.outsideEnvelope);

    const Classification measured = HangGuard::classify(UHD_W, UHD_H, 25.0);
    TEST_ASSERT_FALSE(measured.outsideEnvelope);

    // An exempt session is never "outside the envelope" - that phrase is only
    // about 4K-class content, and saying it of HD would train operators to
    // ignore it.
    TEST_ASSERT_FALSE(HangGuard::classify(HD_W, HD_H, 60.0).outsideEnvelope);
    return true;
}

// ---------------------------------------------------------------------------
// The cap
// ---------------------------------------------------------------------------

bool test_HangGuard_AdmitsUpToCapThenRefuses() {
    HangGuard g = armedFp530();
    for (int i = 1; i <= 4; ++i) {
        const auto d = g.requestReveal(i, SessionClass::fourKClass, "cue");
        TEST_ASSERT_TRUE(d.admitted);
        TEST_ASSERT_EQ(d.activeFourK, i);
    }
    // The fifth is the measured hang region.
    const auto fifth = g.requestReveal(5, SessionClass::fourKClass, "cue5");
    TEST_ASSERT_FALSE(fifth.admitted);
    TEST_ASSERT_FALSE(fifth.reason.empty());
    TEST_ASSERT_EQ(g.activeFourK(), 4);
    TEST_ASSERT_EQ(g.refusals(), 1L);

    // A refusal must not be a latch: freeing a slot admits the next request.
    g.releaseReveal(1);
    TEST_ASSERT_EQ(g.activeFourK(), 3);
    TEST_ASSERT_TRUE(g.requestReveal(5, SessionClass::fourKClass, "cue5").admitted);
    return true;
}

// HD is never refused at any count. Twelve concurrent HD sessions were
// measured clean; the ceiling is a warning, and warnings do not refuse.
bool test_HangGuard_ExemptSessionsAreNeverRefused() {
    HangGuard g = armedFp530();
    for (int i = 1; i <= 20; ++i) {
        TEST_ASSERT_TRUE(g.requestReveal(i, SessionClass::exempt, "hd").admitted);
    }
    TEST_ASSERT_EQ(g.activeExempt(), 20);
    TEST_ASSERT_EQ(g.refusals(), 0L);
    // And they do not consume 4K slots.
    TEST_ASSERT_TRUE(g.requestReveal(100, SessionClass::fourKClass, "uhd").admitted);
    return true;
}

// Monitor-only profiles count everything and refuse nothing: an unmeasured box
// gets the instrument, never the limit.
bool test_HangGuard_MonitorOnlyProfileNeverRefuses() {
    HangGuard g = monitorOnly();
    for (int i = 1; i <= 8; ++i) {
        TEST_ASSERT_TRUE(g.requestReveal(i, SessionClass::fourKClass, "cue").admitted);
    }
    TEST_ASSERT_EQ(g.activeFourK(), 8);
    TEST_ASSERT_EQ(g.refusals(), 0L);
    TEST_ASSERT_FALSE(g.willRefuse());
    return true;
}

// --hang-guard=off is mandatory: without it the capacity harness could never
// measure a boundary beyond the current cap. It stops the refusal and nothing
// else - the counts must stay true or the measurement is worthless.
bool test_HangGuard_DisarmStopsRefusalButKeepsCounting() {
    HangGuard g = armedFp530();
    g.disarm("test");
    TEST_ASSERT_FALSE(g.willRefuse());
    for (int i = 1; i <= 7; ++i) {
        TEST_ASSERT_TRUE(g.requestReveal(i, SessionClass::fourKClass, "cue").admitted);
    }
    TEST_ASSERT_EQ(g.activeFourK(), 7);
    TEST_ASSERT_EQ(g.refusals(), 0L);
    return true;
}

// ---------------------------------------------------------------------------
// Idempotency - the property that keeps the cap from eroding
// ---------------------------------------------------------------------------

// The engine re-sends /mtcfollow at reveal as belt-and-suspenders, so a second
// request for a layer already decoding is the normal path. It must not consume
// a second slot.
bool test_HangGuard_RepeatedRevealDoesNotDoubleCount() {
    HangGuard g = armedFp530();
    TEST_ASSERT_TRUE(g.requestReveal(1, SessionClass::fourKClass, "cue").admitted);
    TEST_ASSERT_TRUE(g.requestReveal(1, SessionClass::fourKClass, "cue").admitted);
    TEST_ASSERT_TRUE(g.requestReveal(1, SessionClass::fourKClass, "cue").admitted);
    TEST_ASSERT_EQ(g.activeFourK(), 1);
    return true;
}

// Release has two owners - the mtcfollow-0 path and layer teardown - so it is
// reached twice for the same slot as a matter of course.
bool test_HangGuard_ReleaseIsIdempotentAndSafeWhenUnreserved() {
    HangGuard g = armedFp530();
    g.requestReveal(1, SessionClass::fourKClass, "cue");
    TEST_ASSERT_EQ(g.activeFourK(), 1);

    g.releaseReveal(1);
    g.releaseReveal(1);
    g.releaseReveal(1);
    TEST_ASSERT_EQ(g.activeFourK(), 0);

    // A layer that never reserved (never revealed, or refused) still runs the
    // teardown backstop.
    g.releaseReveal(999);
    TEST_ASSERT_EQ(g.activeFourK(), 0);

    // The cap is intact afterwards: this is the assertion that would fail if
    // any of the above had driven the counter negative.
    for (int i = 1; i <= 4; ++i) {
        TEST_ASSERT_TRUE(g.requestReveal(i, SessionClass::fourKClass, "cue").admitted);
    }
    TEST_ASSERT_FALSE(g.requestReveal(5, SessionClass::fourKClass, "cue").admitted);
    return true;
}

// A refused reveal must not leave a phantom reservation behind: the layer is
// not decoding, so it holds nothing, and a later attempt must be able to
// succeed on its own merits.
bool test_HangGuard_RefusedRevealReservesNothing() {
    HangGuard g = armedFp530();
    for (int i = 1; i <= 4; ++i) {
        g.requestReveal(i, SessionClass::fourKClass, "cue");
    }
    TEST_ASSERT_FALSE(g.requestReveal(5, SessionClass::fourKClass, "cue").admitted);

    // Releasing the layer that was refused changes nothing.
    g.releaseReveal(5);
    TEST_ASSERT_EQ(g.activeFourK(), 4);

    g.releaseReveal(4);
    TEST_ASSERT_EQ(g.activeFourK(), 3);
    return true;
}

// ---------------------------------------------------------------------------
// armed4kCount - the advisory's operand
// ---------------------------------------------------------------------------

bool test_HangGuard_ArmedCountCountsOnceAndReleasesOnce() {
    HangGuard g = armedFp530();
    const Classification uhd = HangGuard::classify(UHD_W, UHD_H, 25.0);

    g.noteLoadClassified(uhd);
    g.noteLoadClassified(uhd);
    TEST_ASSERT_EQ(g.armedFourK(), 2);

    g.noteLoadReleased(true);
    TEST_ASSERT_EQ(g.armedFourK(), 1);
    g.noteLoadReleased(true);
    TEST_ASSERT_EQ(g.armedFourK(), 0);
    return true;
}

// Exempt loads are not exposure and must not move the counter - otherwise a
// clean 12xHD show would sit permanently over the cap and paint the operator's
// load monitor amber for content that was measured clean.
bool test_HangGuard_ArmedCountIgnoresExemptLoads() {
    HangGuard g = armedFp530();
    const Classification hd = HangGuard::classify(HD_W, HD_H, 25.0);
    for (int i = 0; i < 12; ++i) {
        g.noteLoadClassified(hd);
    }
    TEST_ASSERT_EQ(g.armedFourK(), 0);

    // And the paired release is equally a no-op.
    g.noteLoadReleased(false);
    TEST_ASSERT_EQ(g.armedFourK(), 0);
    return true;
}

// The armed count is exposure, not admission: it never refuses anything, at
// any value. Arming eight 4K layers is a measured-harmless state (8 held
// sessions soaked flat for 15 minutes), and only revealing them is capped.
bool test_HangGuard_ArmedCountOverCapDoesNotRefuse() {
    HangGuard g = armedFp530();
    const Classification uhd = HangGuard::classify(UHD_W, UHD_H, 25.0);
    for (int i = 0; i < 8; ++i) {
        g.noteLoadClassified(uhd);
    }
    TEST_ASSERT_EQ(g.armedFourK(), 8);
    TEST_ASSERT_EQ(g.refusals(), 0L);

    MonitorState s;
    g.fillMonitorState(s);
    TEST_ASSERT_TRUE(s.advisoryLatched);   // the exposure is on record
    TEST_ASSERT_EQ(s.activeFourK, 0);      // and nothing is decoding

    // Four of them may still reveal.
    for (int i = 1; i <= 4; ++i) {
        TEST_ASSERT_TRUE(g.requestReveal(i, SessionClass::fourKClass, "cue").admitted);
    }
    TEST_ASSERT_FALSE(g.requestReveal(5, SessionClass::fourKClass, "cue").admitted);
    return true;
}

// The advisory is a latched state and not only an event, so a UI that polls
// cannot miss it between samples; and it clears on the way back down.
bool test_HangGuard_AdvisoryLatchTracksTheCrossing() {
    HangGuard g = armedFp530();
    const Classification uhd = HangGuard::classify(UHD_W, UHD_H, 25.0);

    MonitorState s;
    for (int i = 0; i < 4; ++i) g.noteLoadClassified(uhd);
    g.fillMonitorState(s);
    TEST_ASSERT_FALSE(s.advisoryLatched);   // at the cap is not over it

    g.noteLoadClassified(uhd);              // the crossing
    g.fillMonitorState(s);
    TEST_ASSERT_TRUE(s.advisoryLatched);

    g.noteLoadReleased(true);
    g.fillMonitorState(s);
    TEST_ASSERT_FALSE(s.advisoryLatched);
    return true;
}

// A monitor-only profile has no cap to cross, so it never latches an advisory
// and never promises a refusal it could not perform.
bool test_HangGuard_NoAdvisoryWithoutACap() {
    HangGuard g = monitorOnly();
    const Classification uhd = HangGuard::classify(UHD_W, UHD_H, 25.0);
    for (int i = 0; i < 8; ++i) g.noteLoadClassified(uhd);

    MonitorState s;
    g.fillMonitorState(s);
    TEST_ASSERT_EQ(s.armedFourK, 8);
    TEST_ASSERT_FALSE(s.advisoryLatched);
    return true;
}

// ---------------------------------------------------------------------------
// The F9 snapshot
// ---------------------------------------------------------------------------

// The traffic light is a pure function of these fields, so each input it
// consumes has to be present and distinct. Collapsing armed and active into
// one "session count" is the specific mistake this pins against.
bool test_HangGuard_MonitorStateCarriesBothCountsSeparately() {
    HangGuard g = armedFp530();
    const Classification uhd = HangGuard::classify(UHD_W, UHD_H, 25.0);
    for (int i = 0; i < 6; ++i) g.noteLoadClassified(uhd);
    g.requestReveal(1, SessionClass::fourKClass, "cue");
    g.requestReveal(2, SessionClass::fourKClass, "cue");
    g.requestReveal(3, SessionClass::exempt, "hd");

    MonitorState s;
    g.fillMonitorState(s);
    TEST_ASSERT_EQ(s.armedFourK, 6);
    TEST_ASSERT_EQ(s.activeFourK, 2);
    TEST_ASSERT_EQ(s.activeExempt, 1);
    TEST_ASSERT_EQ(s.cap, 4);
    TEST_ASSERT_TRUE(s.guardArmed);
    TEST_ASSERT(s.profile == "fp530-8gb");
    return true;
}

// ---------------------------------------------------------------------------
// Profiles
// ---------------------------------------------------------------------------

// Only a profile with a measurement behind it may carry a cap. This is the
// test that fails if someone ever "helpfully" gives the unknown profile a
// default limit.
bool test_HangGuard_OnlyMeasuredProfilesAreArmed() {
    MachineProfile p;
    TEST_ASSERT_TRUE(MachineProfile::byName("fp530-8gb", p));
    TEST_ASSERT_EQ(p.cap, 4);
    TEST_ASSERT_TRUE(p.armed);

    for (const char* name : {"fp530-16gb", "4ktop-780m", "intel-legacy", "unknown"}) {
        MachineProfile m;
        TEST_ASSERT_TRUE(MachineProfile::byName(name, m));
        TEST_ASSERT_EQ(m.cap, 0);
        TEST_ASSERT_FALSE(m.armed);
    }

    // An unknown name is an error, never a silent fallback to something safe
    // and wrong.
    MachineProfile bogus;
    TEST_ASSERT_FALSE(MachineProfile::byName("fp530-8g", bogus));
    return true;
}

// Autodetection must always produce something usable, including on a box with
// no GPU at all - and that something is monitor-only.
bool test_HangGuard_DetectAlwaysYieldsAUsableProfile() {
    const MachineProfile p = MachineProfile::detect();
    TEST_ASSERT_FALSE(p.name.empty());
    TEST_ASSERT_FALSE(p.detail.empty());
    TEST_ASSERT(p.cap >= 0);
    if (p.cap == 0) {
        TEST_ASSERT_FALSE(p.armed);   // no cap means nothing to enforce
    }
    return true;
}

// ---------------------------------------------------------------------------
// The advisory log converges on the truth
// ---------------------------------------------------------------------------

// The rate limit that stops a cue chain from filling the journal must defer a
// transition, never drop it. Dropping a *clear* is the bad one: the log would
// keep asserting an exposure that has already gone, which is precisely the
// class of quiet lie - a channel that reads fine when it is not - that this
// design exists to avoid.
bool test_HangGuard_DeferredAdvisoryClearIsNotLost() {
    HangGuard g = armedFp530();
    g.setAdvisoryLogIntervalForTest(std::chrono::milliseconds(50));
    const Classification uhd = HangGuard::classify(UHD_W, UHD_H, 25.0);

    for (int i = 0; i < 5; ++i) g.noteLoadClassified(uhd);   // crosses, logs
    TEST_ASSERT_FALSE(g.advisoryLogPending());

    // Straight back down, inside the window: the state is right, the journal
    // is knowingly behind.
    g.noteLoadReleased(true);
    TEST_ASSERT_TRUE(g.advisoryLogPending());

    // ...and the monitor's tick closes it once the window has passed.
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    g.pollAdvisoryLog();
    TEST_ASSERT_FALSE(g.advisoryLogPending());
    return true;
}

// A tick that has nothing to say must say nothing: reconciling against state
// rather than events is what keeps the quiet path quiet.
bool test_HangGuard_PollIsANoOpWhenTheLogIsCurrent() {
    HangGuard g = armedFp530();
    g.setAdvisoryLogIntervalForTest(std::chrono::milliseconds(1));
    for (int i = 0; i < 20; ++i) {
        g.pollAdvisoryLog();
        TEST_ASSERT_FALSE(g.advisoryLogPending());
    }
    return true;
}

// ---------------------------------------------------------------------------
// Shared decode sessions
// ---------------------------------------------------------------------------

// Promotion moves a slot, it does not mint one. When the layer driving a
// shared decode session is removed, a surviving secondary inherits the very
// same session - so the count must not change, and the inheriting layer must
// not be subject to a cap check it could fail while already decoding.
bool test_HangGuard_TransferMovesTheSlotWithoutChangingCounts() {
    HangGuard g = armedFp530();
    for (int i = 1; i <= 4; ++i) {
        g.requestReveal(i, SessionClass::fourKClass, "cue");
    }
    TEST_ASSERT_EQ(g.activeFourK(), 4);

    // At the cap, and a driver hands its session to a secondary.
    g.transferReveal(2, 42);
    TEST_ASSERT_EQ(g.activeFourK(), 4);   // moved, not added
    TEST_ASSERT_EQ(g.refusals(), 0L);     // and never refused

    // The old owner holds nothing now...
    g.releaseReveal(2);
    TEST_ASSERT_EQ(g.activeFourK(), 4);
    // ...and releasing the new owner frees the one slot.
    g.releaseReveal(42);
    TEST_ASSERT_EQ(g.activeFourK(), 3);
    return true;
}

// Transferring from a layer that held nothing must not invent a reservation.
bool test_HangGuard_TransferFromUnreservedIsANoOp() {
    HangGuard g = armedFp530();
    g.transferReveal(7, 8);
    TEST_ASSERT_EQ(g.activeFourK(), 0);
    g.releaseReveal(8);
    TEST_ASSERT_EQ(g.activeFourK(), 0);
    return true;
}
