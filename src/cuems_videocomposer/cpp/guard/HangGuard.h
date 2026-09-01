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

#ifndef VIDEOCOMPOSER_HANG_GUARD_H
#define VIDEOCOMPOSER_HANG_GUARD_H

#include "MachineProfile.h"
#include "MonitorState.h"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>

namespace videocomposer {

/**
 * How a decode session counts against the hang guard.
 *
 * Two classes, no weights. An earlier draft weighed sessions by pixel rate;
 * the campaign's own regression contradicted the formula (one 4K25 measured
 * ~5.2x an HD25 session plus a fixed floor, where pixel rate says 4x), so the
 * guard is a documented session cap and not a model of the hardware.
 */
enum class SessionClass {
    exempt,      // <= HD, or a decode path the guard does not account for
    fourKClass   // counts against the cap
};

/**
 * The outcome of classifying one loaded file.
 *
 * `outsideEnvelope` is not a refusal and must never become one: the actuation
 * rule permits refusing only what has been measured to hang, and no 4K-class
 * session above 25 fps ever has. The fps ladder ran 2x4K60, 4x4K50 and 4x4K60
 * and all three saturated brutally without hanging - so above-envelope content
 * counts 1 toward the cap and earns a warning, nothing more.
 */
struct Classification {
    SessionClass sessionClass = SessionClass::exempt;
    bool outsideEnvelope = false;  // 4K-class above 25 fps: admitted, warned
    bool failedClosed = false;     // metadata missing -> counted as 4K-class
    std::string reason;            // why, for the log

    bool isFourKClass() const { return sessionClass == SessionClass::fourKClass; }
};

/**
 * HangGuard - the only thing in this program that may refuse a layer.
 *
 * ## What it protects against
 *
 * On the FP530 (amdgpu, VCN 1.0) a run of concurrent 4K decode sessions can
 * wedge the decode ring: an IOMMU page fault in the GART band, then the 10 s
 * ring watchdog, then a dead compositor on all four outputs - and in the worst
 * measured case a GPU reset that itself failed, needing a physical power
 * cycle. Four concurrent 4K-class sessions have never been observed hanging;
 * five and beyond have, on both the 6.1 fleet kernel and 6.12, with a shape
 * that is non-monotonic (5 hung, 6 hung, 7 hung on 6.1; 5 clean, 6 hung, 7
 * clean on 6.12). Since no threshold search ever found a line - 99.6 %
 * occupancy survived while 86.9 % hung - the boundary is treated as a region
 * and the cap sits at its clean edge.
 *
 * ## What it counts, and why that is the hard part
 *
 * It counts sessions that are **actively decoding**, reserved when a layer
 * starts following MTC and released when it stops. It does NOT count loaded
 * layers. That distinction is measured, not assumed: 8 open-but-idle 4K
 * sessions held +3.8 GB of surface pools for 15 minutes with the decode
 * engine at 0.0 %, zero page faults and zero timeouts. Counting loaded layers
 * would refuse an ordinary chained show - the engine arms an entire
 * post_go='go' chain at project load and keeps two more cues armed ahead
 * during playback - and refusing a configuration never measured to hang
 * violates the actuation rule from the refusing side.
 *
 * A useful consequence: because the reservation belongs to the layer and a
 * re-load reuses the same layer, re-arming a cue over a loaded one is
 * net-zero by construction. There is no window in which a re-load counts
 * twice, so the guard can never refuse a re-arm and leave an output showing
 * the previous cue's media.
 *
 * ## Three counts, deliberately kept apart
 *
 * 1. **active 4K-class** (this ledger) - what the cap acts on.
 * 2. **armed 4K-class** (`armed4kCount`) - loaded and classified 4K-class,
 *    whether decoding or not. Drives the ADVISORY only; never refuses.
 * 3. the process-global HW decoder census in ExitReporter - resolution-blind,
 *    identity-free, and counts sources discarded mid-project-switch. It is
 *    inventory for the exit record and is **not** an input to anything here.
 *
 * ## Threading
 *
 * The ledger is a keyed map with writers on the OSC thread (reserve/release at
 * mtcfollow) and the render thread (layer teardown), so it takes a short-hold
 * leaf mutex - O(1) critical sections, no I/O, and nothing else is ever
 * acquired while holding it. In particular the monitor never reads the map:
 * it reads mirror atomics updated after each change, and those may tear
 * across fields. "Snapshot" is not promised across the whole struct.
 *
 * `armed4kCount` is a single atomic with no map behind it, so crossings are
 * detected exactly from the value returned by fetch_add/fetch_sub.
 */
class HangGuard {
public:
    /** Result of asking to start decoding one layer. */
    struct RevealDecision {
        bool admitted = true;
        std::string reason;      // populated only on refusal
        int activeFourK = 0;     // count at decision time, for the message
    };

    explicit HangGuard(const MachineProfile& profile);

    /** The profile in force, including cap and armed state. */
    const MachineProfile& profile() const { return profile_; }

    /**
     * Disarm without changing the profile, for --hang-guard=off. The ledger
     * and every count keep running - only the refusal stops - because the
     * harness rungs that measure new boundaries need the counts to stay true
     * while they deliberately exceed the cap.
     */
    void disarm(const std::string& why);

    /** Where the hang_guard setting came from: "compiled" or "flag". */
    void setGuardSource(const std::string& src) { guardSource_ = src; }

    /** Whether a refusal can currently happen. */
    bool willRefuse() const { return profile_.armed && profile_.cap > 0; }

    /**
     * Classify one opened file. Pure, and the single definition of what
     * "4K-class" means. Fails closed: missing dimensions or an unreadable
     * frame rate count as 4K-class rather than sailing through at zero
     * weight.
     */
    static Classification classify(int width, int height, double framerate);

    /** Pixels above which a session is 4K-class (1920x1080 exactly is not). */
    static constexpr long FOURK_CLASS_PIXELS = 1920L * 1080L;

    /** Frame rate at and below which a 4K-class session is inside the measured envelope. */
    static constexpr double MEASURED_ENVELOPE_FPS = 25.0;

    /** HD sessions measured clean concurrently. Beyond this the monitor warns; it never refuses. */
    static constexpr int HD_EXEMPT_CEILING = 12;

    // ---------------------------------------------------------------------
    // The armed-exposure counter (ADVISORY) - load time
    // ---------------------------------------------------------------------

    /**
     * A layer finished loading and was classified. Call exactly once per
     * loaded source, on the loader worker that classified it.
     *
     * This is what makes reveal-counting honest to the operator: because
     * nothing is refused at load, the load moment would otherwise be silent
     * and the first news of an over-committed project would arrive at GO. The
     * upward crossing of the cap logs one ADVISORY line naming the count.
     */
    void noteLoadClassified(const Classification& c);

    /**
     * The classified source is gone - closed, cancelled, or failed after
     * classification. Call exactly once per noteLoadClassified(), from
     * whichever thread destroys it.
     *
     * Takes the fact that was counted rather than a Classification to
     * re-read: a caller that re-classified in between would otherwise
     * decrement a different counter than it incremented and leak a slot for
     * the life of the process.
     */
    void noteLoadReleased(bool wasFourKClass);

    // ---------------------------------------------------------------------
    // The reveal ledger - the only refusal
    // ---------------------------------------------------------------------

    /**
     * A layer is about to start decoding (mtcfollow 1).
     *
     * Idempotent per layer: a second request for a layer that already holds a
     * slot is admitted without counting twice. Exempt sessions are always
     * admitted and never refused at any count.
     */
    RevealDecision requestReveal(int layerId, SessionClass cls, const std::string& cueId);

    /**
     * A layer stopped decoding (mtcfollow 0, loop-end un-follow) or is being
     * torn down. Idempotent, and safe for a layer that never reserved.
     */
    void releaseReveal(int layerId);

    /**
     * Move an existing reservation from one layer to another. Never refuses.
     *
     * For decode-driver promotion: when a cue is displayed on several outputs
     * the layers share one decoder, and only the driver holds the slot. If
     * that driver layer is removed, a surviving secondary is promoted and
     * inherits the very same decode session. The total number of decoding
     * sessions does not change - so applying the cap here would refuse
     * something that is already, demonstrably, running.
     */
    void transferReveal(int fromLayerId, int toLayerId);

    /** Current count of actively-decoding 4K-class sessions. */
    int activeFourK() const { return activeFourK_.load(std::memory_order_relaxed); }

    /** Current count of loaded, 4K-class-classified sources. */
    int armedFourK() const { return armed4kCount_.load(std::memory_order_relaxed); }

    /** Current count of actively-decoding exempt sessions. */
    int activeExempt() const { return activeExempt_.load(std::memory_order_relaxed); }

    /** Refusals since start. */
    long refusals() const { return refusals_.load(std::memory_order_relaxed); }

    /** Publish this object's fields into the shared snapshot. */
    void fillMonitorState(MonitorState& out) const;

    /** One line, at startup, saying exactly what is in force. */
    void logStartupState() const;

    /**
     * Give the advisory log a chance to catch up with reality.
     *
     * Called from the monitor's one-second tick. The rate limit that stops
     * mid-show oscillation from filling the journal can defer a transition,
     * and a deferred *clear* is the dangerous one: it would leave an ADVISORY
     * standing in the log for an exposure that no longer exists, which is a
     * lie of exactly the kind this design has spent its reviews removing. So
     * the log is reconciled against the real state rather than driven by
     * events, and it converges within one interval whether or not another
     * crossing ever happens.
     */
    void pollAdvisoryLog();

    /**
     * Whether the journal is currently behind the real advisory state.
     *
     * True between a crossing that the rate limit deferred and the line that
     * finally reports it. Never true for long: the monitor's tick reconciles
     * it within one interval.
     */
    bool advisoryLogPending() const;

    /**
     * Test seam: shorten the advisory rate-limit window.
     *
     * The property worth proving is that the journal converges on the truth -
     * that a deferred clear is deferred and not dropped - and proving it at
     * the production interval would mean a ten-second unit test.
     */
    void setAdvisoryLogIntervalForTest(std::chrono::milliseconds interval) {
        advisoryLogInterval_ = interval;
    }

private:
    /** Emit the advisory state if it has drifted from what was logged. */
    void reconcileAdvisoryLog();

    MachineProfile profile_;
    std::string guardSource_ = "compiled";

    // Ledger: layerId -> class. Leaf mutex, O(1) critical sections.
    mutable std::mutex ledgerMutex_;
    std::map<int, SessionClass> reserved_;

    // Mirror atomics. Written after each ledger change, read by the monitor.
    std::atomic<int>  activeFourK_{0};
    std::atomic<int>  activeExempt_{0};
    std::atomic<int>  armed4kCount_{0};
    std::atomic<long> refusals_{0};
    std::atomic<bool> advisoryLatched_{false};
    std::atomic<long long> lastTransitionMs_{0};

    // Advisory rate limit: mid-show the engine's lookahead arms and auto-unload
    // can oscillate the armed count across the cap. Crossings stay exact; only
    // the logging is collapsed, and the suppressed total is declared when the
    // next line is emitted.
    //
    // The log is reconciled against state, not driven by events: `loggedOver_`
    // is what the journal last said, `advisoryLatched_` is what is true, and
    // the pair is closed either by the next crossing or by the monitor's tick.
    std::atomic<long> advisorySuppressed_{0};
    // Ten seconds: long enough that a cue chain arming and auto-unloading
    // around the cap cannot fill the journal, short enough that an operator
    // reading the log after a scene change sees the true state.
    std::chrono::milliseconds advisoryLogInterval_{10000};
    bool loggedOver_ = false;
    std::chrono::steady_clock::time_point lastAdvisoryLog_{};
    mutable std::mutex advisoryLogMutex_;
};

/**
 * Process-global guard.
 *
 * Created once at startup by initHangGuard(). Before that call - and in any
 * unit test that never calls it - the accessor returns a guard on the
 * "unknown" profile, which is monitor-only and refuses nothing. That default
 * is deliberate: a code path that reaches the guard before startup finished
 * must not be able to refuse a layer.
 */
HangGuard& hangGuard();

/** Install the process guard for the resolved profile. Call once, at startup. */
void initHangGuard(const MachineProfile& profile);

} // namespace videocomposer

#endif // VIDEOCOMPOSER_HANG_GUARD_H
