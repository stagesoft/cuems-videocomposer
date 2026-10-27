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

#include "CubeLut.h"
#include "OutputColor.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace videocomposer {

namespace {

constexpr const char* kWhitespace = " \t";

std::string lineError(std::size_t lineNo, const std::string& what) {
    return "line " + std::to_string(lineNo) + ": " + what;
}

// Split a line on runs of spaces/tabs.
std::vector<std::string_view> tokenize(std::string_view line) {
    std::vector<std::string_view> tokens;
    std::size_t pos = 0;
    while (true) {
        pos = line.find_first_not_of(kWhitespace, pos);
        if (pos == std::string_view::npos) break;
        std::size_t end = line.find_first_of(kWhitespace, pos);
        if (end == std::string_view::npos) end = line.size();
        tokens.push_back(line.substr(pos, end - pos));
        pos = end;
    }
    return tokens;
}

// Keywords are upper-case identifiers (TITLE, LUT_3D_SIZE, ...); anything else is a data line,
// so a stray "nan" is reported as a bad value, not as an unknown keyword.
bool isKeyword(std::string_view token) {
    if (token.empty() || !(token.front() >= 'A' && token.front() <= 'Z')) return false;
    for (const char c : token) {
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

bool parseTriplet(const std::vector<std::string_view>& tokens, std::size_t first, float out[3]) {
    for (int c = 0; c < 3; ++c) {
        if (!parseFiniteFloat(tokens[first + c], out[c])) return false;
    }
    return true;
}

}  // namespace

bool parseCubeLut(std::string_view text, CubeLut& out, std::string& error) {
    out = CubeLut{};

    // Optional UTF-8 BOM before the first keyword
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }

    bool haveDomainMin = false, haveDomainMax = false, haveInputRange = false;
    float domainMin[3] = {0.0f, 0.0f, 0.0f};
    float domainMax[3] = {1.0f, 1.0f, 1.0f};
    float rangeLo = 0.0f, rangeHi = 1.0f;
    std::size_t expected = 0;
    std::size_t dataLines = 0;

    std::size_t lineNo = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;

        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const std::size_t first = line.find_first_not_of(kWhitespace);
        if (first == std::string_view::npos) continue;  // blank
        line.remove_prefix(first);
        if (line.front() == '#') continue;  // comment line

        const std::vector<std::string_view> tokens = tokenize(line);
        const std::string_view key = tokens.front();

        if (!isKeyword(key)) {
            // A data line
            if (out.size == 0) {
                error = lineError(lineNo, "data before LUT_3D_SIZE");
                return false;
            }
            if (dataLines == expected) {
                error = lineError(lineNo, "more than " + std::to_string(expected) +
                                          " data lines (LUT_3D_SIZE " + std::to_string(out.size) + ")");
                return false;
            }
            if (tokens.size() != 3) {
                error = lineError(lineNo, "data line has " + std::to_string(tokens.size()) +
                                          " values, expected 3");
                return false;
            }
            float v[3];
            if (!parseTriplet(tokens, 0, v)) {
                error = lineError(lineNo, "non-numeric or non-finite value");
                return false;
            }
            for (int c = 0; c < 3; ++c) {
                if (v[c] < 0.0f || v[c] > 1.0f) {
                    v[c] = v[c] < 0.0f ? 0.0f : 1.0f;
                    ++out.clampedValues;
                }
                out.rgb.push_back(v[c]);
            }
            ++dataLines;
        } else if (dataLines > 0) {
            error = lineError(lineNo, "keyword '" + std::string(key) + "' after the data");
            return false;
        } else if (key == "TITLE") {
            const std::size_t q = line.find_first_not_of(kWhitespace, key.size());
            out.title = q == std::string_view::npos ? std::string() : std::string(line.substr(q));
        } else if (key == "LUT_3D_SIZE") {
            if (out.size != 0) {
                error = lineError(lineNo, "LUT_3D_SIZE given twice");
                return false;
            }
            float n = 0.0f;
            if (tokens.size() != 2 || !parseFiniteFloat(tokens[1], n) || n != static_cast<float>(static_cast<int>(n))) {
                error = lineError(lineNo, "LUT_3D_SIZE needs one integer");
                return false;
            }
            const int size = static_cast<int>(n);
            if (size < CubeLut::kMinSize || size > CubeLut::kMaxSize) {
                error = lineError(lineNo, "LUT_3D_SIZE " + std::to_string(size) + " out of range (" +
                                          std::to_string(CubeLut::kMinSize) + ".." +
                                          std::to_string(CubeLut::kMaxSize) + ")");
                return false;
            }
            out.size = size;
            expected = out.entryCount();
            out.rgb.reserve(expected * 3);
        } else if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX") {
            float v[3];
            if (tokens.size() != 4 || !parseTriplet(tokens, 1, v)) {
                error = lineError(lineNo, std::string(key) + " needs three finite numbers");
                return false;
            }
            float* dst = key == "DOMAIN_MIN" ? domainMin : domainMax;
            for (int c = 0; c < 3; ++c) dst[c] = v[c];
            (key == "DOMAIN_MIN" ? haveDomainMin : haveDomainMax) = true;
        } else if (key == "LUT_3D_INPUT_RANGE") {
            if (tokens.size() != 3 || !parseFiniteFloat(tokens[1], rangeLo) || !parseFiniteFloat(tokens[2], rangeHi)) {
                error = lineError(lineNo, "LUT_3D_INPUT_RANGE needs two finite numbers");
                return false;
            }
            haveInputRange = true;
        } else if (key == "LUT_1D_SIZE") {
            error = lineError(lineNo, "1D LUTs are not supported (LUT_1D_SIZE)");
            return false;
        } else {
            error = lineError(lineNo, "unknown keyword '" + std::string(key) + "'");
            return false;
        }
    }

    if (out.size == 0) {
        error = "no LUT_3D_SIZE";
        return false;
    }
    if (dataLines != expected) {
        error = "file ends after " + std::to_string(dataLines) + " data lines, expected " +
                std::to_string(expected) + " (LUT_3D_SIZE " + std::to_string(out.size) + ")";
        return false;
    }

    // LUT_3D_INPUT_RANGE is a uniform domain; with DOMAIN_* given too, they must agree
    if (haveInputRange) {
        for (int c = 0; c < 3; ++c) {
            if ((haveDomainMin && domainMin[c] != rangeLo) || (haveDomainMax && domainMax[c] != rangeHi)) {
                error = "LUT_3D_INPUT_RANGE disagrees with DOMAIN_MIN/DOMAIN_MAX (ambiguous domain)";
                return false;
            }
            domainMin[c] = rangeLo;
            domainMax[c] = rangeHi;
        }
    }
    for (int c = 0; c < 3; ++c) {
        if (!(domainMax[c] > domainMin[c])) {
            error = "DOMAIN_MAX must be above DOMAIN_MIN on every channel";
            return false;
        }
        out.domainMin[c] = domainMin[c];
        out.domainMax[c] = domainMax[c];
    }
    return true;
}

CubeLoadResult loadCubeLutFile(const std::string& path, CubeLut& out) {
    CubeLoadResult result;
    const auto t0 = std::chrono::steady_clock::now();

    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) {
        const int err = errno;
        result.notFound = (err == ENOENT || err == ENOTDIR);
        result.error = std::string("cannot open: ") + std::strerror(err);
        return result;
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        result.error = std::string("cannot stat: ") + std::strerror(errno);
        ::close(fd);
        return result;
    }
    if (!S_ISREG(st.st_mode)) {
        result.error = "not a regular file";
        ::close(fd);
        return result;
    }
    if (static_cast<unsigned long long>(st.st_size) > CubeLut::kMaxFileBytes) {
        result.error = "larger than " + std::to_string(CubeLut::kMaxFileBytes / (1024 * 1024)) + " MiB";
        ::close(fd);
        return result;
    }

    // Read to EOF, never past the bound (the file may grow between fstat and read)
    std::string text;
    text.resize(static_cast<std::size_t>(st.st_size) + 1);
    std::size_t have = 0;
    while (true) {
        if (have == text.size()) {
            if (text.size() > CubeLut::kMaxFileBytes) {
                result.error = "larger than " + std::to_string(CubeLut::kMaxFileBytes / (1024 * 1024)) + " MiB";
                ::close(fd);
                return result;
            }
            text.resize(std::min(text.size() * 2 + 4096, CubeLut::kMaxFileBytes + 1));
        }
        const ssize_t n = ::read(fd, &text[have], text.size() - have);
        if (n < 0) {
            if (errno == EINTR) continue;
            result.error = std::string("read failed: ") + std::strerror(errno);
            ::close(fd);
            return result;
        }
        if (n == 0) break;
        have += static_cast<std::size_t>(n);
    }
    ::close(fd);
    if (have > CubeLut::kMaxFileBytes) {
        result.error = "larger than " + std::to_string(CubeLut::kMaxFileBytes / (1024 * 1024)) + " MiB";
        return result;
    }
    text.resize(have);

    std::string error;
    if (!parseCubeLut(text, out, error)) {
        result.error = error;
        return result;
    }
    result.ok = true;
    result.parseMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return result;
}

namespace {

bool hasControlChar(std::string_view s) {
    for (const char c : s) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) return true;
    }
    return false;
}

std::string joinUnder(std::string_view baseDir, std::string_view rel) {
    std::string out(baseDir);
    if (!out.empty() && out.back() != '/') out += '/';
    out += rel;
    return out;
}

}  // namespace

bool resolveConfigLutPath(std::string_view value, std::string& resolved, std::string& error,
                          std::string_view baseDir) {
    if (value.empty()) {
        error = "empty LUT path";
        return false;
    }
    if (hasControlChar(value)) {
        error = "control character in LUT path";
        return false;
    }
    if (value.front() == '/') {
        resolved = std::string(value);
        return true;
    }
    // Relative: resolved under the LUT directory, no ".." component
    std::size_t pos = 0;
    while (pos <= value.size()) {
        std::size_t slash = value.find('/', pos);
        if (slash == std::string_view::npos) slash = value.size();
        if (value.substr(pos, slash - pos) == "..") {
            error = "'..' is not allowed in a relative LUT path";
            return false;
        }
        pos = slash + 1;
    }
    resolved = joinUnder(baseDir, value);
    return true;
}

bool resolveOscLutName(std::string_view name, std::string& resolved, std::string& error,
                       std::string_view baseDir) {
    static constexpr std::string_view kSuffix = ".cube";
    if (name.find('/') != std::string_view::npos || name.find("..") != std::string_view::npos ||
        hasControlChar(name) || name.size() <= kSuffix.size() ||
        name.substr(name.size() - kSuffix.size()) != kSuffix) {
        error = "LUT name must be a bare file name ending in .cube (no '/', no '..'), under " +
                std::string(baseDir);
        return false;
    }
    resolved = joinUnder(baseDir, name);
    return true;
}

} // namespace videocomposer
