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
 * TestDisplayConfigColor.cpp - the color_* keys of display.conf (programme phase 3,
 * plan S3): parse, save, the loud-but-continue rules for bad values, the
 * color-only-override WARNING, and the copy onto regions under every layout.
 *
 * GL-free: DisplayConfigurationManager parses and copies; the LUT upload is the
 * DRM backend's and is checked on a rig. Log lines are asserted when they reach
 * stderr (a terminal run); under the journal only the counters are checked.
 */

#include "display/DisplayConfigurationManager.h"
#include "display/OutputColorConfig.h"
#include "TestFramework.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace videocomposer;

namespace {

// A private temp directory with the files written into it, removed on scope exit
class TempDir {
public:
    TempDir() {
        char tmpl[] = "/tmp/vc-displayconf-XXXXXX";
        const char* d = ::mkdtemp(tmpl);
        if (d) dir_ = d;
    }
    ~TempDir() {
        for (const auto& f : files_) ::unlink(f.c_str());
        if (!dir_.empty()) ::rmdir(dir_.c_str());
    }
    bool ok() const { return !dir_.empty(); }
    std::string write(const std::string& name, const std::string& text) {
        const std::string path = dir_ + "/" + name;
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return std::string();
        const bool okWrite = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        if (std::fclose(f) != 0 || !okWrite) return std::string();
        files_.push_back(path);
        return path;
    }
    std::string path(const std::string& name) {
        const std::string p = dir_ + "/" + name;
        files_.push_back(p);
        return p;
    }

private:
    std::string dir_;
    std::vector<std::string> files_;
};

std::string readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// WARNING/ERROR lines go to stderr unless the binary runs under the journal
// (Logger routes them to syslog then), so text checks are made only when they
// can be seen.
bool logsVisible() { return std::getenv("JOURNAL_STREAM") == nullptr; }

class StderrCapture {
public:
    StderrCapture() : old_(std::cerr.rdbuf(buf_.rdbuf())) {}
    ~StderrCapture() { std::cerr.rdbuf(old_); }
    std::string text() const { return buf_.str(); }

private:
    std::ostringstream buf_;
    std::streambuf* old_;
};

bool contains(const std::string& haystack, const std::string& needle) {
    if (haystack.find(needle) != std::string::npos) return true;
    std::cout << "\n  missing '" << needle << "' in:\n" << haystack << "  ";
    return false;
}

const OutputConfiguration* outConf(const DisplayConfigurationManager& m, const char* name) {
    return m.getOutputConfig(name);
}

OutputInfo outputInfo(const char* name, int w, int h) {
    OutputInfo info;
    info.name = name;
    info.width = w;
    info.height = h;
    info.connected = true;
    info.enabled = true;
    return info;
}

bool isIdentity(const OutputColor& c) {
    return !c.trimsActive() && !c.lutConfigured();
}

}  // namespace

// All six keys load, save after blend= in key=value syntax, and read back the same
bool test_DisplayConfig_ColorKeysRoundTrip() {
    TempDir tmp;
    TEST_ASSERT(tmp.ok());
    const std::string in = tmp.write("in.conf",
        "canvas_layout=custom\n"
        "[output:HDMI-A-2]\n"
        "canvas_region=3840,0,3840,2160\n"
        "resolution=3840x2160\n"
        "refresh=30\n"
        "blend=0,0,0,0,2.2\n"
        "color_brightness=0.02\n"
        "color_contrast=1.08\n"
        "color_saturation=1.3\n"
        "color_hue=-3\n"
        "color_gamma=1.05\n"
        "color_lut=p2-christie.cube\n");
    TEST_ASSERT(!in.empty());

    DisplayConfigurationManager m;
    TEST_ASSERT(m.loadFromFile(in));
    TEST_ASSERT_EQ(m.lastLoadReport().colorErrors, 0);
    TEST_ASSERT_EQ(m.lastLoadReport().colorWarnings, 0);
    TEST_ASSERT(m.lastLoadReport().colorWithoutCanvasRegion.empty());
    const OutputConfiguration* oc = outConf(m, "HDMI-A-2");
    TEST_ASSERT(oc != nullptr);
    TEST_ASSERT(oc->hasColor());
    TEST_ASSERT_EQ(oc->color.brightness, 0.02f);
    TEST_ASSERT_EQ(oc->color.contrast, 1.08f);
    TEST_ASSERT_EQ(oc->color.saturation, 1.3f);
    TEST_ASSERT_EQ(oc->color.hue, -3.0f);
    TEST_ASSERT_EQ(oc->color.gamma, 1.05f);
    TEST_ASSERT_EQ(oc->color.lutValue, std::string("p2-christie.cube"));
    TEST_ASSERT_EQ(oc->color.lutPath, std::string("/etc/cuems/luts/p2-christie.cube"));
    TEST_ASSERT(oc->color.lutStatus == LutStatus::None);  // set by the backend when loaded
    TEST_ASSERT_EQ(oc->canvasWidth, 3840);                // the other keys are untouched

    // Paste-ready line: what the start-up INFO line and saveToFile print
    TEST_ASSERT_EQ(colorConfigLine(oc->color),
                   std::string("color_brightness=0.02 color_contrast=1.08 color_saturation=1.3"
                               " color_hue=-3 color_gamma=1.05 color_lut=p2-christie.cube"));

    const std::string out = tmp.path("out.conf");
    TEST_ASSERT(m.saveToFile(out));
    const std::string saved = readAll(out);
    TEST_ASSERT(contains(saved, "color_contrast=1.08\n"));
    TEST_ASSERT(contains(saved, "color_lut=p2-christie.cube\n"));
    const auto blendAt = saved.find("\nblend=");
    const auto firstColorAt = saved.find("\ncolor_");
    const auto enabledAt = saved.find("\nenabled=");
    TEST_ASSERT(blendAt != std::string::npos && firstColorAt != std::string::npos);
    TEST_ASSERT(blendAt < firstColorAt && firstColorAt < enabledAt);
    TEST_ASSERT(saved.find(" = ") == std::string::npos);  // never a spaced key

    DisplayConfigurationManager again;
    TEST_ASSERT(again.loadFromFile(out));
    const OutputConfiguration* oc2 = outConf(again, "HDMI-A-2");
    TEST_ASSERT(oc2 != nullptr);
    TEST_ASSERT_EQ(oc2->color.brightness, oc->color.brightness);
    TEST_ASSERT_EQ(oc2->color.contrast, oc->color.contrast);
    TEST_ASSERT_EQ(oc2->color.saturation, oc->color.saturation);
    TEST_ASSERT_EQ(oc2->color.hue, oc->color.hue);
    TEST_ASSERT_EQ(oc2->color.gamma, oc->color.gamma);
    TEST_ASSERT_EQ(oc2->color.lutPath, oc->color.lutPath);
    TEST_ASSERT_EQ(oc2->color.lutValue, oc->color.lutValue);
    TEST_ASSERT_EQ(again.lastLoadReport().colorErrors, 0);
    return true;
}

// Any subset of the keys: the rest stay at identity, and save writes only what is set
bool test_DisplayConfig_PartialColorKeys() {
    TempDir tmp;
    TEST_ASSERT(tmp.ok());
    const std::string in = tmp.write("in.conf",
        "canvas_layout=custom\n"
        "[output:HDMI-A-1]\n"
        "canvas_region=0,0,1920,1080\n"
        "[output:HDMI-A-2]\n"
        "canvas_region=1920,0,1920,1080\n"
        "color_gamma=1.05\n");
    DisplayConfigurationManager m;
    TEST_ASSERT(m.loadFromFile(in));

    const OutputConfiguration* plain = outConf(m, "HDMI-A-1");
    TEST_ASSERT(plain != nullptr && !plain->hasColor() && isIdentity(plain->color));

    const OutputConfiguration* oc = outConf(m, "HDMI-A-2");
    TEST_ASSERT(oc != nullptr && oc->hasColor());
    TEST_ASSERT_EQ(oc->color.gamma, 1.05f);
    TEST_ASSERT_EQ(oc->color.brightness, 0.0f);
    TEST_ASSERT_EQ(oc->color.contrast, 1.0f);
    TEST_ASSERT_EQ(oc->color.saturation, 1.0f);
    TEST_ASSERT_EQ(oc->color.hue, 0.0f);
    TEST_ASSERT(!oc->color.lutConfigured());

    // A LUT alone is color too
    const std::string lutOnly = tmp.write("lut.conf",
        "[output:DP-1]\ncanvas_region=0,0,1920,1080\ncolor_lut=warm.cube\n");
    DisplayConfigurationManager l;
    TEST_ASSERT(l.loadFromFile(lutOnly));
    TEST_ASSERT(outConf(l, "DP-1")->hasColor());
    TEST_ASSERT(!outConf(l, "DP-1")->color.trimsActive());

    const std::string out = tmp.path("out.conf");
    TEST_ASSERT(m.saveToFile(out));
    const std::string saved = readAll(out);
    TEST_ASSERT(contains(saved, "color_gamma=1.05\n"));
    // exactly one color line in the whole file (HDMI-A-1 has none, the other trims are at identity)
    std::size_t count = 0;
    for (std::size_t at = saved.find("color_"); at != std::string::npos; at = saved.find("color_", at + 1)) {
        ++count;
    }
    TEST_ASSERT_EQ(count, static_cast<std::size_t>(1));
    return true;
}

// The value loses surrounding whitespace and the '\r' of a CRLF file; the key does not
bool test_DisplayConfig_ValueCrlfAndTrailingSpaceStripped() {
    TempDir tmp;
    TEST_ASSERT(tmp.ok());
    const std::string in = tmp.write("crlf.conf",
        "canvas_layout=custom\r\n"
        "[output:HDMI-A-2]\r\n"
        "canvas_region=0,0,1920,1080\r\n"
        "color_contrast=1.08\r\n"
        "color_gamma=  1.05 \t\r\n"
        "color_hue=\t+12  \r\n"
        "color_lut= p2.cube \r\n");
    DisplayConfigurationManager m;
    TEST_ASSERT(m.loadFromFile(in));
    TEST_ASSERT_EQ(m.lastLoadReport().colorErrors, 0);
    TEST_ASSERT_EQ(m.lastLoadReport().colorWarnings, 0);
    const OutputConfiguration* oc = outConf(m, "HDMI-A-2");
    TEST_ASSERT(oc != nullptr);
    TEST_ASSERT_EQ(oc->color.contrast, 1.08f);
    TEST_ASSERT_EQ(oc->color.gamma, 1.05f);
    TEST_ASSERT_EQ(oc->color.hue, 12.0f);
    TEST_ASSERT_EQ(oc->color.lutValue, std::string("p2.cube"));
    TEST_ASSERT_EQ(oc->color.lutPath, std::string("/etc/cuems/luts/p2.cube"));
    return true;
}

// Unparsable values: one ERROR each naming output, key and text; the field keeps its
// value and the rest of the file still applies
bool test_DisplayConfig_BadColorValueIsLoudAndIgnored() {
    TempDir tmp;
    TEST_ASSERT(tmp.ok());
    const std::string in = tmp.write("bad.conf",
        "canvas_layout=custom\n"
        "[output:HDMI-A-1]\n"
        "color_contrast=abc\n"
        "color_gamma=1.2x\n"
        "color_brightness=0,1\n"
        "color_saturation=\n"
        "color_hue=10\n"
        "canvas_region=0,0,1920,1080\n"
        "[output:HDMI-A-2]\n"
        "canvas_region=1920,0,1920,1080\n"
        "color_contrast=1.2\n");
    DisplayConfigurationManager m;
    std::string log;
    {
        StderrCapture capture;
        TEST_ASSERT(m.loadFromFile(in));
        log = capture.text();
    }
    TEST_ASSERT_EQ(m.lastLoadReport().colorErrors, 4);
    TEST_ASSERT_EQ(m.lastLoadReport().colorWarnings, 0);
    const OutputConfiguration* oc = outConf(m, "HDMI-A-1");
    TEST_ASSERT(oc != nullptr);
    TEST_ASSERT_EQ(oc->color.contrast, 1.0f);
    TEST_ASSERT_EQ(oc->color.gamma, 1.0f);
    TEST_ASSERT_EQ(oc->color.brightness, 0.0f);
    TEST_ASSERT_EQ(oc->color.saturation, 1.0f);
    TEST_ASSERT_EQ(oc->color.hue, 10.0f);          // the good key next to them applied
    TEST_ASSERT_EQ(oc->canvasWidth, 1920);         // keys after the bad ones still parsed
    TEST_ASSERT_EQ(outConf(m, "HDMI-A-2")->color.contrast, 1.2f);  // other outputs too

    if (logsVisible()) {
        TEST_ASSERT(contains(log, "[ERROR] OutputColor: HDMI-A-1 color_contrast=abc in " + in +
                                  ": not a finite number — ignored, color_contrast stays 1"));
        TEST_ASSERT(contains(log, "OutputColor: HDMI-A-1 color_gamma=1.2x in "));
        TEST_ASSERT(contains(log, "OutputColor: HDMI-A-1 color_brightness=0,1 in "));
        TEST_ASSERT(contains(log, "OutputColor: HDMI-A-1 color_saturation= in "));
    }
    return true;
}

// Out of range: clamped to the layer ranges with a WARNING naming the value used;
// hue is folded, which is not a clamp
bool test_DisplayConfig_OutOfRangeClampedWithWarning() {
    TempDir tmp;
    TEST_ASSERT(tmp.ok());
    const std::string in = tmp.write("range.conf",
        "[output:HDMI-A-1]\n"
        "canvas_region=0,0,1920,1080\n"
        "color_contrast=5\n"
        "color_gamma=0.01\n"
        "color_brightness=-3\n"
        "color_saturation=2\n"
        "color_hue=540\n");
    DisplayConfigurationManager m;
    std::string log;
    {
        StderrCapture capture;
        TEST_ASSERT(m.loadFromFile(in));
        log = capture.text();
    }
    TEST_ASSERT_EQ(m.lastLoadReport().colorWarnings, 3);
    TEST_ASSERT_EQ(m.lastLoadReport().colorErrors, 0);
    const OutputConfiguration* oc = outConf(m, "HDMI-A-1");
    TEST_ASSERT_EQ(oc->color.contrast, 2.0f);
    TEST_ASSERT_EQ(oc->color.gamma, 0.1f);
    TEST_ASSERT_EQ(oc->color.brightness, -1.0f);
    TEST_ASSERT_EQ(oc->color.saturation, 2.0f);    // the limit itself is in range
    TEST_ASSERT_EQ(oc->color.hue, 180.0f);         // folded, no warning

    if (logsVisible()) {
        TEST_ASSERT(contains(log, "[WARNING] OutputColor: HDMI-A-1 color_contrast=5 in " + in +
                                  ": out of range (0 to 2) — using 2"));
        TEST_ASSERT(contains(log, "OutputColor: HDMI-A-1 color_gamma=0.01 in " + in +
                                  ": out of range (0.1 to 3) — using 0.1"));
        TEST_ASSERT(contains(log, "OutputColor: HDMI-A-1 color_brightness=-3 in "));
        TEST_ASSERT(log.find("color_hue") == std::string::npos);
        TEST_ASSERT(log.find("color_saturation") == std::string::npos);
    }
    return true;
}

// inf / nan (which from_chars accepts) and overflow are refused, not clamped
bool test_DisplayConfig_NonFiniteRefused() {
    TempDir tmp;
    TEST_ASSERT(tmp.ok());
    const std::string in = tmp.write("nonfinite.conf",
        "[output:HDMI-A-1]\n"
        "canvas_region=0,0,1920,1080\n"
        "color_contrast=inf\n"
        "color_gamma=nan\n"
        "color_brightness=-inf\n"
        "color_saturation=1e39\n"
        "color_hue=+inf\n");
    DisplayConfigurationManager m;
    {
        StderrCapture capture;
        TEST_ASSERT(m.loadFromFile(in));
    }
    TEST_ASSERT_EQ(m.lastLoadReport().colorErrors, 5);
    TEST_ASSERT_EQ(m.lastLoadReport().colorWarnings, 0);
    const OutputConfiguration* oc = outConf(m, "HDMI-A-1");
    TEST_ASSERT(isIdentity(oc->color));
    TEST_ASSERT(!oc->hasColor());
    return true;
}

// Unknown keys, including look-alikes of the color keys, are still ignored silently
bool test_DisplayConfig_UnknownKeysStillIgnored() {
    TempDir tmp;
    TEST_ASSERT(tmp.ok());
    const std::string in = tmp.write("unknown.conf",
        "future_global=1\n"
        "[output:HDMI-A-1]\n"
        "color_foo=1\n"
        "colour_contrast=1.5\n"
        "COLOR_CONTRAST=1.5\n"
        "color_=1\n"
        "bogus=yes\n"
        "[output:HDMI-A-2]\n"
        "canvas_region=1920,0,1920,1080\n"
        "color_contrast=1.5\n"
        "future_key=whatever\n");
    DisplayConfigurationManager m;
    std::string log;
    {
        StderrCapture capture;
        TEST_ASSERT(m.loadFromFile(in));
        log = capture.text();
    }
    TEST_ASSERT_EQ(m.lastLoadReport().colorErrors, 0);
    TEST_ASSERT_EQ(m.lastLoadReport().colorWarnings, 0);
    // an unknown color_-looking key is not a color key, so no color-only WARNING either
    TEST_ASSERT(m.lastLoadReport().colorWithoutCanvasRegion.empty());
    TEST_ASSERT(isIdentity(outConf(m, "HDMI-A-1")->color));
    TEST_ASSERT_EQ(outConf(m, "HDMI-A-2")->color.contrast, 1.5f);
    if (logsVisible()) {
        TEST_ASSERT(log.empty());
    }
    TEST_ASSERT(!isOutputColorKey("color_foo"));
    TEST_ASSERT(!isOutputColorKey("color_"));
    TEST_ASSERT(isOutputColorKey("color_lut"));
    TEST_ASSERT(isOutputColorKey("color_hue"));
    return true;
}

// Today's rule, pinned: the key is the text before '=' untrimmed, so an indented or
// spaced key is unknown to the VC (while the engine's configparser would honour it)
bool test_DisplayConfig_SpacedKeyIsUnknown() {
    TempDir tmp;
    TEST_ASSERT(tmp.ok());
    const std::string in = tmp.write("spaced.conf",
        "canvas_layout=custom\n"
        "[output:HDMI-A-1]\n"
        "canvas_region = 0,0,1920,1080\n"
        " color_gamma=1.5\n"
        "color_contrast =1.5\n"
        "color_saturation=1.5\n");
    DisplayConfigurationManager m;
    {
        StderrCapture capture;
        TEST_ASSERT(m.loadFromFile(in));
    }
    const OutputConfiguration* oc = outConf(m, "HDMI-A-1");
    TEST_ASSERT(oc != nullptr);
    TEST_ASSERT_EQ(oc->canvasWidth, 0);          // spaced canvas_region: not read
    TEST_ASSERT_EQ(oc->color.gamma, 1.0f);       // indented key: not read
    TEST_ASSERT_EQ(oc->color.contrast, 1.0f);    // spaced key: not read
    TEST_ASSERT_EQ(oc->color.saturation, 1.5f);  // column-0 key: read
    // ...and for the VC this section has color and no canvas_region
    TEST_ASSERT_EQ(m.lastLoadReport().colorWithoutCanvasRegion.size(), static_cast<std::size_t>(1));
    return true;
}

// Color reaches the regions under an auto layout too (blend= does not), and only
// connected outputs with color are touched
bool test_DisplayConfig_ColorWithAutoLayout() {
    TempDir tmp;
    TEST_ASSERT(tmp.ok());
    const std::string in = tmp.write("auto.conf",
        "canvas_layout=horizontal\n"
        "[output:HDMI-A-1]\n"
        "canvas_region=0,0,1920,1080\n"
        "[output:HDMI-A-2]\n"
        "canvas_region=1920,0,1920,1080\n"
        "blend=100,0,0,0,2.2\n"
        "color_contrast=1.2\n"
        "color_lut=/srv/luts/warm.cube\n"
        "[output:DP-1]\n"
        "canvas_region=3840,0,1920,1080\n"
        "color_gamma=1.1\n");
    DisplayConfigurationManager m;
    TEST_ASSERT(m.loadFromFile(in));
    TEST_ASSERT(m.getCanvasLayout() == CanvasLayout::AUTO_HORIZONTAL);

    // DP-1 is declared but not connected
    std::vector<OutputInfo> connected{outputInfo("HDMI-A-1", 1920, 1080), outputInfo("HDMI-A-2", 1920, 1080)};
    std::vector<OutputRegion> regions = m.generateOutputRegions(connected);
    TEST_ASSERT_EQ(regions.size(), static_cast<std::size_t>(2));
    TEST_ASSERT(!regions[1].hasBlending());   // blend= reaches a region only under custom (unchanged)

    // A stale state from before must be replaced, not kept
    auto staleDraw = regions[1].colorDraw;
    staleDraw->failed = true;
    regions[1].color.lutStatus = LutStatus::Ok;

    TEST_ASSERT_EQ(m.applyColorToRegions(regions), 1);
    TEST_ASSERT(isIdentity(regions[0].color));
    TEST_ASSERT_EQ(regions[1].name, std::string("HDMI-A-2"));
    TEST_ASSERT_EQ(regions[1].color.contrast, 1.2f);
    TEST_ASSERT_EQ(regions[1].color.lutPath, std::string("/srv/luts/warm.cube"));
    TEST_ASSERT(regions[1].color.lutStatus == LutStatus::None);   // the backend loads it
    TEST_ASSERT(regions[1].lut == nullptr);
    TEST_ASSERT(regions[1].colorDraw != nullptr && regions[1].colorDraw != staleDraw);
    TEST_ASSERT(!regions[1].colorDraw->failed);
    TEST_ASSERT(regions[1].hasColor());                          // trims active

    // Same under custom
    const std::string custom = tmp.write("custom.conf",
        "canvas_layout=custom\n"
        "[output:HDMI-A-2]\n"
        "canvas_region=0,0,1920,1080\n"
        "color_hue=-3\n");
    DisplayConfigurationManager c;
    TEST_ASSERT(c.loadFromFile(custom));
    std::vector<OutputRegion> customRegions = c.generateOutputRegions(connected);
    TEST_ASSERT_EQ(c.applyColorToRegions(customRegions), 1);
    for (const auto& r : customRegions) {
        TEST_ASSERT(r.name == "HDMI-A-2" ? r.color.hue == -3.0f : isIdentity(r.color));
    }
    return true;
}

// A section with color_* keys and no canvas_region gets one WARNING naming the
// copy-first procedure
bool test_DisplayConfig_ColorWithoutCanvasRegionWarns() {
    TempDir tmp;
    TEST_ASSERT(tmp.ok());
    const std::string in = tmp.write("coloronly.conf",
        "[output:HDMI-A-1]\n"
        "color_contrast=1.1\n"
        "color_gamma=1.2\n"
        "[output:HDMI-A-2]\n"
        "canvas_region=1920,0,1920,1080\n"
        "color_contrast=1.1\n"
        "[output:DP-1]\n"
        "canvas_region=3840,0,1920,1080\n");
    DisplayConfigurationManager m;
    std::string log;
    {
        StderrCapture capture;
        TEST_ASSERT(m.loadFromFile(in));
        log = capture.text();
    }
    const auto& listed = m.lastLoadReport().colorWithoutCanvasRegion;
    TEST_ASSERT_EQ(listed.size(), static_cast<std::size_t>(1));
    TEST_ASSERT_EQ(listed[0], std::string("HDMI-A-1"));
    TEST_ASSERT_EQ(outConf(m, "HDMI-A-1")->color.contrast, 1.1f);  // still parsed
    if (logsVisible()) {
        TEST_ASSERT(contains(log, "[WARNING] OutputColor: [output:HDMI-A-1] in " + in +
                                  " has color_* keys but no canvas_region"));
        TEST_ASSERT(contains(log, "Copy /run/cuems/display.conf to /etc/cuems/display.conf while the"
                                  " videocomposer is running, then add the color_* keys to that copy"));
        // one line per section, not per key
        TEST_ASSERT_EQ(log.find("no canvas_region"), log.rfind("no canvas_region"));
    }
    return true;
}

// color_lut: relative under /etc/cuems/luts, '..' refused (ERROR, no LUT), absolute kept
bool test_DisplayConfig_LutPathRules() {
    TempDir tmp;
    TEST_ASSERT(tmp.ok());
    const std::string in = tmp.write("luts.conf",
        "[output:A]\ncanvas_region=0,0,10,10\ncolor_lut=p2.cube\n"
        "[output:B]\ncanvas_region=0,0,10,10\ncolor_lut=site/p2.cube\n"
        "[output:C]\ncanvas_region=0,0,10,10\ncolor_lut=../p2.cube\n"
        "[output:D]\ncanvas_region=0,0,10,10\ncolor_lut=/srv/luts/p2.cube\n"
        "[output:E]\ncanvas_region=0,0,10,10\ncolor_lut=good.cube\ncolor_lut=site/../../etc/shadow\n");
    DisplayConfigurationManager m;
    std::string log;
    {
        StderrCapture capture;
        TEST_ASSERT(m.loadFromFile(in));
        log = capture.text();
    }
    TEST_ASSERT_EQ(outConf(m, "A")->color.lutPath, std::string("/etc/cuems/luts/p2.cube"));
    TEST_ASSERT_EQ(outConf(m, "A")->color.lutValue, std::string("p2.cube"));
    TEST_ASSERT_EQ(outConf(m, "B")->color.lutPath, std::string("/etc/cuems/luts/site/p2.cube"));
    TEST_ASSERT(!outConf(m, "C")->color.lutConfigured());
    TEST_ASSERT(!outConf(m, "C")->hasColor());
    TEST_ASSERT_EQ(outConf(m, "D")->color.lutPath, std::string("/srv/luts/p2.cube"));
    TEST_ASSERT_EQ(outConf(m, "D")->color.lutValue, std::string("/srv/luts/p2.cube"));
    TEST_ASSERT(!outConf(m, "E")->color.lutConfigured());   // a refused line leaves no LUT
    TEST_ASSERT_EQ(m.lastLoadReport().colorErrors, 2);
    if (logsVisible()) {
        TEST_ASSERT(contains(log, "[ERROR] OutputColor: C color_lut=../p2.cube in " + in +
                                  ": refused: '..' is not allowed in a relative LUT path"
                                  " — no LUT configured"));
    }

    // The resolver's base directory is a parameter of the helper (tests, a future dir)
    OutputColor color;
    std::string detail;
    TEST_ASSERT(applyOutputColorKey("color_lut", "x.cube", color, detail, "/tmp/luts") ==
                ColorKeyResult::Applied);
    TEST_ASSERT_EQ(color.lutPath, std::string("/tmp/luts/x.cube"));
    return true;
}
