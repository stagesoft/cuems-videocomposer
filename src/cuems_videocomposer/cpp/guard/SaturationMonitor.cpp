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

#include "SaturationMonitor.h"
#include "HangGuard.h"
#include "SaturationSignals.h"
#include "../utils/Logger.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sstream>
#include <unistd.h>
#include <vector>

namespace videocomposer {

namespace {

int64_t nowMsEpoch() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

/** Read a whole small file. Returns false when it cannot be read. */
bool readFileText(const char* path, std::string& out) {
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[4096];
    out.clear();
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
        if (out.size() > 64 * 1024) break;  // fdinfo is never this big
    }
    ::close(fd);
    return true;
}

long readSysfsLongMb(const std::string& path) {
    if (path.empty()) return -1;
    std::string text;
    if (!readFileText(path.c_str(), text)) return -1;
    long v = 0;
    if (std::sscanf(text.c_str(), "%ld", &v) != 1) return -1;
    return v / (1024 * 1024);
}

/** Quietest interval between two logged saturation transitions. */
constexpr std::chrono::seconds TRANSITION_LOG_INTERVAL{5};

} // namespace

SaturationMonitor::SaturationMonitor() = default;

SaturationMonitor::~SaturationMonitor() {
    stop();
}

void SaturationMonitor::start(const MachineProfile& profile) {
    if (running_.load()) return;
    profile_ = profile;

    if (!profile_.drmDevicePath.empty()) {
        vramUsedPath_  = profile_.drmDevicePath + "/mem_info_vram_used";
        vramTotalPath_ = profile_.drmDevicePath + "/mem_info_vram_total";
        gttUsedPath_   = profile_.drmDevicePath + "/mem_info_gtt_used";
    }

    running_.store(true);
    thread_ = std::thread(&SaturationMonitor::run, this);
    LOG_INFO << "SaturationMonitor: started (sampling every "
             << SAMPLE_PERIOD.count() << " ms, WARN after "
             << WARN_SUSTAIN_SECONDS << " s sustained)";
}

void SaturationMonitor::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (kmsgFd_ >= 0) {
        ::close(kmsgFd_);
        kmsgFd_ = -1;
    }
}

void SaturationMonitor::run() {
    // The census is per-process, and so is this monitor. The IOMMU reading
    // attributed 8 of 37 ring timeouts to an ffmpeg outside this process,
    // which no amount of self-inspection can see. Say so once.
    LOG_INFO << "SaturationMonitor: signals are per-process - decode work by "
             << "other processes on the same GPU is invisible here";

    while (running_.load()) {
        sample();
        std::this_thread::sleep_for(SAMPLE_PERIOD);
    }
}

void SaturationMonitor::sample() {
    const auto now = std::chrono::steady_clock::now();

    const double occupancy = readDecodeOccupancyPercent();
    readMemory();
    const int newFaults = readNewPageFaults();

    double elapsedSec = 1.0;
    if (lastSampleAt_.time_since_epoch().count() != 0) {
        elapsedSec = std::chrono::duration<double>(now - lastSampleAt_).count();
    }
    lastSampleAt_ = now;
    if (elapsedSec <= 0.0) elapsedSec = 1.0;

    const long missTotal = signals::framesMissed();
    const double missRate = static_cast<double>(missTotal - lastMissTotal_) / elapsedSec;
    lastMissTotal_ = missTotal;

    const int erroring = signals::layersErroringWithin(ERROR_BURST_WINDOW_MS);

    // Let the guard close any advisory transition its own rate limit deferred.
    // Without this a clear could sit unlogged indefinitely, leaving the
    // journal asserting an exposure that has already gone away.
    hangGuard().pollAdvisoryLog();

    evaluate(occupancy, missRate, erroring, newFaults);
}

void SaturationMonitor::evaluate(double occupancy, double missRate,
                                 int erroringLayers, int newFaults) {
    const SaturationLevel current = static_cast<SaturationLevel>(level_.load());

    // --- ALARM conditions: happening now, and no threshold search behind them.
    std::string alarmWhy;
    if (newFaults > 0) {
        std::ostringstream os;
        os << "IO_PAGE_FAULT burst (" << newFaults << " in the last sample) - "
           << "every recorded ring timeout was preceded by one, typically ~10 s ahead";
        alarmWhy = os.str();
    } else if (erroringLayers >= ERROR_BURST_LAYERS) {
        std::ostringstream os;
        os << erroringLayers << " layers reporting decode errors at once - "
           << "a platform event rather than a bad file";
        alarmWhy = os.str();
    }

    if (!alarmWhy.empty()) {
        if (current != SaturationLevel::alarm) {
            level_.store(static_cast<int>(SaturationLevel::alarm));
            alarms_.fetch_add(1, std::memory_order_relaxed);
            lastTransitionMs_.store(nowMsEpoch(), std::memory_order_relaxed);
            logTransition(current, SaturationLevel::alarm, alarmWhy);
        }
        sustainedSeconds_ = WARN_SUSTAIN_SECONDS;  // stay hot while it lasts
        return;
    }

    // --- WARN conditions: sustained saturation, never instantaneous.
    std::string warnWhy;
    if (occupancy >= OCCUPANCY_WARN_PERCENT) {
        std::ostringstream os;
        os << "decode engine at " << occupancy << " %";
        warnWhy = os.str();
    } else if (missRate >= MISS_RATE_WARN_PER_SEC) {
        std::ostringstream os;
        os << missRate << " frame misses/s";
        warnWhy = os.str();
    }

    if (!warnWhy.empty()) {
        // The arm burst at project load drives these hard for ~3.5 s. Only a
        // condition that outlives it is worth an operator's attention.
        sustainedSeconds_ += 1;
        if (sustainedSeconds_ >= WARN_SUSTAIN_SECONDS && current == SaturationLevel::clear) {
            level_.store(static_cast<int>(SaturationLevel::warn));
            lastTransitionMs_.store(nowMsEpoch(), std::memory_order_relaxed);
            logTransition(current, SaturationLevel::warn, warnWhy);
        }
        return;
    }

    sustainedSeconds_ = 0;
    if (current != SaturationLevel::clear) {
        level_.store(static_cast<int>(SaturationLevel::clear));
        lastTransitionMs_.store(nowMsEpoch(), std::memory_order_relaxed);
        logTransition(current, SaturationLevel::clear, "signals back inside their measured range");
    }
}

void SaturationMonitor::logTransition(SaturationLevel from, SaturationLevel to,
                                      const std::string& why) {
    (void)from;

    const auto now = std::chrono::steady_clock::now();
    const bool first = (lastTransitionLog_.time_since_epoch().count() == 0);
    if (!first && (now - lastTransitionLog_) < TRANSITION_LOG_INTERVAL) {
        ++suppressedTransitions_;
        return;
    }
    lastTransitionLog_ = now;

    std::ostringstream tail;
    if (suppressedTransitions_ > 0) {
        tail << " (" << suppressedTransitions_ << " transition(s) suppressed since the last line)";
        suppressedTransitions_ = 0;
    }

    // Context every line carries, because the numbers only mean something
    // together: what is decoding, what is armed, and what the box is holding.
    const MonitorState s = monitorSnapshot();
    std::ostringstream census;
    census << " [active4k=" << s.activeFourK << " armed4k=" << s.armedFourK
           << " exempt=" << s.activeExempt << " cap=" << s.cap
           << " misses=" << signals::framesMissed()
           << " held=" << signals::framesHeldLong();
    if (s.vramUsedMb >= 0) {
        census << " vram=" << s.vramUsedMb << "/" << s.vramTotalMb << "MB";
    }
    if (s.gttUsedMb >= 0) {
        census << " gtt=" << s.gttUsedMb << "MB";
    }
    census << "]";

    switch (to) {
        case SaturationLevel::alarm:
            LOG_ERROR << "SaturationMonitor: ALARM " << why << census.str() << tail.str();
            break;
        case SaturationLevel::warn:
            LOG_WARNING << "SaturationMonitor: WARN " << why << census.str() << tail.str();
            break;
        case SaturationLevel::clear:
            LOG_INFO << "SaturationMonitor: clear " << why << census.str() << tail.str();
            break;
    }
}

// ---------------------------------------------------------------------------
// Signal readers
// ---------------------------------------------------------------------------

double SaturationMonitor::readDecodeOccupancyPercent() {
    // Sum drm-engine-dec across this process's DRM clients, deduplicated by
    // drm-client-id: several fds can name the same client, and counting a
    // client twice would report impossible occupancy.
    DIR* dir = ::opendir("/proc/self/fdinfo");
    if (!dir) {
        occupancyAvailable_.store(false);
        return -1.0;
    }

    std::vector<long long> seenClients;
    long long totalDecNs = 0;
    bool sawDecField = false;

    std::string text;
    while (dirent* e = ::readdir(dir)) {
        if (e->d_name[0] == '.') continue;
        std::string path = std::string("/proc/self/fdinfo/") + e->d_name;
        if (!readFileText(path.c_str(), text)) continue;
        if (text.find("drm-client-id:") == std::string::npos) continue;

        long long clientId = -1;
        const char* cid = std::strstr(text.c_str(), "drm-client-id:");
        if (cid) std::sscanf(cid, "drm-client-id: %lld", &clientId);
        if (clientId < 0) continue;

        if (std::find(seenClients.begin(), seenClients.end(), clientId) != seenClients.end()) {
            continue;
        }
        seenClients.push_back(clientId);

        const char* dec = std::strstr(text.c_str(), "drm-engine-dec:");
        if (dec) {
            long long ns = 0;
            if (std::sscanf(dec, "drm-engine-dec: %lld", &ns) == 1) {
                totalDecNs += ns;
                sawDecField = true;
            }
        }
    }
    ::closedir(dir);

    if (!sawDecField) {
        // i915 reports no decode engine at all - it would read a flat 0.0 %
        // while genuinely busy, which is worse than admitting ignorance.
        if (!announcedOccupancyUnavailable_) {
            announcedOccupancyUnavailable_ = true;
            LOG_INFO << "SaturationMonitor: decode occupancy unavailable on this "
                     << "platform (no drm-engine-dec) - saturation warnings will "
                     << "come from frame misses instead";
        }
        occupancyAvailable_.store(false);
        lastDecNs_ = -1;
        return -1.0;
    }

    occupancyAvailable_.store(true);

    if (lastDecNs_ < 0) {
        lastDecNs_ = totalDecNs;
        return -1.0;  // first sample establishes the baseline
    }

    const long long deltaNs = totalDecNs - lastDecNs_;
    lastDecNs_ = totalDecNs;

    const double windowNs = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(SAMPLE_PERIOD).count());
    double pct = (windowNs > 0.0) ? (static_cast<double>(deltaNs) / windowNs) * 100.0 : -1.0;
    if (pct < 0.0) pct = 0.0;   // a client closing can make the sum go backwards

    occupancyMilliPercent_.store(static_cast<long long>(pct * 1000.0),
                                 std::memory_order_relaxed);
    return pct;
}

void SaturationMonitor::readMemory() {
    const long used = readSysfsLongMb(vramUsedPath_);
    const long total = readSysfsLongMb(vramTotalPath_);
    const long gtt = readSysfsLongMb(gttUsedPath_);
    vramUsedMb_.store(used, std::memory_order_relaxed);
    vramTotalMb_.store(total, std::memory_order_relaxed);
    gttUsedMb_.store(gtt, std::memory_order_relaxed);
    memoryAvailable_.store(used >= 0 || total >= 0 || gtt >= 0, std::memory_order_relaxed);
}

int SaturationMonitor::readNewPageFaults() {
    if (kmsgFd_ < 0) {
        kmsgFd_ = ::open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (kmsgFd_ < 0) {
            // Typically EACCES as the cuems user. The sharpest signal we have
            // is simply absent here; the UI must show it as unavailable rather
            // than let a missing channel look like a quiet one.
            if (!announcedPageFaultUnavailable_) {
                announcedPageFaultUnavailable_ = true;
                LOG_INFO << "SaturationMonitor: /dev/kmsg not readable - "
                         << "IO_PAGE_FAULT alarms unavailable in-process "
                         << "(they remain visible in cuems-logs)";
            }
            pageFaultAvailable_.store(false, std::memory_order_relaxed);
            return -1;
        }
        // Start from the present: history belongs to cuems-logs, and replaying
        // an old fault as though it were happening now would be a false alarm.
        ::lseek(kmsgFd_, 0, SEEK_END);
        pageFaultAvailable_.store(true, std::memory_order_relaxed);
    }

    int faults = 0;
    char buf[8192];
    for (;;) {
        ssize_t n = ::read(kmsgFd_, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        if (std::strstr(buf, "IO_PAGE_FAULT") != nullptr) {
            ++faults;
        }
    }
    if (faults > 0) {
        pageFaultAlarms_.fetch_add(faults, std::memory_order_relaxed);
    }
    return faults;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

void SaturationMonitor::fillMonitorState(MonitorState& out) const {
    out.saturation = static_cast<SaturationLevel>(level_.load(std::memory_order_relaxed));
    out.saturationAlarms = alarms_.load(std::memory_order_relaxed);
    out.saturationAvailable = running_.load(std::memory_order_relaxed);

    const long long milli = occupancyMilliPercent_.load(std::memory_order_relaxed);
    out.decodeOccupancyAvailable = occupancyAvailable_.load(std::memory_order_relaxed);
    out.decodeOccupancyPercent = (milli <= -1000) ? -1.0 : static_cast<double>(milli) / 1000.0;

    out.vramUsedMb = vramUsedMb_.load(std::memory_order_relaxed);
    out.vramTotalMb = vramTotalMb_.load(std::memory_order_relaxed);
    out.gttUsedMb = gttUsedMb_.load(std::memory_order_relaxed);
    out.memoryAvailable = memoryAvailable_.load(std::memory_order_relaxed);

    out.pageFaultAlarms = pageFaultAlarms_.load(std::memory_order_relaxed);
    out.pageFaultAvailable = pageFaultAvailable_.load(std::memory_order_relaxed);

    // v1 never closes admission on a precursor. The field exists so the F9 UI
    // contract does not change if that is ever revisited.
    out.emergencyAdmissionClosed = false;
    out.emergencyAdmissionCloseAvailable = false;

    const long long t = lastTransitionMs_.load(std::memory_order_relaxed);
    if (t > out.lastTransitionMs) out.lastTransitionMs = t;
}

SaturationMonitor& saturationMonitor() {
    static SaturationMonitor m;
    return m;
}

// ---------------------------------------------------------------------------
// The one call F9 needs
// ---------------------------------------------------------------------------

MonitorState monitorSnapshot() {
    MonitorState s;
    hangGuard().fillMonitorState(s);
    saturationMonitor().fillMonitorState(s);
    return s;
}

} // namespace videocomposer
