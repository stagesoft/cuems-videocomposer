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

#include "HangGuard.h"
#include "../utils/Logger.h"

#include <memory>
#include <sstream>

namespace videocomposer {

namespace {

/** Milliseconds since the epoch, for MonitorState's transition stamp. */
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

Classification HangGuard::classify(int width, int height, double framerate) {
    Classification c;

    // Fail closed on missing metadata.
    //
    // The frame rate comes from r_frame_rate with no fallback, and the
    // dimensions from codecpar; either can come back zero on a file whose
    // header we could not fully believe. Counting such a session as exempt
    // would let a 4K clip with a broken header sail past the cap - the exact
    // shape of bug that makes a guard worse than no guard, because it fails
    // silently in the direction of the hang.
    if (width <= 0 || height <= 0) {
        c.sessionClass = SessionClass::fourKClass;
        c.failedClosed = true;
        c.reason = "dimensions unknown - counted as 4K-class (fail closed)";
        return c;
    }
    if (!(framerate > 0.0)) {
        c.sessionClass = SessionClass::fourKClass;
        c.failedClosed = true;
        std::ostringstream os;
        os << "frame rate unknown for " << width << "x" << height
           << " - counted as 4K-class (fail closed)";
        c.reason = os.str();
        return c;
    }

    const long pixels = static_cast<long>(width) * static_cast<long>(height);
    std::ostringstream os;

    if (pixels <= FOURK_CLASS_PIXELS) {
        c.sessionClass = SessionClass::exempt;
        os << width << "x" << height << "@" << framerate
           << " is <= HD - exempt from the cap";
        c.reason = os.str();
        return c;
    }

    c.sessionClass = SessionClass::fourKClass;

    // Above 25 fps is outside every cell the campaign measured on this binary.
    // It is admitted anyway - the ladder that did run (2x4K60, 4x4K50, 4x4K60)
    // saturated without ever hanging, so refusing it would refuse the
    // unmeasured, which the actuation rule forbids.
    if (framerate > MEASURED_ENVELOPE_FPS + 0.01) {
        c.outsideEnvelope = true;
    }

    os << width << "x" << height << "@" << framerate << " is 4K-class";
    if (c.outsideEnvelope) {
        os << " and above the measured 25 fps envelope (counts 1, admitted with a warning)";
    }
    c.reason = os.str();
    return c;
}

// ---------------------------------------------------------------------------
// Construction and arming
// ---------------------------------------------------------------------------

HangGuard::HangGuard(const MachineProfile& profile)
    : profile_(profile)
    , lastAdvisoryLog_(std::chrono::steady_clock::time_point{})
{
}

void HangGuard::disarm(const std::string& why) {
    profile_.armed = false;
    guardSource_ = "flag";
    LOG_WARNING << "HangGuard: DISARMED (" << why << ") - the cap will not be "
                << "enforced; counts and warnings keep running";
}

// ---------------------------------------------------------------------------
// The armed-exposure counter
// ---------------------------------------------------------------------------

void HangGuard::noteLoadClassified(const Classification& c) {
    if (!c.isFourKClass()) {
        return;
    }
    const int prev = armed4kCount_.fetch_add(1, std::memory_order_relaxed);
    const int now = prev + 1;

    // Exact crossing detection from the value we just replaced: no re-read, so
    // two workers classifying at once cannot both decide they crossed.
    if (profile_.cap > 0 && prev == profile_.cap) {
        advisoryLatched_.store(true, std::memory_order_relaxed);
        lastTransitionMs_.store(nowMs(), std::memory_order_relaxed);
    }
    (void)now;
    reconcileAdvisoryLog();
}

void HangGuard::noteLoadReleased(bool wasFourKClass) {
    if (!wasFourKClass) {
        return;
    }
    const int prev = armed4kCount_.fetch_sub(1, std::memory_order_relaxed);
    const int now = prev - 1;

    if (profile_.cap > 0 && prev == profile_.cap + 1) {
        advisoryLatched_.store(false, std::memory_order_relaxed);
        lastTransitionMs_.store(nowMs(), std::memory_order_relaxed);
    }
    (void)now;
    reconcileAdvisoryLog();
}

void HangGuard::pollAdvisoryLog() {
    reconcileAdvisoryLog();
}

bool HangGuard::advisoryLogPending() const {
    if (profile_.cap <= 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(advisoryLogMutex_);
    return loggedOver_ != advisoryLatched_.load(std::memory_order_relaxed);
}

void HangGuard::reconcileAdvisoryLog() {
    // Monitor-only profiles have no cap to cross and say nothing: their
    // prevision arrives the day someone measures a boundary for them.
    if (profile_.cap <= 0) {
        return;
    }

    const bool over = advisoryLatched_.load(std::memory_order_relaxed);
    const int armedNow = armed4kCount_.load(std::memory_order_relaxed);

    bool emitOver = false;
    bool emitClear = false;
    long suppressed = 0;

    {
        std::lock_guard<std::mutex> lock(advisoryLogMutex_);
        if (over == loggedOver_) {
            return;  // the journal already says the truth
        }

        const auto now = std::chrono::steady_clock::now();
        const bool first = (lastAdvisoryLog_.time_since_epoch().count() == 0);
        if (!first && (now - lastAdvisoryLog_) < advisoryLogInterval_) {
            // Too soon. The state is still exact and still published in
            // MonitorState; only the line waits, and the monitor's tick will
            // emit it as soon as the window passes even if nothing else
            // happens.
            advisorySuppressed_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        lastAdvisoryLog_ = now;
        loggedOver_ = over;
        suppressed = advisorySuppressed_.exchange(0, std::memory_order_relaxed);
        emitOver = over;
        emitClear = !over;
    }

    std::ostringstream tail;
    if (suppressed > 0) {
        tail << " (" << suppressed << " further crossing(s) suppressed since the last line)";
    }

    if (emitOver) {
        std::ostringstream os;
        os << "HangGuard: ADVISORY armed4k=" << armedNow << " cap=" << profile_.cap;
        if (profile_.armed) {
            os << " - a " << (profile_.cap + 1)
               << "th simultaneous reveal will be refused";
        } else {
            // Saying "will be refused" here would be a lie, and this is the
            // mode the measurement harness runs in.
            os << " (guard off - nothing will be refused)";
        }
        os << tail.str();
        LOG_WARNING << os.str();
    } else if (emitClear) {
        LOG_INFO << "HangGuard: advisory-clear armed4k=" << armedNow
                 << " cap=" << profile_.cap << tail.str();
    }
}

// ---------------------------------------------------------------------------
// The reveal ledger
// ---------------------------------------------------------------------------

HangGuard::RevealDecision HangGuard::requestReveal(int layerId, SessionClass cls,
                                                   const std::string& cueId) {
    RevealDecision d;

    std::lock_guard<std::mutex> lock(ledgerMutex_);

    // Idempotent: /mtcfollow 1 arriving twice for one layer must not consume
    // two slots. The engine re-sends it at reveal as belt-and-suspenders, so
    // this is the normal path, not a defensive one.
    auto existing = reserved_.find(layerId);
    if (existing != reserved_.end()) {
        d.activeFourK = activeFourK_.load(std::memory_order_relaxed);
        return d;
    }

    if (cls != SessionClass::fourKClass) {
        // Exempt sessions are never refused at any count. They are still
        // tracked so the monitor can warn past the measured HD ceiling.
        reserved_[layerId] = cls;
        const int exempt = activeExempt_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (exempt > HD_EXEMPT_CEILING) {
            LOG_WARNING << "SaturationMonitor: WARN outside-measured-envelope "
                        << exempt << " concurrent exempt sessions (measured clean to "
                        << HD_EXEMPT_CEILING << ") - admitted, never refused";
        }
        d.activeFourK = activeFourK_.load(std::memory_order_relaxed);
        return d;
    }

    const int active = activeFourK_.load(std::memory_order_relaxed);

    if (willRefuse() && active >= profile_.cap) {
        std::ostringstream os;
        os << "would be 4K-class session " << (active + 1) << " decoding at once, "
           << "cap is " << profile_.cap << " on profile " << profile_.name
           << " (measured hang region begins at " << (profile_.cap + 1) << ")";
        d.admitted = false;
        d.reason = os.str();
        d.activeFourK = active;
        refusals_.fetch_add(1, std::memory_order_relaxed);
        lastTransitionMs_.store(nowMs(), std::memory_order_relaxed);

        LOG_ERROR << "HangGuard: REFUSED reveal of cue " << cueId
                  << " (layer " << layerId << "): " << d.reason
                  << " - the layer stays loaded and held, the project keeps running";
        return d;
    }

    reserved_[layerId] = cls;
    d.activeFourK = activeFourK_.fetch_add(1, std::memory_order_relaxed) + 1;

    if (!willRefuse() && profile_.cap > 0 && d.activeFourK > profile_.cap) {
        // Over the cap only because the guard is off. Loud, because this is
        // the state in which the machine can hang.
        LOG_WARNING << "HangGuard: " << d.activeFourK << " 4K-class sessions decoding, "
                    << "over the cap of " << profile_.cap
                    << " - guard is off, this is the measured hang region";
    }
    return d;
}

void HangGuard::transferReveal(int fromLayerId, int toLayerId) {
    std::lock_guard<std::mutex> lock(ledgerMutex_);
    auto it = reserved_.find(fromLayerId);
    if (it == reserved_.end()) {
        return;  // the old driver held nothing; nothing to inherit
    }
    const SessionClass cls = it->second;
    reserved_.erase(it);
    // Counts are unchanged by construction: one slot moves, none is created.
    // Deliberately no cap check - see the header.
    reserved_[toLayerId] = cls;
}

void HangGuard::releaseReveal(int layerId) {
    std::lock_guard<std::mutex> lock(ledgerMutex_);
    auto it = reserved_.find(layerId);
    if (it == reserved_.end()) {
        return;  // never reserved, or already released - both fine
    }
    if (it->second == SessionClass::fourKClass) {
        activeFourK_.fetch_sub(1, std::memory_order_relaxed);
    } else {
        activeExempt_.fetch_sub(1, std::memory_order_relaxed);
    }
    reserved_.erase(it);
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

void HangGuard::fillMonitorState(MonitorState& out) const {
    out.activeFourK = activeFourK_.load(std::memory_order_relaxed);
    out.armedFourK = armed4kCount_.load(std::memory_order_relaxed);
    out.activeExempt = activeExempt_.load(std::memory_order_relaxed);
    out.cap = profile_.cap;
    out.profile = profile_.name;
    out.guardArmed = willRefuse();
    out.advisoryLatched = advisoryLatched_.load(std::memory_order_relaxed);
    out.refusals = refusals_.load(std::memory_order_relaxed);
    out.lastTransitionMs = lastTransitionMs_.load(std::memory_order_relaxed);
}

void HangGuard::logStartupState() const {
    // Every setting reports where it came from. A guard that is off because a
    // flag turned it off and one that is off because the hardware has no
    // measured boundary are very different situations, and an operator
    // reading the journal after an incident must not have to guess which.
    if (willRefuse()) {
        LOG_INFO << "HangGuard: armed profile=" << profile_.name
                 << " (source=" << profile_.source << ")"
                 << " cap=" << profile_.cap
                 << " hang_guard=armed (source=" << guardSource_ << ")"
                 << " - " << profile_.detail;
    } else {
        LOG_WARNING << "HangGuard: monitor-only profile=" << profile_.name
                    << " (source=" << profile_.source << ")"
                    << " cap=" << profile_.cap
                    << " hang_guard=" << (profile_.armed ? "armed" : "off")
                    << " (source=" << guardSource_ << ")"
                    << " - NOTHING WILL BE REFUSED - " << profile_.detail;
    }

    std::ostringstream hw;
    hw << "HangGuard: hardware vendor=";
    if (profile_.pciVendor >= 0) hw << "0x" << std::hex << profile_.pciVendor << std::dec;
    else hw << "unknown";
    hw << " device=";
    if (profile_.pciDevice >= 0) hw << "0x" << std::hex << profile_.pciDevice << std::dec;
    else hw << "unknown";
    hw << " ram=" << profile_.ramTotalMb << "MB drm="
       << (profile_.drmDevicePath.empty() ? std::string("none") : profile_.drmDevicePath);
    LOG_INFO << hw.str();

    // The ledger is per-process; the hang is per-GPU. A guard with silent
    // bypasses is worse than none, so the bypasses are named at startup.
    LOG_INFO << "HangGuard: scope is this process only - a second videocomposer, "
             << "the separate videoindexer process, and HAP layers all decode "
             << "outside this ledger";
}

// ---------------------------------------------------------------------------
// Process-global instance
// ---------------------------------------------------------------------------

namespace {

/**
 * The process guard.
 *
 * Deliberately never destroyed. Other threads - the saturation monitor, a
 * loader worker finishing late, a layer being torn down - can reach the guard
 * during process teardown, and a static destructor running while they do
 * would be a use-after-free found only in a crash report from a venue. The
 * memory is reclaimed by the kernel at exit like everything else, so the leak
 * is a formality; the alternative is a real bug. ExitReporter's state is
 * arranged the same way and for the same reason.
 */
std::atomic<HangGuard*> g_guard{nullptr};

/**
 * Monitor-only stand-in, used until initHangGuard() runs and by any unit test
 * that never calls it. A path that reaches the guard before the profile is
 * resolved must be able to load and play - it must simply never be able to
 * refuse.
 */
HangGuard* fallbackGuard() {
    static HangGuard* g = new HangGuard([] {
        MachineProfile p;
        p.detail = "guard not initialised yet - monitor only";
        return p;
    }());
    return g;
}

} // namespace

HangGuard& hangGuard() {
    HangGuard* p = g_guard.load(std::memory_order_acquire);
    return p ? *p : *fallbackGuard();
}

void initHangGuard(const MachineProfile& profile) {
    // Called once, from startup, before any other thread exists. The old guard
    // (if a test installed one) is intentionally not freed - see above.
    g_guard.store(new HangGuard(profile), std::memory_order_release);
}

} // namespace videocomposer
