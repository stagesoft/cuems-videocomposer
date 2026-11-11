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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


/*
 * 1amfwm7hjze: DRM is the default display backend; X11/Wayland only by explicit flag.
 * AC1 of cuems-RELATIONS Plans/2026-11-11-vc-drm-default-backend-1amfwm7hjze.md, three layers:
 *   (a) the selection against every environment, (b) the real parser with argv arrays,
 *   (c) the explicit-desktop init path when the window cannot be created.
 */

#include "TestFramework.h"
#include "../config/ConfigurationManager.h"
#include "../display/DisplayBackendChoice.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace videocomposer;
namespace db = videocomposer::displaybackend;

namespace {

bool logsVisible() { return std::getenv("JOURNAL_STREAM") == nullptr; }

class StreamCapture {
public:
    StreamCapture()
        : oldOut_(std::cout.rdbuf(buf_.rdbuf())), oldErr_(std::cerr.rdbuf(buf_.rdbuf())) {}
    ~StreamCapture() {
        std::cout.rdbuf(oldOut_);
        std::cerr.rdbuf(oldErr_);
    }
    std::string text() const { return buf_.str(); }

private:
    std::ostringstream buf_;
    std::streambuf* oldOut_;
    std::streambuf* oldErr_;
};

int count(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size())) ++n;
    return n;
}

// Sets DISPLAY / WAYLAND_DISPLAY for one scope and puts the caller's values back.
class EnvScope {
public:
    EnvScope(const char* display, const char* wayland) {
        save("DISPLAY", display, hadX_, oldX_);
        save("WAYLAND_DISPLAY", wayland, hadW_, oldW_);
    }
    ~EnvScope() {
        restore("DISPLAY", hadX_, oldX_);
        restore("WAYLAND_DISPLAY", hadW_, oldW_);
    }

private:
    static void save(const char* var, const char* value, bool& had, std::string& old) {
        const char* cur = std::getenv(var);
        had = cur != nullptr;
        if (had) old = cur;
        if (value) ::setenv(var, value, 1); else ::unsetenv(var);
    }
    static void restore(const char* var, bool had, const std::string& old) {
        if (had) ::setenv(var, old.c_str(), 1); else ::unsetenv(var);
    }
    bool hadX_ = false, hadW_ = false;
    std::string oldX_, oldW_;
};

// parseCommandLine with a real argv; returns its result, the stored option and what it printed.
struct ParseResult {
    int rc;
    std::string stored;
    std::string printed;
    std::vector<std::string> unrecognized;
};

ParseResult parseArgs(std::vector<std::string> args) {
    args.insert(args.begin(), "cuems-videocomposer");
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    ConfigurationManager config;
    ParseResult r;
    {
        StreamCapture cap;
        r.rc = config.parseCommandLine(static_cast<int>(args.size()), argv.data());
        r.printed = cap.text();
    }
    r.stored = config.getString("display_backend", "");
    r.unrecognized = config.unrecognizedOptions();
    return r;
}

} // namespace

// (a) no option or drm -> DRM in every environment; x11/wayland -> that backend in every environment
bool test_DisplayBackend_SelectionIgnoresEnvironment() {
    struct Env { const char* display; const char* wayland; };
    const Env envs[] = {{nullptr, nullptr}, {":0", nullptr}, {nullptr, "wayland-0"}, {":0", "wayland-0"}};
    struct Row { const char* option; db::Kind expect; };
    const Row rows[] = {{"", db::Kind::Drm}, {"drm", db::Kind::Drm}, {"x11", db::Kind::X11},
                        {"wayland", db::Kind::Wayland}};
    for (const Env& e : envs) {
        EnvScope scope(e.display, e.wayland);
        for (const Row& r : rows) {
            db::Kind got = db::Kind::Wayland;
            TEST_ASSERT_TRUE(db::select(r.option, got));
            if (got != r.expect) {
                std::cout << "\n  option '" << r.option << "' DISPLAY=" << (e.display ? e.display : "-")
                          << " WAYLAND_DISPLAY=" << (e.wayland ? e.wayland : "-") << " -> " << db::name(got)
                          << ", expected " << db::name(r.expect) << "  ";
                return false;
            }
        }
    }
    return true;
}

// (a) a config-file value the parser never saw is rejected by the selection too; there is no auto (D1)
bool test_DisplayBackend_SelectionRejectsUnknownValues() {
    db::Kind k = db::Kind::Drm;
    TEST_ASSERT_FALSE(db::select("auto", k));
    TEST_ASSERT_FALSE(db::select("foo", k));
    TEST_ASSERT_FALSE(db::select("x11 ", k));
    TEST_ASSERT_TRUE(db::select("Wayland", k));
    TEST_ASSERT_TRUE(k == db::Kind::Wayland);
    TEST_ASSERT_EQ(std::string(db::acceptedValues()), std::string("drm, x11, wayland"));
    return true;
}

// (b) the valid forms set the backend: =value, separate word, any case, and drm
bool test_DisplayBackend_ParserAcceptsValidForms() {
    struct Row { std::vector<std::string> args; const char* stored; };
    const Row rows[] = {
        {{"--display-backend=x11"}, "x11"},
        {{"--display-backend", "x11"}, "x11"},
        {{"--display-backend=X11"}, "x11"},
        {{"--display-backend=drm"}, "drm"},
        {{"--display-backend=WAYLAND"}, "wayland"},
    };
    for (const Row& r : rows) {
        ParseResult p = parseArgs(r.args);
        if (p.rc != 0 || p.stored != r.stored || !p.unrecognized.empty()) {
            std::cout << "\n  " << r.args[0] << (r.args.size() > 1 ? " " + r.args[1] : "") << ": rc=" << p.rc
                      << " stored='" << p.stored << "' unrecognized=" << p.unrecognized.size() << " printed: "
                      << p.printed << "  ";
            return false;
        }
    }
    // Not given: nothing stored, so the selection takes DRM
    ParseResult none = parseArgs({"--verbose"});
    TEST_ASSERT_EQ(none.rc, 0);
    TEST_ASSERT_EQ(none.stored, std::string());
    return true;
}

// (b) auto, unknown, empty and missing values stop the start (return 2 with a stderr message), never 0 with a default
bool test_DisplayBackend_ParserRejectsBadValues() {
    const std::vector<std::vector<std::string>> rows = {
        {"--display-backend=auto"},
        {"--display-backend=foo"},
        {"--display-backend="},
        {"--display-backend"},                    // last, no value
        {"--display-backend", "--verbose"},       // the next word is a flag, not a value
    };
    for (const auto& args : rows) {
        ParseResult p = parseArgs(args);
        const bool namesFlag = p.printed.find("--display-backend") != std::string::npos;
        const bool namesValues = p.printed.find("drm, x11, wayland") != std::string::npos;
        if (p.rc != 2 || !namesFlag || !namesValues) {
            std::cout << "\n  " << args[0] << (args.size() > 1 ? " " + args[1] : "") << ": rc=" << p.rc
                      << " printed: '" << p.printed << "'  ";
            return false;
        }
    }
    return true;
}

// (c) an explicit desktop backend whose window fails: one attempt, one ERROR naming it, false; nothing else tried
bool test_DisplayBackend_ExplicitDesktopFailureTriesNothingElse() {
    for (db::Kind kind : {db::Kind::X11, db::Kind::Wayland}) {
        EnvScope scope(":0", "wayland-0");  // both set: the old code fell back from Wayland to X11 here
        std::vector<db::Kind> attempts;
        bool ok = true;
        std::string printed;
        {
            StreamCapture cap;
            ok = db::openExplicitDesktop(kind, [&](db::Kind k) {
                attempts.push_back(k);
                return false;
            });
            printed = cap.text();
        }
        TEST_ASSERT_FALSE(ok);
        TEST_ASSERT_EQ(attempts.size(), static_cast<size_t>(1));
        TEST_ASSERT_TRUE(attempts[0] == kind);
        if (logsVisible()) {
            TEST_ASSERT_EQ(count(printed, "[ERROR]"), 1);
            TEST_ASSERT_TRUE(printed.find(std::string("display backend ") + db::name(kind)) != std::string::npos);
        }
    }
    // and a backend that opens is not logged as a failure
    std::string printed;
    bool ok = false;
    {
        StreamCapture cap;
        ok = db::openExplicitDesktop(db::Kind::X11, [](db::Kind) { return true; });
        printed = cap.text();
    }
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQ(count(printed, "[ERROR]"), 0);
    return true;
}
