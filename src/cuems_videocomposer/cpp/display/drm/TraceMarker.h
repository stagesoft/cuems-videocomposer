/*
 * SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
 *
 * This file is part of cuems-videocomposer.
 *
 * DEBUG ONLY -- lives on the instrumented branch, never on rc_1.
 *
 * Puts the compositor's own decisions into the kernel's trace buffer, so they
 * sit on the same timeline as the DRM tracepoints. This exists because the
 * amdgpu_dm_atomic_commit_tail_begin tracepoint fires in the kworker, which is
 * too late to say WHEN the compositor decided to submit -- and on the FP530
 * that gap is where 22.3ms go missing every frame (ClickUp 869emcrwa).
 *
 * Set trace_clock to "mono" and these markers are directly comparable with
 * drm_vblank_event_delivered and commit_tail_begin, to the microsecond.
 *
 * Off unless VIDEOCOMPOSER_TRACE_MARKER=1. Even then, writing to trace_marker
 * fails with EIO while tracing_on is 0, so a forgotten environment variable
 * costs one failed write per marker and nothing else. The compositor runs as
 * user "cuems", which cannot reach tracefs by default; grant it with
 *
 *     chmod o+x /sys/kernel/tracing
 *     chmod o+w /sys/kernel/tracing/trace_marker
 *
 * and take it back afterwards. Both are runtime-only and vanish on reboot.
 */

#ifndef CUEMS_VIDEOCOMPOSER_TRACEMARKER_H
#define CUEMS_VIDEOCOMPOSER_TRACEMARKER_H

#include <fcntl.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cuems {
namespace debug {

class TraceMarker {
public:
    static TraceMarker& instance() {
        static TraceMarker marker;
        return marker;
    }

    bool enabled() const { return fd_ >= 0; }

    void mark(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (fd_ < 0) {
            return;
        }
        char buf[256];
        va_list args;
        va_start(args, fmt);
        int n = vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if (n <= 0) {
            return;
        }
        if (n > static_cast<int>(sizeof(buf)) - 1) {
            n = sizeof(buf) - 1;
        }
        // Errors are expected and ignored: EIO simply means tracing is off.
        ssize_t written = ::write(fd_, buf, static_cast<size_t>(n));
        (void)written;
    }

    // Why the marker is not writing, in one line, so a silent capture is not
    // mistaken for a compositor that never submitted.
    const char* status() const { return status_; }

private:
    TraceMarker() {
        const char* env = std::getenv("VIDEOCOMPOSER_TRACE_MARKER");
        if (!env || (std::strcmp(env, "1") != 0 && std::strcmp(env, "true") != 0)) {
            status_ = "disabled (VIDEOCOMPOSER_TRACE_MARKER unset)";
            return;
        }
        static const char* const kPaths[] = {
            "/sys/kernel/tracing/trace_marker",
            "/sys/kernel/debug/tracing/trace_marker",
        };
        for (const char* path : kPaths) {
            fd_ = ::open(path, O_WRONLY | O_CLOEXEC);
            if (fd_ >= 0) {
                status_ = "enabled";
                return;
            }
        }
        status_ = "enabled but tracefs is not writable -- chmod o+x /sys/kernel/tracing"
                  " and o+w its trace_marker";
    }

    ~TraceMarker() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    TraceMarker(const TraceMarker&) = delete;
    TraceMarker& operator=(const TraceMarker&) = delete;

    int fd_ = -1;
    const char* status_ = "disabled";
};

}  // namespace debug
}  // namespace cuems

// The fd check is inlined at the call site so a disabled marker costs one
// predictable branch, not a call plus a vsnprintf.
#define VC_MARK(...)                                                     \
    do {                                                                 \
        auto& _m = ::cuems::debug::TraceMarker::instance();              \
        if (_m.enabled()) { _m.mark(__VA_ARGS__); }                      \
    } while (0)

#endif  // CUEMS_VIDEOCOMPOSER_TRACEMARKER_H
