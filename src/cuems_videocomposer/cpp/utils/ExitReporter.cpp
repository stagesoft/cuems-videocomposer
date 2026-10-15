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

#include "ExitReporter.h"
#include "Logger.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <sstream>

#include <fcntl.h>
#include <unistd.h>

namespace videocomposer {
namespace exitreport {
namespace {

// ---------------------------------------------------------------------------
// State
//
// All plain atomics with no destructor of their own. The handler can run while
// other threads are mid-teardown, and on the fatal-signal path it runs with the
// process in an unknown state, so nothing here may depend on an object having
// been destroyed in any particular order.
// ---------------------------------------------------------------------------

std::atomic<bool> g_installed{false};
std::atomic<bool> g_running{false};        // main loop reached
std::atomic<bool> g_clean{false};          // orderly stop completed
std::atomic<bool> g_reported{false};       // one record per process, never two

std::atomic<int>  g_decoders{0};
std::atomic<int>  g_decodersPeak{0};
std::atomic<int>  g_surfaces{0};
std::atomic<int>  g_surfacesPeak{0};
std::atomic<long> g_decodeErrors{0};
std::atomic<int>  g_lastDecodeAVError{0};

std::chrono::steady_clock::time_point g_start{};

// ---------------------------------------------------------------------------
// amdgpu accounting
//
// These sysfs nodes are world-readable on card0 and cost one open/read/close.
// They are absent on non-AMD hardware (the Intel N97/N100 nodes) and can fail
// mid-reset, so every reader has to tolerate -1 rather than assume a number.
// ---------------------------------------------------------------------------

// Async-signal-safe: open/read/close are all on the POSIX safe list, and the
// parse is hand-rolled so no locale or allocation is involved. Used by both the
// signal path and the ordinary one, so there is a single reader to be wrong.
long readSysfsLong(const char* path) {
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[32];
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    long v = 0;
    bool any = false;
    for (ssize_t i = 0; i < n; ++i) {
        if (buf[i] < '0' || buf[i] > '9') break;
        v = v * 10 + (buf[i] - '0');
        any = true;
    }
    return any ? v : -1;
}

long vramUsedMb()  { long v = readSysfsLong("/sys/class/drm/card0/device/mem_info_vram_used");  return v < 0 ? -1 : v / (1024 * 1024); }
long vramTotalMb() { long v = readSysfsLong("/sys/class/drm/card0/device/mem_info_vram_total"); return v < 0 ? -1 : v / (1024 * 1024); }
long gttUsedMb()   { long v = readSysfsLong("/sys/class/drm/card0/device/mem_info_gtt_used");   return v < 0 ? -1 : v / (1024 * 1024); }

// TTM's page budget is what actually bounds the GTT aperture -- not
// mem_info_gtt_total -- and it defaults to half of system RAM. On the FP530 that
// is the overflow valve the eviction storm runs into, so the record states it.
long ttmLimitMb() { long p = readSysfsLong("/sys/module/ttm/parameters/pages_limit"); return p < 0 ? -1 : p / 256; }

long uptimeSeconds() {
    if (g_start.time_since_epoch().count() == 0) return -1;
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - g_start).count();
}

// ---------------------------------------------------------------------------
// The ordinary (atexit / set_terminate) path
// ---------------------------------------------------------------------------

void appendCensus(std::ostringstream& os) {
    os << "uptime=" << uptimeSeconds() << "s"
       << " hw_decoders=" << g_decoders.load()
       << " (peak " << g_decodersPeak.load() << ")"
       << " hw_surfaces=" << g_surfaces.load()
       << " (peak " << g_surfacesPeak.load() << ")"
       << " decode_errors=" << g_decodeErrors.load();
    const int averr = g_lastDecodeAVError.load();
    if (averr != 0) os << " last_decode_averr=" << averr;

    const long vu = vramUsedMb(), vt = vramTotalMb(), gu = gttUsedMb(), tl = ttmLimitMb();
    if (vt > 0) {
        os << " vram=" << vu << "/" << vt << "MB";
    } else {
        os << " vram=n/a";  // not an AMD card, or the node went away with the reset
    }
    if (gu >= 0) os << " gtt=" << gu << "MB";
    if (tl > 0)  os << "/" << tl << "MB";
}

void emitRecord(const char* how) {
    bool expected = false;
    if (!g_reported.compare_exchange_strong(expected, true)) return;
    if (!g_running.load()) return;  // a CLI run, not a compositor run

    std::ostringstream os;

    if (g_clean.load()) {
        os << "exit: orderly shutdown -- ";
        appendCensus(os);
        LOG_INFO << os.str();
        return;
    }

    os << "DIED: " << how << " -- ";
    appendCensus(os);

    // The one interpretation worth making here rather than leaving to whoever
    // reads the journal at 3am. 869en65tm's signature is the carve-out full at
    // the moment of death; anything else with these fields is a different bug.
    const long vu = vramUsedMb(), vt = vramTotalMb();
    if (vt > 0 && vu >= 0 && vu * 100 >= vt * 90 && g_decoders.load() > 0) {
        os << " -- VRAM at " << (vu * 100 / vt) << "% of the carve-out with "
           << g_decoders.load() << " decoder(s) live; consistent with the "
           << "eviction-storm GPU reset (869en65tm). Check dmesg for "
           << "'ring vcn_dec timeout'";
    }

    LOG_ERROR << os.str();
}

void onAtexit() {
    emitRecord("process exited without an orderly shutdown "
               "(exit()/return, not a signal -- a GPU reset kills us this way)");
}

void onTerminate() {
    emitRecord("std::terminate -- uncaught exception");
    std::abort();  // keeps the usual abort semantics; our SIGABRT handler is
                   // already spent by g_reported, so this does not double-report
}

// ---------------------------------------------------------------------------
// The fatal-signal path
//
// atexit handlers do not run for a signal death, so this is the only place a
// SIGSEGV leaves a mark. Everything below is async-signal-safe: `write` to
// stderr, no syslog (`syslog()` is not on the safe list), no iostreams, no
// allocation. Under systemd stderr is the journal, so the line lands in the same
// place the LOG_ERROR one would.
// ---------------------------------------------------------------------------

void sigWrite(const char* s) {
    if (!s) return;
    ssize_t ignored = ::write(STDERR_FILENO, s, ::strlen(s));
    (void)ignored;
}

void sigWriteLong(long v) {
    char buf[24];
    int i = sizeof(buf);
    bool neg = v < 0;
    unsigned long u = neg ? static_cast<unsigned long>(-(v + 1)) + 1u
                          : static_cast<unsigned long>(v);
    if (u == 0) buf[--i] = '0';
    while (u > 0) { buf[--i] = static_cast<char>('0' + (u % 10)); u /= 10; }
    if (neg) buf[--i] = '-';
    ssize_t ignored = ::write(STDERR_FILENO, buf + i, sizeof(buf) - i);
    (void)ignored;
}

extern "C" void onFatalSignal(int sig) {
    // Best effort only. If a second fatal signal arrives inside this handler,
    // SA_RESETHAND has already restored the default disposition, so the process
    // dies rather than looping.
    bool expected = false;
    if (g_reported.compare_exchange_strong(expected, true) && g_running.load()) {
        sigWrite("[ERROR] DIED: fatal signal ");
        sigWriteLong(sig);
        sigWrite(" -- uptime=");
        sigWriteLong(uptimeSeconds());
        sigWrite("s hw_decoders=");
        sigWriteLong(g_decoders.load());
        sigWrite(" hw_surfaces=");
        sigWriteLong(g_surfaces.load());
        sigWrite(" decode_errors=");
        sigWriteLong(g_decodeErrors.load());
        sigWrite(" vram=");
        sigWriteLong(vramUsedMb());
        sigWrite("/");
        sigWriteLong(vramTotalMb());
        sigWrite("MB gtt=");
        sigWriteLong(gttUsedMb());
        sigWrite("MB\n");
    }

    // Re-raise so the exit status, and any core dump, are exactly what they
    // would have been without us in the way.
    ::raise(sig);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

void install() {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) return;

    g_start = std::chrono::steady_clock::now();

    // Touch the logger first so its static outlives our handler: atexit
    // handlers and static destructors unwind in reverse registration order.
    Logger::getInstance();

    std::atexit(&onAtexit);
    std::set_terminate(&onTerminate);

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &onFatalSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;  // the re-raise then takes the default action
    const int fatal[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
    for (int sig : fatal) {
        ::sigaction(sig, &sa, nullptr);
    }
    // SIGTERM/SIGINT are intentionally left alone -- see the header.
}

void markRunning()       { g_running = true; }
void markCleanShutdown() { g_clean = true; }

void decoderOpened(int surfaces) {
    const int d = ++g_decoders;
    int peak = g_decodersPeak.load();
    while (d > peak && !g_decodersPeak.compare_exchange_weak(peak, d)) {}

    const int s = (g_surfaces += surfaces);
    int speak = g_surfacesPeak.load();
    while (s > speak && !g_surfacesPeak.compare_exchange_weak(speak, s)) {}
}

void decoderClosed(int surfaces) {
    --g_decoders;
    g_surfaces -= surfaces;
}

void decodeErrorObserved(int averr) {
    ++g_decodeErrors;
    g_lastDecodeAVError = averr;
}

std::string censusLine() {
    std::ostringstream os;
    appendCensus(os);
    return os.str();
}

}  // namespace exitreport
}  // namespace videocomposer
