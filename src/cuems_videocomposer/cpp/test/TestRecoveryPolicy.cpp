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
 * TestRecoveryPolicy.cpp - the terminating recovery policy (869en65tm, G3)
 *
 * The defect this policy replaces was not a crash: it was a loop that never
 * ended. Recovery counted attempts *within one wake*, and attempt 1 - a
 * full-pool reopen - always succeeds for the only fault class that reaches
 * recovery, so a damaged file cycled reopen -> ~28 good frames -> unhealthy
 * forever, rebuilding a VAAPI context each time. `declared_failed` was
 * unreachable.
 *
 * So the property under test is termination, and its exact opposite: that a
 * layer which genuinely comes back is not punished for having once failed.
 * The drill on real hardware is the integration proof; this pins the
 * arithmetic, which is where the original bug actually lived.
 */

#include "input/RecoveryPolicy.h"
#include "TestFramework.h"

using namespace videocomposer;

namespace {

// A limping layer's per-episode yield: ~28 frames, measured on the FP530.
// Far below the decay threshold, which is the point.
constexpr long long LIMP_YIELD = 28;

}  // namespace

// The failure that started all of this: sub-threshold yields must accumulate,
// not reset. Three limping wakes exhaust the run.
bool test_RecoveryPolicy_LimpingYieldsAccumulateToCap() {
    RecoveryPolicy p;
    for (int i = 1; i <= RecoveryPolicy::RECOVERY_EPISODE_CAP; ++i) {
        const RecoveryPolicy::Decision d1 = p.onWake(LIMP_YIELD);
        TEST_ASSERT(d1 == RecoveryPolicy::Decision::attempt);
        TEST_ASSERT_EQ(p.episodeNumber(), i);
    }
    // The next wake is the declaration.
    const RecoveryPolicy::Decision d2 = p.onWake(LIMP_YIELD);
    TEST_ASSERT(d2 == RecoveryPolicy::Decision::declare);
    return true;
}

// A layer that actually recovers earns a fresh ladder. Without this the policy
// would kill a healthy layer on its second unrelated fault hours later.
bool test_RecoveryPolicy_GoodRunResetsTheLadder() {
    RecoveryPolicy p;
    p.onWake(LIMP_YIELD);
    p.onWake(LIMP_YIELD);
    TEST_ASSERT_EQ(p.episodeNumber(), 2);

    // A yield at the threshold clears the run, and the wake still gets its
    // episode (the queue is unhealthy now, whatever it did before).
    const RecoveryPolicy::Decision d3 = p.onWake(RecoveryPolicy::RECOVERY_DECAY_GOOD_FRAMES);
    TEST_ASSERT(d3 == RecoveryPolicy::Decision::attempt);
    TEST_ASSERT_EQ(p.episodeNumber(), 1);

    // And the full ladder is available again from there.
    const RecoveryPolicy::Decision d4 = p.onWake(LIMP_YIELD);
    TEST_ASSERT(d4 == RecoveryPolicy::Decision::attempt);
    const RecoveryPolicy::Decision d5 = p.onWake(LIMP_YIELD);
    TEST_ASSERT(d5 == RecoveryPolicy::Decision::attempt);
    const RecoveryPolicy::Decision d6 = p.onWake(LIMP_YIELD);
    TEST_ASSERT(d6 == RecoveryPolicy::Decision::declare);
    return true;
}

// One frame short of the threshold must NOT decay. The limper's whole trick is
// yielding a little every cycle; a >= that slipped to > , or an off-by-one in
// the comparison, would restore the infinite loop.
bool test_RecoveryPolicy_JustBelowThresholdDoesNotDecay() {
    RecoveryPolicy p;
    p.onWake(LIMP_YIELD);
    p.onWake(LIMP_YIELD);
    p.onWake(RecoveryPolicy::RECOVERY_DECAY_GOOD_FRAMES - 1);
    TEST_ASSERT_EQ(p.episodeNumber(), RecoveryPolicy::RECOVERY_EPISODE_CAP);
    const RecoveryPolicy::Decision d7 = p.onWake(LIMP_YIELD);
    TEST_ASSERT(d7 == RecoveryPolicy::Decision::declare);
    return true;
}

// The declaring wake must not consume an episode: it is a decision, not an
// attempt, and it must never reopen the queue (that reopen is exactly the
// context rebuild the cap exists to prevent). Repeated wakes stay a decision.
bool test_RecoveryPolicy_DeclarationConsumesNothing() {
    RecoveryPolicy p;
    for (int i = 0; i < RecoveryPolicy::RECOVERY_EPISODE_CAP; ++i) {
        p.onWake(LIMP_YIELD);
    }
    const int atCap = p.episodeNumber();

    const RecoveryPolicy::Decision d8 = p.onWake(LIMP_YIELD);
    TEST_ASSERT(d8 == RecoveryPolicy::Decision::declare);
    TEST_ASSERT_EQ(p.episodeNumber(), atCap);
    const RecoveryPolicy::Decision d9 = p.onWake(LIMP_YIELD);
    TEST_ASSERT(d9 == RecoveryPolicy::Decision::declare);
    TEST_ASSERT_EQ(p.episodeNumber(), atCap);
    return true;
}

// Even at the cap, evidence wins: a layer that delivered a full good run is
// rescued rather than declared. This is the safety valve on decision 3 - the
// cap kills limpers, not survivors.
bool test_RecoveryPolicy_DecayRescuesAtTheCap() {
    RecoveryPolicy p;
    for (int i = 0; i < RecoveryPolicy::RECOVERY_EPISODE_CAP; ++i) {
        p.onWake(LIMP_YIELD);
    }
    const RecoveryPolicy::Decision d10 = p.onWake(RecoveryPolicy::RECOVERY_DECAY_GOOD_FRAMES);
    TEST_ASSERT(d10 == RecoveryPolicy::Decision::attempt);
    TEST_ASSERT_EQ(p.episodeNumber(), 1);
    return true;
}

// Within-episode reopen failures are the worker's business, not the policy's:
// one wake is one episode however many times the reopen ladder retries inside
// it. Pinned because conflating the two is what the original defect did.
bool test_RecoveryPolicy_OneWakeIsOneEpisode() {
    RecoveryPolicy p;
    const RecoveryPolicy::Decision d11 = p.onWake(0);
    TEST_ASSERT(d11 == RecoveryPolicy::Decision::attempt);
    TEST_ASSERT_EQ(p.episodeNumber(), 1);
    // No policy call happens for the 2nd and 3rd reopen tries inside the
    // episode - so the count is still 1 when the worker parks.
    TEST_ASSERT_EQ(p.episodeNumber(), 1);
    return true;
}

// A first fault on a freshly opened queue reports zero good frames. It must be
// an ordinary attempt, not a decay and not a declaration.
bool test_RecoveryPolicy_ZeroYieldFirstWakeAttempts() {
    RecoveryPolicy p;
    TEST_ASSERT_EQ(p.episodeNumber(), 0);
    const RecoveryPolicy::Decision d12 = p.onWake(0);
    TEST_ASSERT(d12 == RecoveryPolicy::Decision::attempt);
    TEST_ASSERT_EQ(p.episodeNumber(), 1);
    return true;
}
