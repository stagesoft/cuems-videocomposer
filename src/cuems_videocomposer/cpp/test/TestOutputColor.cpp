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
 * TestOutputColor.cpp - per-output color: trims, number parsing, the .cube parser
 * and its file safety (programme phase 3, plan S1).
 *
 * Everything here is GL-free. The properties that matter most are the ones that
 * fail silently on a rig: a LUT read with its axes transposed looks plausible, a
 * hue that loops hangs the render thread, and a FIFO on the LUT path blocks it.
 */

#include "display/CubeLut.h"
#include "display/OutputColor.h"
#include "TestFramework.h"

#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace videocomposer;

namespace {

bool near(float a, float b, float eps = 1e-6f) { return std::fabs(a - b) <= eps; }

// An identity LUT of size n, red fastest
std::string identityCube(int n, const std::string& header = "") {
    std::ostringstream s;
    s << header << "LUT_3D_SIZE " << n << "\n";
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r)
                s << float(r) / (n - 1) << " " << float(g) / (n - 1) << " " << float(b) / (n - 1) << "\n";
    return s.str();
}

bool parses(const std::string& text, CubeLut& lut) {
    std::string err;
    const bool ok = parseCubeLut(text, lut, err);
    if (!ok) std::cerr << "  parse error: " << err << "\n";
    return ok;
}

bool refused(const std::string& text, const std::string& needle = "") {
    CubeLut lut;
    std::string err;
    if (parseCubeLut(text, lut, err)) return false;
    if (!needle.empty() && err.find(needle) == std::string::npos) {
        std::cerr << "  refused, but the reason '" << err << "' lacks '" << needle << "'\n";
        return false;
    }
    return true;
}

// A private temp directory, removed by the caller
std::string makeTempDir() {
    char tmpl[] = "/tmp/vc-outputcolor-XXXXXX";
    const char* d = ::mkdtemp(tmpl);
    return d ? std::string(d) : std::string();
}

bool writeFile(const std::string& path, const std::string& text) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

}  // namespace

// ---- .cube parser ---------------------------------------------------------

bool test_CubeLut_ParsesHeaderAndData() {
    CubeLut lut;
    TEST_ASSERT(parses("TITLE \"warm tint\"\n# a comment\n\n" + identityCube(2), lut));
    TEST_ASSERT_EQ(lut.size, 2);
    TEST_ASSERT_EQ(lut.title, std::string("\"warm tint\""));
    TEST_ASSERT_EQ(lut.rgb.size(), std::size_t(2 * 2 * 2 * 3));
    TEST_ASSERT_EQ(lut.clampedValues, std::size_t(0));
    for (int c = 0; c < 3; ++c) {
        TEST_ASSERT(lut.domainMin[c] == 0.0f);
        TEST_ASSERT(lut.domainMax[c] == 1.0f);
    }
    return true;
}

// The .cube order is red fastest: entry (r, g, b) is line r + N*(g + N*b). A transposed
// read gives a plausible-looking LUT that swaps red and blue, so pin it with values that
// are different on every axis.
bool test_CubeLut_RedVariesFastest() {
    std::ostringstream s;
    s << "LUT_3D_SIZE 2\n";
    for (int i = 0; i < 8; ++i) s << 0.1f * i << " " << 0.01f * i << " " << 0.001f * i << "\n";
    CubeLut lut;
    TEST_ASSERT(parses(s.str(), lut));
    auto at = [&](int r, int g, int b, int c) { return lut.rgb[(r + 2 * (g + 2 * b)) * 3 + c]; };
    // line index of (r=1, g=0, b=0) is 1; of (0,1,0) is 2; of (0,0,1) is 4
    TEST_ASSERT(near(at(1, 0, 0, 0), 0.1f));
    TEST_ASSERT(near(at(0, 1, 0, 0), 0.2f));
    TEST_ASSERT(near(at(0, 0, 1, 0), 0.4f));
    TEST_ASSERT(near(at(1, 1, 1, 2), 0.007f));
    return true;
}

bool test_CubeLut_DomainMinMax() {
    CubeLut lut;
    TEST_ASSERT(parses(identityCube(2, "DOMAIN_MIN -0.1 0 0.2\nDOMAIN_MAX 1.1 1 1.5\n"), lut));
    TEST_ASSERT(near(lut.domainMin[0], -0.1f));
    TEST_ASSERT(near(lut.domainMin[2], 0.2f));
    TEST_ASSERT(near(lut.domainMax[0], 1.1f));
    TEST_ASSERT(near(lut.domainMax[2], 1.5f));
    return true;
}

bool test_CubeLut_InputRangeMapsToDomain() {
    CubeLut lut;
    TEST_ASSERT(parses(identityCube(2, "LUT_3D_INPUT_RANGE -0.05 1.05\n"), lut));
    for (int c = 0; c < 3; ++c) {
        TEST_ASSERT(near(lut.domainMin[c], -0.05f));
        TEST_ASSERT(near(lut.domainMax[c], 1.05f));
    }
    // Both spellings, agreeing: accepted
    TEST_ASSERT(parses(identityCube(2, "LUT_3D_INPUT_RANGE 0 1\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n"), lut));
    return true;
}

bool test_CubeLut_RejectsDomainAndInputRangeDisagree() {
    TEST_ASSERT(refused(identityCube(2, "LUT_3D_INPUT_RANGE 0 1\nDOMAIN_MAX 1 1 2\n"), "ambiguous"));
    return true;
}

bool test_CubeLut_RejectsDomainMaxNotAboveMin() {
    TEST_ASSERT(refused(identityCube(2, "DOMAIN_MIN 0 0.5 0\nDOMAIN_MAX 1 0.5 1\n"), "DOMAIN_MAX"));
    TEST_ASSERT(refused(identityCube(2, "LUT_3D_INPUT_RANGE 1 0\n"), "DOMAIN_MAX"));
    return true;
}

bool test_CubeLut_RejectsSizeOutOfRange() {
    TEST_ASSERT(refused("LUT_3D_SIZE 1\n0 0 0\n", "out of range"));
    TEST_ASSERT(refused("LUT_3D_SIZE 66\n", "out of range"));
    TEST_ASSERT(refused("LUT_3D_SIZE 2.5\n", "integer"));
    TEST_ASSERT(refused("0 0 0\n", "before LUT_3D_SIZE"));
    TEST_ASSERT(refused("# nothing\n", "no LUT_3D_SIZE"));
    return true;
}

// The whole file is read: too few lines and any line after the N^3-th are both refused
bool test_CubeLut_RejectsShortOrLongData() {
    std::string full = identityCube(2);
    std::string shortText = full.substr(0, full.rfind('\n', full.size() - 2) + 1);
    TEST_ASSERT(refused(shortText, "expected 8"));
    TEST_ASSERT(refused(full + "0 0 0\n", "more than 8"));
    // Trailing blank lines and comments after the data are fine
    CubeLut lut;
    TEST_ASSERT(parses(full + "\n# end\n\n", lut));
    return true;
}

bool test_CubeLut_RejectsFourTokenLine() {
    std::string text = identityCube(2);
    text.replace(text.find("0 0 0\n"), 6, "0 0 0 1\n");
    TEST_ASSERT(refused(text, "expected 3"));
    TEST_ASSERT(refused("LUT_3D_SIZE 2\n0 0\n", "expected 3"));
    return true;
}

bool test_CubeLut_Rejects1D() {
    TEST_ASSERT(refused("LUT_1D_SIZE 1024\n", "1D"));
    return true;
}

bool test_CubeLut_RejectsUnknownKeyword() {
    TEST_ASSERT(refused(identityCube(2, "LUT_IN_VIDEO_RANGE\n"), "unknown keyword 'LUT_IN_VIDEO_RANGE'"));
    // A keyword between the data lines is not data
    std::string text = identityCube(2);
    text.insert(text.find('\n', text.find("0 0 0")) + 1, "DOMAIN_MIN 0 0 0\n");
    TEST_ASSERT(refused(text, "after the data"));
    return true;
}

// from_chars accepts "inf" and "nan"; the finite check is what refuses them
bool test_CubeLut_RejectsNonFinite() {
    std::string text = identityCube(2);
    std::string a = text, b = text, c = text;
    a.replace(a.find("0 0 0\n"), 6, "inf 0 0\n");
    b.replace(b.find("0 0 0\n"), 6, "0 nan 0\n");
    c.replace(c.find("0 0 0\n"), 6, "0 0 1e999\n");
    TEST_ASSERT(refused(a, "non-finite"));
    TEST_ASSERT(refused(b, "non-finite"));
    TEST_ASSERT(refused(c, "non-finite"));
    TEST_ASSERT(refused(identityCube(2, "DOMAIN_MIN nan 0 0\n"), "DOMAIN_MIN"));
    return true;
}

bool test_CubeLut_AcceptsBomTabsScientificCrlf() {
    std::string body = "\xEF\xBB\xBFTITLE \"x\"\r\nLUT_3D_SIZE\t2\r\n";
    for (int i = 0; i < 8; ++i) body += "  5e-1\t\t+1.0E0  0.25 \r\n";
    CubeLut lut;
    TEST_ASSERT(parses(body, lut));
    TEST_ASSERT_EQ(lut.size, 2);
    TEST_ASSERT(near(lut.rgb[0], 0.5f));
    TEST_ASSERT(near(lut.rgb[1], 1.0f));
    TEST_ASSERT(near(lut.rgb[2], 0.25f));
    return true;
}

// HDR-authored values are clamped to [0, 1] and counted (the outputs are SDR scanout)
bool test_CubeLut_ClampsOutOfRangeAndCounts() {
    std::string text = identityCube(2);
    text.replace(text.find("0 0 0\n"), 6, "-0.5 1.25 0\n");
    CubeLut lut;
    TEST_ASSERT(parses(text, lut));
    TEST_ASSERT_EQ(lut.clampedValues, std::size_t(2));
    TEST_ASSERT(lut.rgb[0] == 0.0f);
    TEST_ASSERT(lut.rgb[1] == 1.0f);
    return true;
}

// A FIFO would block a plain open() and /dev/zero has st_size 0 and never ends:
// both are refused by type, before a byte is read
bool test_CubeLut_RefusesNonRegularFile() {
    const std::string dir = makeTempDir();
    TEST_ASSERT(!dir.empty());
    const std::string fifo = dir + "/lut.cube";
    TEST_ASSERT(::mkfifo(fifo.c_str(), 0600) == 0);
    CubeLut lut;
    const auto t0 = std::chrono::steady_clock::now();
    CubeLoadResult r = loadCubeLutFile(fifo, lut);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ::unlink(fifo.c_str());
    ::rmdir(dir.c_str());
    TEST_ASSERT(!r.ok);
    TEST_ASSERT(!r.notFound);
    TEST_ASSERT(r.error.find("not a regular file") != std::string::npos);
    TEST_ASSERT(ms < 1000.0);

    r = loadCubeLutFile("/dev/zero", lut);
    TEST_ASSERT(!r.ok);
    TEST_ASSERT(r.error.find("not a regular file") != std::string::npos);

    r = loadCubeLutFile("/nonexistent-dir-vc/x.cube", lut);
    TEST_ASSERT(!r.ok);
    TEST_ASSERT(r.notFound);
    return true;
}

bool test_CubeLut_RejectsDotDotRelativePath() {
    std::string out, err;
    TEST_ASSERT(resolveConfigLutPath("p2.cube", out, err));
    TEST_ASSERT_EQ(out, std::string("/etc/cuems/luts/p2.cube"));
    TEST_ASSERT(resolveConfigLutPath("site/p2.cube", out, err));
    TEST_ASSERT_EQ(out, std::string("/etc/cuems/luts/site/p2.cube"));
    TEST_ASSERT(resolveConfigLutPath("/srv/luts/p2.cube", out, err));
    TEST_ASSERT_EQ(out, std::string("/srv/luts/p2.cube"));
    TEST_ASSERT_FALSE(resolveConfigLutPath("../p2.cube", out, err));
    TEST_ASSERT_FALSE(resolveConfigLutPath("site/../../etc/shadow", out, err));
    TEST_ASSERT_FALSE(resolveConfigLutPath("site/..", out, err));
    TEST_ASSERT_FALSE(resolveConfigLutPath("", out, err));
    TEST_ASSERT(resolveConfigLutPath("a..b.cube", out, err));  // not a ".." component

    // OSC: bare names only
    TEST_ASSERT(resolveOscLutName("p2-christie.cube", out, err));
    TEST_ASSERT_EQ(out, std::string("/etc/cuems/luts/p2-christie.cube"));
    TEST_ASSERT_FALSE(resolveOscLutName("/etc/shadow", out, err));
    TEST_ASSERT_FALSE(resolveOscLutName("site/p2.cube", out, err));
    TEST_ASSERT_FALSE(resolveOscLutName("..cube", out, err));
    TEST_ASSERT_FALSE(resolveOscLutName("p2.txt", out, err));
    TEST_ASSERT_FALSE(resolveOscLutName(".cube", out, err));
    TEST_ASSERT_FALSE(resolveOscLutName(std::string("p2\n.cube"), out, err));
    return true;
}

// A file over the cap is refused from fstat, without reading it
bool test_CubeLut_SizeCap16MiB() {
    const std::string dir = makeTempDir();
    TEST_ASSERT(!dir.empty());
    const std::string path = dir + "/big.cube";
    const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    TEST_ASSERT(fd >= 0);
    const bool sized = ::ftruncate(fd, off_t(CubeLut::kMaxFileBytes) + 1) == 0;
    ::close(fd);
    CubeLut lut;
    const CubeLoadResult r = loadCubeLutFile(path, lut);
    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
    TEST_ASSERT(sized);
    TEST_ASSERT(!r.ok);
    TEST_ASSERT(r.error.find("16 MiB") != std::string::npos);
    return true;
}

// Measured, not asserted: a 65^3 LUT is the worst case a LUT change costs the render thread
bool test_CubeLut_ParseTime65() {
    const std::string dir = makeTempDir();
    TEST_ASSERT(!dir.empty());
    const std::string path = dir + "/id65.cube";
    const std::string text = identityCube(65, "TITLE \"identity 65\"\n");
    TEST_ASSERT(writeFile(path, text));
    CubeLut lut;
    const CubeLoadResult r = loadCubeLutFile(path, lut);
    ::unlink(path.c_str());
    ::rmdir(dir.c_str());
    TEST_ASSERT(r.ok);
    TEST_ASSERT_EQ(lut.entryCount(), std::size_t(65 * 65 * 65));
    std::cout << "[65^3: " << text.size() / 1024 << " KiB, load+parse " << r.parseMs << " ms] ";
    return true;
}

// ---- trims and numbers ----------------------------------------------------

bool test_OutputColor_DefaultsAreIdentity() {
    OutputColor c;
    TEST_ASSERT_FALSE(c.trimsActive());
    TEST_ASSERT_FALSE(c.lutConfigured());
    TEST_ASSERT(c.lutStatus == LutStatus::None);
    float r = 0.25f, g = 0.5f, b = 0.75f;
    c.apply(r, g, b);
    TEST_ASSERT(near(r, 0.25f) && near(g, 0.5f) && near(b, 0.75f));
    for (const auto& spec : kColorTrimSpecs) TEST_ASSERT(c.get(spec.trim) == spec.identity);
    return true;
}

// The same ranges as RemoteCommandRouter::handleLayerBrightness/Contrast/Saturation/Gamma
bool test_OutputColor_ClampsMatchLayerHandlers() {
    OutputColor c;
    TEST_ASSERT_FALSE(c.set(ColorTrim::Brightness, 1.5f));
    TEST_ASSERT(c.brightness == 1.0f);
    TEST_ASSERT_FALSE(c.set(ColorTrim::Brightness, -2.0f));
    TEST_ASSERT(c.brightness == -1.0f);
    TEST_ASSERT_FALSE(c.set(ColorTrim::Contrast, 2.5f));
    TEST_ASSERT(c.contrast == 2.0f);
    TEST_ASSERT_FALSE(c.set(ColorTrim::Saturation, -0.1f));
    TEST_ASSERT(c.saturation == 0.0f);
    TEST_ASSERT_FALSE(c.set(ColorTrim::Gamma, 0.05f));
    TEST_ASSERT(c.gamma == 0.1f);
    TEST_ASSERT_FALSE(c.set(ColorTrim::Gamma, 3.5f));
    TEST_ASSERT(c.gamma == 3.0f);
    TEST_ASSERT(c.set(ColorTrim::Contrast, 1.08f));
    TEST_ASSERT(c.contrast == 1.08f);
    TEST_ASSERT(c.trimsActive());
    c.resetTrims();
    TEST_ASSERT_FALSE(c.trimsActive());
    TEST_ASSERT(findColorTrim("gamma") && findColorTrim("gamma")->trim == ColorTrim::Gamma);
    TEST_ASSERT(findColorTrim("lut") == nullptr);
    return true;
}

// The layer loop `while (hue > 180) hue -= 360` never ends for huge values; the fold is
// O(1) and agrees with the loop wherever the loop ends (180 stays 180, -180 stays -180)
bool test_OutputColor_HueFoldNeverLoops() {
    TEST_ASSERT(foldHue(540.0f) == 180.0f);
    TEST_ASSERT(foldHue(900.0f) == 180.0f);
    TEST_ASSERT(foldHue(-540.0f) == -180.0f);
    TEST_ASSERT(foldHue(-200.0f) == 160.0f);
    TEST_ASSERT(foldHue(180.0f) == 180.0f);
    TEST_ASSERT(foldHue(-180.0f) == -180.0f);
    TEST_ASSERT(foldHue(370.0f) == 10.0f);
    const float big = foldHue(1e30f);
    TEST_ASSERT(std::isfinite(big) && big >= -180.0f && big <= 180.0f);
    const float bigNeg = foldHue(-1e30f);
    TEST_ASSERT(std::isfinite(bigNeg) && bigNeg >= -180.0f && bigNeg <= 180.0f);
    // Non-finite never reaches the fold: the parser refuses it
    float v = 0.0f;
    TEST_ASSERT_FALSE(parseFiniteFloat("inf", v));
    TEST_ASSERT_FALSE(parseFiniteFloat("NaN", v));
    return true;
}

bool test_OutputColor_FromCharsWholeToken() {
    float v = 0.0f;
    TEST_ASSERT(parseFiniteFloat("1.08", v) && v == 1.08f);
    TEST_ASSERT(parseFiniteFloat(" 1.08", v) && v == 1.08f);
    TEST_ASSERT(parseFiniteFloat("1.08 \r", v) && v == 1.08f);
    TEST_ASSERT(parseFiniteFloat("+3", v) && v == 3.0f);
    TEST_ASSERT(parseFiniteFloat("-3", v) && v == -3.0f);
    TEST_ASSERT(parseFiniteFloat("5e-1", v) && v == 0.5f);
    TEST_ASSERT_FALSE(parseFiniteFloat("1.2x", v));
    TEST_ASSERT_FALSE(parseFiniteFloat("1,2", v));
    TEST_ASSERT_FALSE(parseFiniteFloat("1 2", v));
    TEST_ASSERT_FALSE(parseFiniteFloat("++3", v));
    TEST_ASSERT_FALSE(parseFiniteFloat("+-3", v));
    TEST_ASSERT_FALSE(parseFiniteFloat("", v));
    TEST_ASSERT_FALSE(parseFiniteFloat("  ", v));
    TEST_ASSERT_FALSE(parseFiniteFloat("inf", v));
    TEST_ASSERT_FALSE(parseFiniteFloat("-infinity", v));
    TEST_ASSERT_FALSE(parseFiniteFloat("nan", v));
    TEST_ASSERT_FALSE(parseFiniteFloat("1e999", v));
    // Locale-independent: a comma-decimal locale changes nothing
    const char* old = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = old ? old : "C";
    if (std::setlocale(LC_NUMERIC, "es_ES.UTF-8") || std::setlocale(LC_NUMERIC, "de_DE.UTF-8")) {
        const bool dot = parseFiniteFloat("1.5", v) && v == 1.5f;
        const bool comma = parseFiniteFloat("1,5", v);
        std::setlocale(LC_NUMERIC, saved.c_str());
        TEST_ASSERT(dot);
        TEST_ASSERT_FALSE(comma);
    }
    return true;
}

// The CPU reference follows the layer shader's maths (display/VideoShaders.h)
bool test_OutputColor_ReferenceMatchesLayerSemantics() {
    float r, g, b;
    {
        OutputColor c; c.contrast = 1.2f;
        r = g = b = 0.25f; c.apply(r, g, b);
        TEST_ASSERT(near(r, 0.2f));
    }
    {
        OutputColor c; c.gamma = 2.0f;  // pow(x, 1/g): g > 1 brightens
        r = g = b = 0.25f; c.apply(r, g, b);
        TEST_ASSERT(near(r, 0.5f));
    }
    {
        OutputColor c; c.saturation = 0.0f;  // grayscale = Rec.601 luma
        r = 1.0f; g = 0.0f; b = 0.0f; c.apply(r, g, b);
        TEST_ASSERT(near(r, 0.299f) && near(g, 0.299f) && near(b, 0.299f));
    }
    {
        OutputColor c; c.brightness = 0.1f;
        r = g = b = 0.5f; c.apply(r, g, b);
        TEST_ASSERT(near(r, 0.6f));
    }
    {
        // hue 180 on pure red: (0.213 - 0.787, 0.213 + 0.213, 0.213 + 0.213) -> clamped
        OutputColor c; c.hue = 180.0f;
        r = 1.0f; g = 0.0f; b = 0.0f; c.apply(r, g, b);
        TEST_ASSERT(near(r, 0.0f) && near(g, 0.426f, 1e-5f) && near(b, 0.426f, 1e-5f));
    }
    {
        // hue 120 on pure red moves it to green (the W3C hueRotate matrix the shader uses)
        OutputColor c; c.hue = 120.0f;
        r = 1.0f; g = 0.0f; b = 0.0f; c.apply(r, g, b);
        const float cs = std::cos(120.0f * 3.14159265359f / 180.0f);
        const float sn = std::sin(120.0f * 3.14159265359f / 180.0f);
        TEST_ASSERT(near(r, 0.0f));
        TEST_ASSERT(near(g, 0.213f - cs * 0.213f + sn * 0.143f, 1e-5f));
        TEST_ASSERT(near(b, 0.0f));
        TEST_ASSERT(g > 0.4f);
    }
    {
        // hue 90 on a gray stays gray: rotation preserves the neutral axis
        OutputColor c; c.hue = 90.0f;
        r = g = b = 0.5f; c.apply(r, g, b);
        TEST_ASSERT(near(r, 0.5f, 1e-3f) && near(g, 0.5f, 1e-3f) && near(b, 0.5f, 1e-3f));
    }
    return true;
}

// Texel centres: the domain's ends land on the first and last texel centres
bool test_OutputColor_LutCoordHalfTexel() {
    for (const int n : {33, 65}) {
        const float dmin[3] = {0.0f, 0.0f, 0.0f};
        const float dmax[3] = {1.0f, 1.0f, 1.0f};
        const LutTransform t = LutTransform::make(n, dmin, dmax);
        TEST_ASSERT(near(t.coord(0, 0.0f), 0.5f / n));
        TEST_ASSERT(near(t.coord(1, 1.0f), 1.0f - 0.5f / n));
        TEST_ASSERT(near(t.coord(2, 0.5f), 0.5f));

        const float wmin[3] = {-0.1f, -0.1f, -0.1f};
        const float wmax[3] = {1.1f, 1.1f, 1.1f};
        const LutTransform w = LutTransform::make(n, wmin, wmax);
        TEST_ASSERT(near(w.coord(0, -0.1f), 0.5f / n));
        TEST_ASSERT(near(w.coord(2, 1.1f), 1.0f - 0.5f / n, 1e-5f));
        TEST_ASSERT(near(w.coord(1, 0.5f), 0.5f));
    }
    return true;
}
