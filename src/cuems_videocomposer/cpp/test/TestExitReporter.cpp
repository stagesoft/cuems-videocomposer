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
 * TestExitReporter.cpp - the F1 death record (869en65tm)
 *
 * The point of the reporter is what it does *as the process dies*, so most of
 * this runs the interesting paths in a forked child and reads back what the
 * child managed to say. Testing them in-process is not possible: the whole
 * subject is exit() and fatal signals.
 *
 * The policy under test:
 *   - before the main loop starts, say nothing (a --help run is not a death)
 *   - orderly stop -> one INFO line
 *   - exit() without an orderly stop -> one ERROR line with census + VRAM
 *     (this is the shape of the GPU-reset death: Mesa calls exit(1) itself)
 *   - fatal signal -> the same record from an async-signal-safe handler, and
 *     the process still dies of that signal
 */

#include "utils/ExitReporter.h"
#include "TestFramework.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

using namespace videocomposer;

namespace {

// Pin utils/Logger.h to its plain-stream path for the whole suite, before
// anything can log.
//
// The test target is built with HAVE_CUEMS_LOGGER -- despite the CMakeLists
// comment beside that definition claiming the test binaries keep the plain
// streams -- so when JOURNAL_STREAM is set (systemd sets it, and a developer
// shell inherits it) Logger routes through CuemsLogger to syslog. In production
// that is exactly what is wanted: it is what gives the death record a real
// journal PRIORITY instead of the flat 6 every stdout line gets. Here it would
// mean the forked child says its piece to the journal and the capture pipe sees
// an empty string, so the assertions below would pass or fail on the
// environment rather than on the code.
//
// Unsetting it inside the child is too late: Logger caches the answer in a
// function-local static on first use, and the suite logs long before these
// tests run. Static initialisation is the only point guaranteed to precede
// that.
const bool g_plainStreams = (::unsetenv("JOURNAL_STREAM"), true);

struct ChildResult {
    std::string output;   // stdout and stderr, merged
    int status = 0;
};

// Run `body` in a forked child with both streams captured. The child never
// returns; whatever exit path `body` takes is the path under test.
template <typename Fn>
ChildResult runInChild(Fn body) {
    int fds[2];
    if (::pipe(fds) != 0) return {};

    pid_t pid = ::fork();
    if (pid < 0) { ::close(fds[0]); ::close(fds[1]); return {}; }

    if (pid == 0) {
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[1]);
        body();
        // A body that returns is a bug in the test, not in the reporter.
        ::_exit(70);
    }

    ::close(fds[1]);
    ChildResult r;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(fds[0], buf, sizeof(buf))) > 0) r.output.append(buf, n);
    ::close(fds[0]);
    ::waitpid(pid, &r.status, 0);
    return r;
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// A CLI run -- install()ed but the main loop never started -- must be silent.
// Otherwise every `--help` and `--version` invocation would file a death record.
bool test_ExitReporter_SilentBeforeRunning() {
    ChildResult r = runInChild([] {
        exitreport::install();
        std::exit(0);
    });
    TEST_ASSERT(WIFEXITED(r.status) && WEXITSTATUS(r.status) == 0);
    TEST_ASSERT_FALSE(contains(r.output, "DIED"));
    TEST_ASSERT_FALSE(contains(r.output, "orderly shutdown"));
    return true;
}

// The orderly path states itself once, at INFO, and never as a death.
bool test_ExitReporter_CleanShutdownIsInfo() {
    ChildResult r = runInChild([] {
        exitreport::install();
        exitreport::markRunning();
        exitreport::markCleanShutdown();
        std::exit(0);
    });
    TEST_ASSERT(WIFEXITED(r.status) && WEXITSTATUS(r.status) == 0);
    TEST_ASSERT(contains(r.output, "[INFO]"));
    TEST_ASSERT(contains(r.output, "exit: orderly shutdown"));
    TEST_ASSERT_FALSE(contains(r.output, "DIED"));
    return true;
}

// The path that matters: something called exit() while the compositor was
// running, which is exactly how Mesa terminates us after a GPU reset. Must be
// ERROR, must carry the census, must not be swallowed.
bool test_ExitReporter_DirtyExitIsErrorWithCensus() {
    ChildResult r = runInChild([] {
        exitreport::install();
        exitreport::markRunning();
        exitreport::decoderOpened(11);
        exitreport::decoderOpened(11);
        exitreport::decodeErrorObserved(-1094995529);  // AVERROR_INVALIDDATA
        std::exit(1);                                  // Mesa's own exit code
    });
    TEST_ASSERT(WIFEXITED(r.status) && WEXITSTATUS(r.status) == 1);
    TEST_ASSERT(contains(r.output, "[ERROR]"));
    TEST_ASSERT(contains(r.output, "DIED"));
    TEST_ASSERT(contains(r.output, "hw_decoders=2"));
    TEST_ASSERT(contains(r.output, "hw_surfaces=22"));
    TEST_ASSERT(contains(r.output, "decode_errors=1"));
    TEST_ASSERT(contains(r.output, "last_decode_averr=-1094995529"));
    TEST_ASSERT(contains(r.output, "vram="));  // "n/a" off AMD, a number on it
    return true;
}

// Peaks survive teardown: a process that closed its decoders on the way out
// still has to say how many it was holding at the top.
bool test_ExitReporter_PeaksSurviveClose() {
    ChildResult r = runInChild([] {
        exitreport::install();
        exitreport::markRunning();
        exitreport::decoderOpened(11);
        exitreport::decoderOpened(11);
        exitreport::decoderOpened(11);
        exitreport::decoderClosed(11);
        exitreport::decoderClosed(11);
        exitreport::decoderClosed(11);
        std::exit(1);
    });
    TEST_ASSERT(contains(r.output, "hw_decoders=0 (peak 3)"));
    TEST_ASSERT(contains(r.output, "hw_surfaces=0 (peak 33)"));
    return true;
}

// A fatal signal runs no atexit handler, so the signal handler is the only
// thing that can speak. It must speak, and must then let the signal kill us
// with its default disposition so the exit status and core dump are untouched.
bool test_ExitReporter_FatalSignalRecordAndReraise() {
    ChildResult r = runInChild([] {
        exitreport::install();
        exitreport::markRunning();
        exitreport::decoderOpened(11);
        ::raise(SIGSEGV);
        ::_exit(71);  // reached only if the re-raise failed to kill us
    });
    TEST_ASSERT(WIFSIGNALED(r.status));
    TEST_ASSERT_EQ(WTERMSIG(r.status), SIGSEGV);
    TEST_ASSERT(contains(r.output, "DIED: fatal signal 11"));
    TEST_ASSERT(contains(r.output, "hw_decoders=1"));
    return true;
}

// One record per process. A signal arriving after the record was filed (or an
// abort chained off std::terminate) must not produce a second, contradictory
// line.
bool test_ExitReporter_ReportsOnlyOnce() {
    ChildResult r = runInChild([] {
        exitreport::install();
        exitreport::markRunning();
        ::raise(SIGABRT);
        ::_exit(72);
    });
    TEST_ASSERT(WIFSIGNALED(r.status));
    size_t first = r.output.find("DIED");
    TEST_ASSERT(first != std::string::npos);
    TEST_ASSERT_EQ(r.output.find("DIED", first + 1), std::string::npos);
    return true;
}

// The census is readable outside an exit path, and the counters are honest.
// Runs in-process on purpose: no install(), so the test binary's own exit stays
// unaffected.
bool test_ExitReporter_CensusLine() {
    exitreport::decoderOpened(11);
    std::string line = exitreport::censusLine();
    TEST_ASSERT(contains(line, "hw_decoders=1"));
    TEST_ASSERT(contains(line, "hw_surfaces=11"));
    TEST_ASSERT(contains(line, "uptime="));

    exitreport::decoderClosed(11);
    line = exitreport::censusLine();
    TEST_ASSERT(contains(line, "hw_decoders=0"));
    TEST_ASSERT(contains(line, "hw_surfaces=0"));
    return true;
}
