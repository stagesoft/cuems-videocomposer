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

// #region DEBUG
// TEMPORARY instrumentation for ClickUp 869en65tm (vcn_dec ring hang / GPU
// reset). Whole file is debug scaffolding — delete it, and every `#region
// DEBUG` block that references it, once the investigation closes.
//
// Hypotheses under test:
//   H1  VRAM exhaustion -> eviction thrash -> VCN job misses its deadline
//   H2  the process is killed by Mesa (non-robust context), not by our code
//   H3  serialization on the single vcn_dec ring (session count, not bytes)
//   H4  two hardware decoders per layer double the surface pools
//   H5  layerTextureCache_ leak raises the VRAM floor
#ifndef VIDEOCOMPOSER_DEBUGLOG_H
#define VIDEOCOMPOSER_DEBUGLOG_H

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>

namespace vcdbg {

// Deliberately leaked: Mesa's exit(1) runs static destructors while the
// detached sampler thread may still be inside log(). A destroyed mutex there
// would crash us on the way out and destroy the very evidence H2 wants.
inline std::mutex& mutex() { static std::mutex* m = new std::mutex(); return *m; }
inline std::atomic<bool>& samplerStop() { static std::atomic<bool> b{false}; return b; }

// Live decoder accounting (H1/H3/H4). Every hardware AVCodecContext the
// process holds open is counted here, tagged by which of the two per-layer
// decoders it is.
inline std::atomic<int>&  syncDecoders()   { static std::atomic<int>  n{0}; return n; }
inline std::atomic<int>&  asyncDecoders()  { static std::atomic<int>  n{0}; return n; }
inline std::atomic<long>& surfaceBytes()   { static std::atomic<long> n{0}; return n; }
// Set by the reset detector (H2) so the last-gasp handler can say why we died.
inline std::atomic<int>&  resetSeen()      { static std::atomic<int>  n{0}; return n; }
// 869en4tqt: layerTextureCache_ occupancy. A monotonic rise across layer
// load/unload cycles is the leak; its size is what makes it matter to
// 869en65tm, since it eats the same 2 GB carve-out the decoders need.
// Pool sizes overridable at runtime so a headroom sweep needs one build, not
// one per arm: CUEMS_DEBUG_EXTRA_SYNC / CUEMS_DEBUG_EXTRA_ASYNC.
inline int envExtra(const char* name, int dflt) {
    const char* v = std::getenv(name);
    if (!v || !*v) return dflt;
    int n = std::atoi(v);
    return (n < 0 || n > 64) ? dflt : n;
}

inline std::atomic<int>&  textureCacheEntries() { static std::atomic<int>  n{0}; return n; }
inline std::atomic<long>& textureCacheBytes()   { static std::atomic<long> n{0}; return n; }

inline long readSysfsLong(const char* path) {
    std::ifstream f(path);
    if (!f) return -1;
    long v = -1;
    f >> v;
    return v;
}

// amdgpu exposes these unprivileged on card0; -1 whenever the node is absent
// (non-AMD box, or a reset in flight).
struct GpuMem {
    long vramUsed = -1, vramTotal = -1, gttUsed = -1;
};

inline GpuMem gpuMem() {
    GpuMem m;
    m.vramUsed  = readSysfsLong("/sys/class/drm/card0/device/mem_info_vram_used");
    m.vramTotal = readSysfsLong("/sys/class/drm/card0/device/mem_info_vram_total");
    m.gttUsed   = readSysfsLong("/sys/class/drm/card0/device/mem_info_gtt_used");
    return m;
}

// System-memory pressure. GTT is carved out of system RAM through TTM, and on
// this box TTM's default pages_limit is half of RAM (752313 pages = 2938 MB of
// 5877 MB). Every hang so far sits at GTT 2511-2619 MB while the survivor
// peaked at 2292, so the question is whether the box is simply running out of
// RAM to back the GTT aperture at that point.
struct SysMem { long availMb = -1, freeMb = -1, cachedMb = -1, swapFreeMb = -1; };

inline SysMem sysMem() {
    SysMem m;
    std::ifstream f("/proc/meminfo");
    std::string k;
    long v;
    std::string unit;
    while (f >> k >> v >> unit) {
        if      (k == "MemAvailable:") m.availMb   = v / 1024;
        else if (k == "MemFree:")      m.freeMb    = v / 1024;
        else if (k == "Cached:")       m.cachedMb  = v / 1024;
        else if (k == "SwapFree:")   { m.swapFreeMb = v / 1024; break; }
    }
    return m;
}

// TTM's global page accounting: how close the GTT aperture is to the limit that
// actually binds it (which is NOT mem_info_gtt_total).
inline long ttmPagesLimitMb() {
    long p = readSysfsLong("/sys/module/ttm/parameters/pages_limit");
    return p < 0 ? -1 : p / 256;  // 4 KiB pages -> MB
}

inline std::string gpuMemStr() {
    GpuMem m = gpuMem();
    std::ostringstream os;
    SysMem sm = sysMem();
    os << "vram=" << (m.vramUsed  < 0 ? -1 : m.vramUsed  / (1024 * 1024))
       << "/"     << (m.vramTotal < 0 ? -1 : m.vramTotal / (1024 * 1024))
       << "MB gtt=" << (m.gttUsed < 0 ? -1 : m.gttUsed   / (1024 * 1024)) << "MB"
       << "/" << ttmPagesLimitMb() << "MB"
       << " memavail=" << sm.availMb << "MB"
       << " memfree=" << sm.freeMb << "MB"
       << " cached=" << sm.cachedMb << "MB"
       << " swapfree=" << sm.swapFreeMb << "MB";
    return os.str();
}

// The binary is built on casas but runs on the FP530, so the log path has to be
// valid on the target, not on the build host: /tmp/.claude/debug.log. The unit
// has PrivateTmp=no, so the file is readable from outside the service.
inline void log(const char* hyps, const char* tag, const std::string& msg) {
    try {
        static bool dirReady = false;
        if (!dirReady) { ::mkdir("/tmp/.claude", 0777); dirReady = true; }

        auto now = std::chrono::system_clock::now();
        auto us  = std::chrono::duration_cast<std::chrono::microseconds>(
                       now.time_since_epoch()) % 1000000;
        auto t   = std::chrono::system_clock::to_time_t(now);
        std::tm tmb{};
        localtime_r(&t, &tmb);

        std::lock_guard<std::mutex> lk(mutex());
        std::ofstream f("/tmp/.claude/debug.log", std::ios::app);
        if (!f) return;
        f << "[" << std::put_time(&tmb, "%Y-%m-%dT%H:%M:%S")
          << "." << std::setw(6) << std::setfill('0') << us.count()
          << "] [DEBUG " << hyps << "] [" << tag << "] " << msg << "\n";
    } catch (...) {}
}

// Counts decoders and the surface memory we believe we asked for, so the
// accounting can be compared against what amdgpu actually reports (H1/H4).
inline std::string decoderCensus() {
    std::ostringstream os;
    os << "sync_decoders=" << syncDecoders().load()
       << " async_decoders=" << asyncDecoders().load()
       << " requested_surface_mb=" << (surfaceBytes().load() / (1024 * 1024))
       << " texcache_entries=" << textureCacheEntries().load()
       << " texcache_mb=" << (textureCacheBytes().load() / (1024 * 1024));
    return os.str();
}

// 1 Hz sampler. The capacity harness samples every 15 s, which is far too
// coarse to see the eviction thrash: E3's GTT doubled inside a single interval.
inline void startSampler() {
    static std::atomic<bool> started{false};
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) return;

    std::thread([] {
        while (!samplerStop().load()) {
            log("H1 H3 H4 H5", "SAMPLE",
                gpuMemStr() + " " + decoderCensus());
            for (int i = 0; i < 20 && !samplerStop().load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }).detach();
}

// H2: Mesa calls exit(1) itself when a CS is rejected on a non-robust context
// ("The CS has been rejected (-125), but the context isn't robust." /
// "The process will be terminated."). exit() DOES run atexit handlers, so this
// hook fires on that path and turns a silent death into a stated one. It is
// also the prototype of the real fix for the ticket's item 1.
inline void installLastGasp() {
    static std::atomic<bool> installed{false};
    bool expected = false;
    if (!installed.compare_exchange_strong(expected, true)) return;

    std::atexit([] {
        samplerStop() = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        std::ostringstream os;
        os << "process exiting -- " << decoderCensus()
           << " " << gpuMemStr()
           << " reset_status_seen=" << resetSeen().load();
        log("H2", "LAST-GASP", os.str());
    });

    std::set_terminate([] {
        log("H2", "LAST-GASP", "std::terminate -- uncaught exception; "
                               + decoderCensus());
        std::abort();
    });
}

}  // namespace vcdbg

#endif  // VIDEOCOMPOSER_DEBUGLOG_H
// #endregion DEBUG
