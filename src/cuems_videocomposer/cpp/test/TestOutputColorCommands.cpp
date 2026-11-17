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
 * TestOutputColorCommands.cpp - the show-lock gate and /videocomposer/display/color
 * (programme phase 3, plan S4): argument parsing and clamps, the bare-name LUT rule,
 * the status -> journal line mapping (a refusal is never "Unknown output"), the
 * paste-ready line, dump under the lock, `lut none`, and the output/list line that
 * must never look like a `region` line to the NDI preview tool.
 *
 * GL-free: the handler is driven with a fake DisplayBackend and a temporary lock
 * path. Log lines are asserted when they reach the terminal (not under the journal).
 */

#include "display/DisplayBackend.h"
#include "display/OutputColorConfig.h"
#include "display/ShowLock.h"
#include "remote/JournalContract.h"
#include "remote/OutputColorCommands.h"
#include "TestFramework.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace videocomposer;

namespace {

bool logsVisible() { return std::getenv("JOURNAL_STREAM") == nullptr; }

// Both streams: INFO goes to stdout, WARNING/ERROR to stderr
class LogCapture {
public:
    LogCapture()
        : oldOut_(std::cout.rdbuf(buf_.rdbuf())), oldErr_(std::cerr.rdbuf(buf_.rdbuf())) {}
    ~LogCapture() {
        std::cout.rdbuf(oldOut_);
        std::cerr.rdbuf(oldErr_);
    }
    std::string text() const { return buf_.str(); }

private:
    std::ostringstream buf_;
    std::streambuf* oldOut_;
    std::streambuf* oldErr_;
};

bool contains(const std::string& haystack, const std::string& needle) {
    if (haystack.find(needle) != std::string::npos) return true;
    std::cout << "\n  missing '" << needle << "' in:\n" << haystack << "  ";
    return false;
}

bool lacks(const std::string& haystack, const std::string& needle) {
    if (haystack.find(needle) == std::string::npos) return true;
    std::cout << "\n  unexpected '" << needle << "' in:\n" << haystack << "  ";
    return false;
}

// A private temp directory for the lock file; the gate goes back to the real path on exit
class LockDir {
public:
    LockDir() {
        char tmpl[] = "/tmp/vc-showlock-XXXXXX";
        const char* d = ::mkdtemp(tmpl);
        if (d) {
            dir_ = d;
            lock_ = dir_ + "/cuems.show.lock";
            setShowLockPathForTesting(lock_);
        }
    }
    ~LockDir() {
        setShowLockPathForTesting(std::string());
        ::unlink(lock_.c_str());
        if (!dir_.empty()) ::rmdir(dir_.c_str());
    }
    bool ok() const { return !dir_.empty(); }
    const std::string& lockPath() const { return lock_; }
    bool create() {
        FILE* f = std::fopen(lock_.c_str(), "w");
        return f && std::fclose(f) == 0;
    }
    bool remove() { return ::unlink(lock_.c_str()) == 0; }

private:
    std::string dir_;
    std::string lock_;
};

// The parts of DisplayBackend every backend must have, as no-ops
class NoWindowBackend : public DisplayBackend {
public:
    bool openWindow() override { return true; }
    void closeWindow() override {}
    bool isWindowOpen() const override { return true; }
    void render(LayerManager*, OSDManager*) override {}
    void handleEvents() override {}
    void resize(unsigned int, unsigned int) override {}
    void getWindowSize(unsigned int* w, unsigned int* h) const override { *w = 1920; *h = 1080; }
    void setPosition(int, int) override {}
    void getWindowPos(int* x, int* y) const override { *x = 0; *y = 0; }
    void setFullscreen(int) override {}
    bool getFullscreen() const override { return true; }
    void setOnTop(int) override {}
    bool getOnTop() const override { return true; }
    bool supportsMultiDisplay() const override { return false; }
};

// A backend with per-output color, behaving like DRMBackend's owner-first API
// (no GL): a LUT whose path ends in "bad.cube" fails to load.
class FakeColorBackend : public NoWindowBackend {
public:
    FakeColorBackend() {
        for (const char* name : {"HDMI-A-1", "HDMI-A-2"}) {
            OutputColorReport r;
            r.name = name;
            r.applied = true;
            outputs.push_back(r);
        }
    }

    std::vector<OutputColorReport> outputs;
    OutputColorStatus forced = OutputColorStatus::Applied;   // anything else: every call returns it
    int colorCalls = 0, lutCalls = 0, clearCalls = 0, resetCalls = 0;
    mutable int readCalls = 0;
    std::string lastResolvedPath;

    OutputColorReport* find(const std::string& name) {
        for (auto& r : outputs) {
            if (r.name == name) return &r;
        }
        return nullptr;
    }
    int mutations() const { return colorCalls + lutCalls + clearCalls + resetCalls; }

    OutputColorStatus configureOutputColor(const std::string& name, const OutputColor& trims) override {
        ++colorCalls;
        if (forced != OutputColorStatus::Applied) return forced;
        OutputColorReport* r = find(name);
        if (!r) return OutputColorStatus::UnknownOutput;
        OutputColor next = r->color;
        for (const auto& spec : kColorTrimSpecs) {
            if (!next.set(spec.trim, trims.get(spec.trim))) return OutputColorStatus::BadValue;
        }
        r->color = next;
        return OutputColorStatus::Applied;
    }
    OutputColorStatus configureOutputLut(const std::string& name, const std::string& resolvedPath,
                                         const std::string& displayValue,
                                         OutputLutLoadInfo& info) override {
        ++lutCalls;
        lastResolvedPath = resolvedPath;
        if (forced != OutputColorStatus::Applied) return forced;
        OutputColorReport* r = find(name);
        if (!r) return OutputColorStatus::UnknownOutput;
        const std::string bad = "bad.cube";
        if (resolvedPath.size() >= bad.size() &&
            resolvedPath.compare(resolvedPath.size() - bad.size(), bad.size(), bad) == 0) {
            info.error = resolvedPath + ": line 3: expected three numbers";
            return OutputColorStatus::LutFailed;   // the previous LUT stays
        }
        r->color.lutPath = resolvedPath;
        r->color.lutValue = displayValue;
        r->color.lutStatus = LutStatus::Ok;
        info.size = 17;
        info.parseMs = 1.0;
        return OutputColorStatus::Applied;
    }
    OutputColorStatus clearOutputLut(const std::string& name) override {
        ++clearCalls;
        if (forced != OutputColorStatus::Applied) return forced;
        OutputColorReport* r = find(name);
        if (!r) return OutputColorStatus::UnknownOutput;
        r->color.lutPath.clear();
        r->color.lutValue.clear();
        r->color.lutStatus = LutStatus::None;
        return OutputColorStatus::Applied;
    }
    OutputColorStatus resetOutputColor(const std::string& name) override {
        ++resetCalls;
        if (forced != OutputColorStatus::Applied) return forced;
        OutputColorReport* r = find(name);
        if (!r) return OutputColorStatus::UnknownOutput;
        r->color = OutputColor{};
        return OutputColorStatus::Applied;
    }
    OutputColorStatus getOutputColors(std::vector<OutputColorReport>& out) const override {
        ++readCalls;
        out = outputs;
        return OutputColorStatus::Applied;
    }
};

bool run(DisplayBackend& backend, const std::vector<std::string>& args, std::string* log = nullptr,
         const std::string& lutDir = "/tmp/vc-luts") {
    LogCapture capture;
    const bool ok = handleDisplayColorCommand(&backend, args, lutDir);
    if (log) *log = capture.text();
    return ok;
}

}  // namespace

// The gate reads the lock path it is given: absent -> pass, present -> refuse
bool test_ShowLock_GateOnTempPath() {
    TEST_ASSERT_EQ(std::string(showLockPath()), std::string("/tmp/cuems.show.lock"));
    TEST_ASSERT_EQ(std::string(kShowLockPath), std::string("/tmp/cuems.show.lock"));
    {
        LockDir lock;
        TEST_ASSERT(lock.ok());
        TEST_ASSERT_EQ(std::string(showLockPath()), lock.lockPath());

        TEST_ASSERT(!showLockPresent());
        TEST_ASSERT(!refuseUnderShowLock("display/blend"));

        TEST_ASSERT(lock.create());
        TEST_ASSERT(showLockPresent());
        TEST_ASSERT(showLockPresent(lock.lockPath().c_str()));
        std::string log;
        {
            LogCapture capture;
            TEST_ASSERT(refuseUnderShowLock("display/blend"));
            log = capture.text();
        }
        if (logsVisible()) {
            TEST_ASSERT(contains(log, "[WARNING] display/blend REFUSED: " + lock.lockPath() + " exists"));
        }

        TEST_ASSERT(lock.remove());
        TEST_ASSERT(!showLockPresent());
        TEST_ASSERT(!refuseUnderShowLock("display/blend"));
    }
    // A missing directory on the path reads as absent (ENOENT/ENOTDIR), not as a lock
    TEST_ASSERT(!showLockPresent("/nonexistent-vc-dir/cuems.show.lock"));
    TEST_ASSERT(!showLockPresent("/etc/hostname/cuems.show.lock"));
    // The override is gone with the LockDir
    TEST_ASSERT_EQ(std::string(showLockPath()), std::string(kShowLockPath));
    return true;
}

// The refusal names the path, both causes and what to do, and is never an "Unknown output"
bool test_ShowLock_RefusalLineNamesBothCauses() {
    const std::string line = showLockRefusalLine("display/blend", kShowLockPath);
    TEST_ASSERT_EQ(line, std::string(
        "display/blend REFUSED: /tmp/cuems.show.lock exists — a project is loaded, or the lock"
        " was left by an engine older than cuems-engine 0.1.0rc6-6 or by a crashed engine;"
        " unload the project (engine ≥ rc6-6) or stop the engines, then retry; color is also"
        " applied from display.conf at the next start"));
    TEST_ASSERT(lacks(line, "Unknown output"));
    return true;
}

// Numbers through parseFiniteFloat, clamped with a WARNING naming the value used, hue folded
bool test_OutputColorCommands_ParsesAndClamps() {
    LockDir lock;   // lock absent
    TEST_ASSERT(lock.ok());
    FakeColorBackend backend;
    std::string log;

    TEST_ASSERT(run(backend, {"HDMI-A-2", "contrast", "1.08"}, &log));
    TEST_ASSERT_EQ(backend.find("HDMI-A-2")->color.contrast, 1.08f);
    TEST_ASSERT_EQ(backend.colorCalls, 1);
    if (logsVisible()) {
        TEST_ASSERT(contains(log, "[INFO] OutputColor: HDMI-A-2 color_contrast=1.08 (applied live"));
    }

    TEST_ASSERT(run(backend, {"HDMI-A-2", "brightness", " 0.02 "}));   // whitespace stripped
    TEST_ASSERT_EQ(backend.find("HDMI-A-2")->color.brightness, 0.02f);
    TEST_ASSERT_EQ(backend.find("HDMI-A-2")->color.contrast, 1.08f);   // the other trims stay

    TEST_ASSERT(run(backend, {"HDMI-A-2", "gamma", "+1.5"}));
    TEST_ASSERT_EQ(backend.find("HDMI-A-2")->color.gamma, 1.5f);

    TEST_ASSERT(run(backend, {"HDMI-A-2", "contrast", "5"}, &log));
    TEST_ASSERT_EQ(backend.find("HDMI-A-2")->color.contrast, 2.0f);
    if (logsVisible()) {
        TEST_ASSERT(contains(log, "[WARNING] display/color: HDMI-A-2 contrast=5 out of range (0 to 2)"
                                  " — using 2"));
    }

    TEST_ASSERT(run(backend, {"HDMI-A-2", "hue", "540"}));
    TEST_ASSERT_EQ(backend.find("HDMI-A-2")->color.hue, 180.0f);
    TEST_ASSERT(run(backend, {"HDMI-A-2", "hue", "1e30"}));   // folded in O(1), never loops
    TEST_ASSERT(backend.find("HDMI-A-2")->color.hue >= -180.0f &&
                backend.find("HDMI-A-2")->color.hue <= 180.0f);

    // Refused before the backend: nothing changes
    const int callsBefore = backend.colorCalls;
    const OutputColor before = backend.find("HDMI-A-2")->color;
    for (const char* text : {"1,2", "1.2x", "inf", "nan", "", "++1"}) {
        TEST_ASSERT(!run(backend, {"HDMI-A-2", "saturation", text}, &log));
    }
    TEST_ASSERT_EQ(backend.colorCalls, callsBefore);
    TEST_ASSERT_EQ(backend.find("HDMI-A-2")->color.saturation, before.saturation);
    if (logsVisible()) {
        TEST_ASSERT(contains(log, "[WARNING] display/color: HDMI-A-2 saturation '++1' is not a finite"
                                  " number — nothing changed"));
    }
    TEST_ASSERT(!run(backend, {"HDMI-A-2", "contrast"}));            // value missing
    TEST_ASSERT(!run(backend, {"HDMI-A-2", "contrast", "1", "2"}));  // one too many
    TEST_ASSERT(!run(backend, {"HDMI-A-2", "sharpness", "1"}, &log));
    if (logsVisible()) {
        TEST_ASSERT(contains(log, "[WARNING] display/color: unknown parameter 'sharpness'"));
    }
    TEST_ASSERT(!run(backend, {"HDMI-A-2"}));
    TEST_ASSERT(!run(backend, {}));
    TEST_ASSERT_EQ(backend.colorCalls, callsBefore);

    // Unknown connector: a WARNING naming the outputs, sanitised
    TEST_ASSERT(!run(backend, {"DP-9\x1b[2J", "contrast", "1.1"}, &log));
    if (logsVisible()) {
        TEST_ASSERT(contains(log, "[WARNING] display/color: Unknown output 'DP-9?[2J'"
                                  " (outputs: HDMI-A-1, HDMI-A-2)"));
        TEST_ASSERT(lacks(log, "\x1b"));
    }

    // reset: trims to identity and the LUT cleared
    TEST_ASSERT(run(backend, {"HDMI-A-2", "lut", "p2.cube"}));
    TEST_ASSERT(run(backend, {"HDMI-A-2", "reset"}, &log));
    TEST_ASSERT(!backend.find("HDMI-A-2")->color.trimsActive());
    TEST_ASSERT(!backend.find("HDMI-A-2")->color.lutConfigured());
    TEST_ASSERT_EQ(backend.resetCalls, 1);
    if (logsVisible()) {
        TEST_ASSERT(contains(log, "[INFO] OutputColor: HDMI-A-2 identity, no color_* keys (applied live"));
    }
    return true;
}

// lut takes a bare name ending in .cube under the LUT directory, nothing else
bool test_OutputColorCommands_RejectsPathsAndDotDot() {
    LockDir lock;
    TEST_ASSERT(lock.ok());
    FakeColorBackend backend;
    std::string log;
    for (const char* name : {"/etc/passwd", "../x.cube", "sub/x.cube", "x.txt", ".cube", "a..b.cube",
                             "/tmp/vc-luts/p2.cube", "x\n.cube", "x.cube/"}) {
        TEST_ASSERT(!run(backend, {"HDMI-A-2", "lut", name}, &log));
        if (logsVisible()) {
            TEST_ASSERT(contains(log, "REFUSED: LUT name must be a bare file name ending in .cube"));
            TEST_ASSERT(contains(log, "— nothing changed"));
        }
    }
    TEST_ASSERT_EQ(backend.lutCalls, 0);
    TEST_ASSERT(!backend.find("HDMI-A-2")->color.lutConfigured());
    if (logsVisible()) {
        TEST_ASSERT(run(backend, {"HDMI-A-2", "lut", "x\n.cube"}, &log) == false);
        TEST_ASSERT(contains(log, "lut 'x?.cube' REFUSED"));   // control characters never logged raw
    }

    // A bare name resolves under the LUT directory and keeps its operator form
    TEST_ASSERT(run(backend, {"HDMI-A-2", "lut", "p2-christie.cube"}, &log, "/srv/test-luts"));
    TEST_ASSERT_EQ(backend.lastResolvedPath, std::string("/srv/test-luts/p2-christie.cube"));
    TEST_ASSERT_EQ(backend.find("HDMI-A-2")->color.lutValue, std::string("p2-christie.cube"));
    if (logsVisible()) {
        TEST_ASSERT(contains(log, "[INFO] OutputColor: HDMI-A-2 color_lut=p2-christie.cube loaded"
                                  " (ok, 17^3, parsed in 1 ms)"));
        TEST_ASSERT(contains(log, "[INFO] OutputColor: HDMI-A-2 color_lut=p2-christie.cube"
                                  " (applied live — volatile: paste into /etc/cuems/display.conf"
                                  " to keep it)"));
    }
    return true;
}

// The paste-ready line: display.conf syntax, keys at column 0 when pasted, no spaces in keys
bool test_OutputColorCommands_PasteReadyLine() {
    OutputColor color;
    color.contrast = 1.08f;
    color.hue = -3.0f;
    color.lutPath = "/etc/cuems/luts/p2.cube";
    color.lutValue = "p2.cube";
    color.lutStatus = LutStatus::Ok;
    TEST_ASSERT_EQ(outputColorPasteReadyLine("HDMI-A-2", color), std::string(
        "OutputColor: HDMI-A-2 color_contrast=1.08 color_hue=-3 color_lut=p2.cube"
        " (applied live — volatile: paste into /etc/cuems/display.conf to keep it)"));

    TEST_ASSERT_EQ(outputColorPasteReadyLine("HDMI-A-1", OutputColor{}), std::string(
        "OutputColor: HDMI-A-1 identity, no color_* keys (applied live — volatile: remove this"
        " output's color_* keys from /etc/cuems/display.conf to keep it)"));

    // Wire strings sanitised; a long LUT name is not cut (it must paste back exactly)
    OutputColor odd;
    odd.lutValue = std::string(100, 'a') + ".cube";
    odd.lutPath = "/etc/cuems/luts/" + odd.lutValue;
    const std::string line = outputColorPasteReadyLine("HDMI\x01-A-2", odd);
    TEST_ASSERT(contains(line, "OutputColor: HDMI?-A-2 color_lut=" + odd.lutValue + " (applied live"));

    // What the handler logs after a change is the same line, built from the backend's state
    LockDir lock;
    TEST_ASSERT(lock.ok());
    FakeColorBackend backend;
    std::string log;
    TEST_ASSERT(run(backend, {"HDMI-A-2", "contrast", "1.08"}, &log));
    TEST_ASSERT(run(backend, {"HDMI-A-2", "hue", "-3"}, &log));
    TEST_ASSERT(run(backend, {"HDMI-A-2", "lut", "p2.cube"}, &log));
    if (logsVisible()) {
        TEST_ASSERT(contains(log, "[INFO] " + outputColorPasteReadyLine("HDMI-A-2", color)));
    }
    return true;
}

// Under the lock every mutation is refused before the backend; dump still answers
bool test_OutputColorCommands_DumpAllowedUnderLock() {
    LockDir lock;
    TEST_ASSERT(lock.ok());
    FakeColorBackend backend;
    TEST_ASSERT(run(backend, {"HDMI-A-2", "contrast", "1.2"}));
    TEST_ASSERT(run(backend, {"HDMI-A-2", "lut", "p1.cube"}));
    const int mutationsBefore = backend.mutations();
    TEST_ASSERT(lock.create());

    std::string log;
    const std::vector<std::vector<std::string>> mutating = {
        {"HDMI-A-2", "contrast", "1.5"}, {"HDMI-A-2", "lut", "p2.cube"}, {"HDMI-A-2", "lut", "none"},
        {"HDMI-A-2", "reset"}, {"DP-9", "contrast", "1.1"}, {"HDMI-A-2", "sharpness", "1"}};
    for (const auto& args : mutating) {
        TEST_ASSERT(!run(backend, args, &log));
        if (logsVisible()) {
            TEST_ASSERT(contains(log, "[WARNING] display/color REFUSED: " + lock.lockPath() + " exists"));
            TEST_ASSERT(lacks(log, "Unknown output"));
        }
    }
    TEST_ASSERT_EQ(backend.mutations(), mutationsBefore);
    TEST_ASSERT_EQ(backend.find("HDMI-A-2")->color.contrast, 1.2f);
    TEST_ASSERT_EQ(backend.find("HDMI-A-2")->color.lutValue, std::string("p1.cube"));

    // dump (all, and one output) is read-only and answers under the lock
    const int readsBefore = backend.readCalls;
    TEST_ASSERT(run(backend, {"dump"}, &log));
    TEST_ASSERT(backend.readCalls > readsBefore);
    if (logsVisible()) {
        TEST_ASSERT(lacks(log, "REFUSED"));
        TEST_ASSERT(contains(log, "[INFO] OutputColor: HDMI-A-1 identity, no color_* keys"
                                  " (dump: lut_status=none applied=yes)"));
        TEST_ASSERT(contains(log, "[INFO] OutputColor: HDMI-A-2 color_contrast=1.2 color_lut=p1.cube"
                                  " (dump: lut_status=ok applied=yes)"));
    }
    TEST_ASSERT(run(backend, {"HDMI-A-2", "dump"}, &log));
    if (logsVisible()) {
        TEST_ASSERT(lacks(log, "HDMI-A-1"));
    }
    TEST_ASSERT(!run(backend, {"DP-9", "dump"}, &log));
    if (logsVisible()) {
        TEST_ASSERT(contains(log, "[WARNING] display/color: Unknown output 'DP-9'"));
    }
    TEST_ASSERT_EQ(backend.mutations(), mutationsBefore);

    // A dump says a failed LUT and an unapplied color again (plan 2.9)
    backend.find("HDMI-A-1")->color.lutValue = "gone.cube";
    backend.find("HDMI-A-1")->color.lutPath = "/etc/cuems/luts/gone.cube";
    backend.find("HDMI-A-1")->color.lutStatus = LutStatus::Missing;
    backend.find("HDMI-A-1")->applied = false;
    TEST_ASSERT(run(backend, {"HDMI-A-1", "dump"}, &log));
    if (logsVisible()) {
        TEST_ASSERT(contains(log, "[ERROR] OutputColor: HDMI-A-1 color_lut=gone.cube NOT APPLIED:"
                                  " lut_status=missing — output runs with trims only"));
        TEST_ASSERT(contains(log, "[ERROR] OutputColor: HDMI-A-1 color configured but not on screen"));
    }

    // The lock gone, the same change applies
    TEST_ASSERT(lock.remove());
    TEST_ASSERT(run(backend, {"HDMI-A-2", "contrast", "1.5"}));
    TEST_ASSERT_EQ(backend.find("HDMI-A-2")->color.contrast, 1.5f);
    return true;
}

// lut none clears the LUT only; the trims stay
bool test_OutputColorCommands_LutNoneClearsOnlyLut() {
    LockDir lock;
    TEST_ASSERT(lock.ok());
    FakeColorBackend backend;
    TEST_ASSERT(run(backend, {"HDMI-A-2", "contrast", "1.2"}));
    TEST_ASSERT(run(backend, {"HDMI-A-2", "gamma", "1.1"}));
    TEST_ASSERT(run(backend, {"HDMI-A-2", "lut", "p2.cube"}));
    TEST_ASSERT(backend.find("HDMI-A-2")->color.lutConfigured());
    const int colorCalls = backend.colorCalls;

    std::string log;
    TEST_ASSERT(run(backend, {"HDMI-A-2", "lut", "none"}, &log));
    const OutputColor& c = backend.find("HDMI-A-2")->color;
    TEST_ASSERT(!c.lutConfigured());
    TEST_ASSERT(c.lutValue.empty());
    TEST_ASSERT(c.lutStatus == LutStatus::None);
    TEST_ASSERT_EQ(c.contrast, 1.2f);
    TEST_ASSERT_EQ(c.gamma, 1.1f);
    TEST_ASSERT_EQ(backend.clearCalls, 1);
    TEST_ASSERT_EQ(backend.resetCalls, 0);
    TEST_ASSERT_EQ(backend.colorCalls, colorCalls);   // the trims were not rewritten
    if (logsVisible()) {
        TEST_ASSERT(contains(log, "[INFO] OutputColor: HDMI-A-2 color_contrast=1.2 color_gamma=1.1"
                                  " (applied live"));
    }
    return true;
}

// Every backend status has its own line; only UnknownOutput says "Unknown output"
bool test_OutputColorCommands_StatusToLogLine() {
    TEST_ASSERT_EQ(outputColorFailureLine(OutputColorStatus::Applied, "HDMI-A-2"), std::string());
    TEST_ASSERT_EQ(outputColorFailureLine(OutputColorStatus::UnknownOutput, "DP-9", "outputs: HDMI-A-1"),
                   std::string("display/color: Unknown output 'DP-9' (outputs: HDMI-A-1)"));
    TEST_ASSERT_EQ(outputColorFailureLine(OutputColorStatus::Unsupported, "HDMI-A-2"),
                   std::string("display/color: per-output color is not supported by this display backend"));
    TEST_ASSERT_EQ(outputColorFailureLine(OutputColorStatus::BadValue, "HDMI-A-2", "gamma 'x' is not a finite number"),
                   std::string("display/color: HDMI-A-2 gamma 'x' is not a finite number — nothing changed"));
    TEST_ASSERT_EQ(outputColorFailureLine(OutputColorStatus::LutFailed, "HDMI-A-2",
                                          "color_lut=p2.cube NOT APPLIED: why — previous LUT p1.cube kept"),
                   std::string("OutputColor: HDMI-A-2 color_lut=p2.cube NOT APPLIED: why — previous LUT p1.cube kept"));
    TEST_ASSERT_EQ(outputColorFailureLine(OutputColorStatus::ProgramUnavailable, "HDMI-A-2", "color_contrast=1.2"),
                   std::string("OutputColor: HDMI-A-2 color configured but the color program is unavailable"
                               " — running without color (stored: color_contrast=1.2)"));
    for (auto s : {OutputColorStatus::Unsupported, OutputColorStatus::BadValue, OutputColorStatus::LutFailed,
                   OutputColorStatus::ProgramUnavailable}) {
        TEST_ASSERT(lacks(outputColorFailureLine(s, "HDMI-A-2", "d"), "Unknown output"));
    }
    TEST_ASSERT(outputColorFailureIsError(OutputColorStatus::Unsupported));
    TEST_ASSERT(outputColorFailureIsError(OutputColorStatus::LutFailed));
    TEST_ASSERT(outputColorFailureIsError(OutputColorStatus::ProgramUnavailable));
    TEST_ASSERT(!outputColorFailureIsError(OutputColorStatus::UnknownOutput));
    TEST_ASSERT(!outputColorFailureIsError(OutputColorStatus::BadValue));

    LockDir lock;
    TEST_ASSERT(lock.ok());
    std::string log;

    // A backend without per-output color (the DisplayBackend defaults): ERROR, every form
    NoWindowBackend plain;
    TEST_ASSERT(!run(plain, {"HDMI-A-1", "contrast", "1.1"}, &log));
    if (logsVisible()) {
        TEST_ASSERT(contains(log, "[ERROR] display/color: per-output color is not supported by this"
                                  " display backend"));
        TEST_ASSERT(lacks(log, "Unknown output"));
    }
    TEST_ASSERT(!run(plain, {"HDMI-A-1", "reset"}, &log));
    TEST_ASSERT(!run(plain, {"HDMI-A-1", "lut", "none"}, &log));
    TEST_ASSERT(!run(plain, {"dump"}, &log));
    if (logsVisible()) {
        TEST_ASSERT(contains(log, "[ERROR] display/color: per-output color is not supported"));
    }
    TEST_ASSERT(outputColorListLines(plain).empty());

    // A failed hot LUT change keeps the previous LUT and says which
    FakeColorBackend backend;
    TEST_ASSERT(run(backend, {"HDMI-A-2", "lut", "p1.cube"}));
    TEST_ASSERT(!run(backend, {"HDMI-A-2", "lut", "bad.cube"}, &log));
    TEST_ASSERT_EQ(backend.find("HDMI-A-2")->color.lutValue, std::string("p1.cube"));
    if (logsVisible()) {
        TEST_ASSERT(contains(log, "[ERROR] OutputColor: HDMI-A-2 color_lut=bad.cube NOT APPLIED:"
                                  " /tmp/vc-luts/bad.cube: line 3: expected three numbers"
                                  " — previous LUT p1.cube kept"));
        TEST_ASSERT(lacks(log, "applied live"));
    }
    TEST_ASSERT(!run(backend, {"HDMI-A-1", "lut", "bad.cube"}, &log));
    if (logsVisible()) {
        TEST_ASSERT(contains(log, "— no previous LUT, the output keeps its trims"));
    }

    // The color program unavailable: stored, ERROR, not "applied live"
    backend.forced = OutputColorStatus::ProgramUnavailable;
    TEST_ASSERT(!run(backend, {"HDMI-A-2", "contrast", "1.3"}, &log));
    if (logsVisible()) {
        TEST_ASSERT(contains(log, "[ERROR] OutputColor: HDMI-A-2 color configured but the color program"
                                  " is unavailable — running without color"));
        TEST_ASSERT(lacks(log, "applied live"));
    }

    // A value the backend refuses: WARNING, nothing changed
    backend.forced = OutputColorStatus::BadValue;
    TEST_ASSERT(!run(backend, {"HDMI-A-2", "contrast", "1.3"}, &log));
    if (logsVisible()) {
        TEST_ASSERT(contains(log, "[WARNING] display/color: HDMI-A-2 bad value — nothing changed"));
    }

    // Unknown output from the backend itself (reset has no read before it)
    backend.forced = OutputColorStatus::Applied;
    TEST_ASSERT(!run(backend, {"DP-9", "reset"}, &log));
    if (logsVisible()) {
        TEST_ASSERT(contains(log, "[WARNING] display/color: Unknown output 'DP-9' (outputs: HDMI-A-1, HDMI-A-2)"));
    }
    return true;
}

// output/list's color line never matches the preview tool's region regex
bool test_OutputList_ColorLineNeverMatchesRegionRegex() {
    // cuemspowerbridge/ndi_preview.py:89
    const std::regex regionRe(R"(^region (\S+) (-?\d+),(-?\d+),(\d+),(\d+)$)");
    TEST_ASSERT(std::regex_match(journal::region("HDMI-A-2", 1920, 0, 1920, 1080), regionRe));

    FakeColorBackend backend;
    backend.find("HDMI-A-2")->color.contrast = 1.08f;
    backend.find("HDMI-A-2")->color.lutPath = "/etc/cuems/luts/p2.cube";
    backend.find("HDMI-A-2")->color.lutValue = "p2.cube";
    backend.find("HDMI-A-2")->color.lutStatus = LutStatus::Ok;
    backend.find("HDMI-A-1")->applied = false;
    OutputColorReport hostile;
    hostile.name = "region";   // even a connector named like the prefix
    backend.outputs.push_back(hostile);

    const auto lines = outputColorListLines(backend);
    TEST_ASSERT_EQ(lines.size(), static_cast<std::size_t>(3));
    TEST_ASSERT_EQ(lines[0], std::string("output_color HDMI-A-1 lut_status=none applied=no"));
    TEST_ASSERT_EQ(lines[1], std::string("output_color HDMI-A-2 color_contrast=1.08 color_lut=p2.cube"
                                         " lut_status=ok applied=yes"));
    TEST_ASSERT_EQ(lines[2], std::string("output_color region lut_status=none applied=no"));
    for (const auto& line : lines) {
        TEST_ASSERT(line.rfind("output_color ", 0) == 0);
        TEST_ASSERT(line.rfind("region", 0) != 0);
        TEST_ASSERT(!std::regex_match(line, regionRe));
        TEST_ASSERT(!std::regex_search(line, std::regex("^region")));
    }
    return true;
}
