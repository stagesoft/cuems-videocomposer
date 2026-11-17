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
 * CanvasSize.h - the size of the virtual canvas, and which connected outputs get a surface (ClickUp 1amfwm7hrv0,
 * cuems-RELATIONS Plans/2026-11-16-vc-declared-canvas-1amfwm7hrv0.md).
 *
 * The engine (cuems-engine tools/display_conf.py) places and sizes every layer relative to ITS canvas: the bounding
 * box of every enabled [output:*] section of display.conf with a valid canvas_region, connected or not. These pure
 * functions are the one place where the videocomposer decides its own canvas, so the two can be made to agree, and
 * they have no GL, no DRM and no file access, so every case has a unit row (test/TestCanvasSize.cpp).
 *
 * "The plain form" is how cuems-generate-display-conf and cuems-display-setup write a section, and the only form in
 * which the engine's configparser reader and this videocomposer's line reader are shown to read a section alike:
 * the header exactly "[output:NAME]" in the first column; every other line blank, a '#' comment in the first column,
 * or key=value in the first column with a key of letters, digits and '_' and no space around the '='; no key twice;
 * the header once in the file; a canvas_region of exactly four integers separated by commas (a space may follow a
 * comma); no carriage return. Anything else is read differently by the two in some case, so it is not counted
 * unless it is driven (today's behaviour), and the journal names it.
 */

#ifndef VIDEOCOMPOSER_CANVASSIZE_H
#define VIDEOCOMPOSER_CANVASSIZE_H

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace videocomposer {
namespace canvas {

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct Size {
    int width = 0;
    int height = 0;
    bool valid() const { return width > 0 && height > 0; }
    bool operator==(const Size& o) const { return width == o.width && height == o.height; }
    bool operator!=(const Size& o) const { return !(*this == o); }
};

inline std::string toString(const Size& s) { return std::to_string(s.width) + "x" + std::to_string(s.height); }

inline std::string toString(const Rect& r) {
    return std::to_string(r.x) + "," + std::to_string(r.y) + "," + std::to_string(r.width) + "," +
           std::to_string(r.height);
}

/** Right edge of the right-most rectangle by the bottom edge of the lowest one, from the canvas origin (0,0). */
inline Size boundingBox(const std::vector<Rect>& rects) {
    Size s;
    for (const auto& r : rects) {
        s.width = std::max(s.width, r.x + r.width);
        s.height = std::max(s.height, r.y + r.height);
    }
    return s;
}

/** An output that has a surface, with the canvas region it shows. */
struct Driven {
    std::string name;
    Rect region;
};

inline std::vector<Rect> regionsOf(const std::vector<Driven>& driven) {
    std::vector<Rect> rects;
    for (const auto& d : driven) {
        rects.push_back(d.region);
    }
    return rects;
}

// ===== What display.conf declares =====

struct DeclaredSection {
    std::string name;
    bool plain = false;
    std::string notPlainBecause;  // empty when plain
    bool enabled = true;          // as both readers read it: the last "enabled=" line, only "true" or "1" keep it on
    bool hasRegion = false;       // a canvas_region of four integers in the plain form
    Rect region;                  // as written (a width or height <= 0 is kept as written, as the engine keeps it)
};

struct DeclaredFile {
    std::vector<DeclaredSection> sections;  // file order, one per connector name
    bool canvasSizeLine = false;            // a canvas_size line, however it is spaced
    bool regionLines = false;               // any canvas_region line, however it is spaced

    const DeclaredSection* find(const std::string& name) const {
        for (const auto& s : sections) {
            if (s.name == name) {
                return &s;
            }
        }
        return nullptr;
    }
};

namespace detail {

inline bool isBlank(const std::string& line) {
    return line.find_first_not_of(" \t") == std::string::npos;
}

/** The line, after leading spaces and tabs, is `key` followed by optional spaces/tabs and '=' or ':'. */
inline bool isKeyLineLoose(const std::string& line, const std::string& key) {
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos || line.compare(i, key.size(), key) != 0) {
        return false;
    }
    i = line.find_first_not_of(" \t", i + key.size());
    return i != std::string::npos && (line[i] == '=' || line[i] == ':');
}

inline bool isPlainKey(const std::string& key) {
    if (key.empty()) {
        return false;
    }
    for (char c : key) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) {
            return false;
        }
    }
    return true;
}

/** Exactly four integers (optional '-', 1 to 7 digits) separated by ',' or ", ", nothing else. */
inline bool parsePlainRegion(const std::string& value, Rect& out) {
    int nums[4] = {0, 0, 0, 0};
    size_t i = 0;
    for (int n = 0; n < 4; ++n) {
        if (n > 0) {
            if (i >= value.size() || value[i] != ',') {
                return false;
            }
            ++i;
            if (i < value.size() && value[i] == ' ') {
                ++i;
            }
        }
        bool negative = false;
        if (i < value.size() && value[i] == '-') {
            negative = true;
            ++i;
        }
        size_t start = i;
        long v = 0;
        while (i < value.size() && std::isdigit(static_cast<unsigned char>(value[i]))) {
            v = v * 10 + (value[i] - '0');
            ++i;
            if (i - start > 7) {
                return false;
            }
        }
        if (i == start) {
            return false;
        }
        nums[n] = static_cast<int>(negative ? -v : v);
    }
    if (i != value.size()) {
        return false;
    }
    out = Rect{nums[0], nums[1], nums[2], nums[3]};
    return true;
}

}  // namespace detail

/**
 * Classify each [output:*] section of a display.conf text. Sections are found the way this videocomposer finds them
 * (DisplayConfigurationManager::loadFromFile): a line starting with "[output:" and holding a ']' opens one; any other
 * '[' line does not close it for this videocomposer, while it does for the engine.
 */
inline DeclaredFile plainSections(const std::string& text) {
    DeclaredFile file;
    std::map<std::string, size_t> index;
    std::map<std::string, int> headerCount;
    std::map<std::string, std::set<std::string>> keysSeen;

    auto notPlain = [](DeclaredSection& s, const std::string& why) {
        if (s.notPlainBecause.empty()) {
            s.notPlainBecause = why;
        }
    };

    std::vector<std::string> lines;
    {
        size_t start = 0;
        while (start <= text.size()) {
            size_t nl = text.find('\n', start);
            if (nl == std::string::npos) {
                if (start < text.size()) {
                    lines.push_back(text.substr(start));
                }
                break;
            }
            lines.push_back(text.substr(start, nl - start));
            start = nl + 1;
        }
    }

    long current = -1;     // index into file.sections of this videocomposer's current section
    bool foreign = false;  // past another '[' header: the engine has left the section, this videocomposer has not
    for (const auto& line : lines) {
        if (detail::isKeyLineLoose(line, "canvas_size")) {
            file.canvasSizeLine = true;
        }
        if (detail::isKeyLineLoose(line, "canvas_region")) {
            file.regionLines = true;
        }

        if (!line.empty() && line[0] == '[') {
            const size_t close = line.find(']');
            if (line.rfind("[output:", 0) == 0 && close != std::string::npos) {
                const std::string name = line.substr(8, close - 8);
                auto it = index.find(name);
                if (it == index.end()) {
                    DeclaredSection s;
                    s.name = name;
                    file.sections.push_back(s);
                    it = index.emplace(name, file.sections.size() - 1).first;
                }
                current = static_cast<long>(it->second);
                foreign = false;
                ++headerCount[name];
                DeclaredSection& s = file.sections[it->second];
                if (close + 1 != line.size()) {
                    notPlain(s, "the header line has text after the ']'");
                }
                if (name.empty() || name.find_first_of(" \t\r") != std::string::npos) {
                    notPlain(s, "the connector name is empty or holds a space");
                }
            } else {
                foreign = true;
            }
            continue;
        }
        if (current < 0) {
            continue;  // global keys
        }
        DeclaredSection& s = file.sections[static_cast<size_t>(current)];

        if (foreign) {
            // Keys this videocomposer still reads into the section; the engine reads them into another one.
            if (line.empty() || line[0] == '#') {
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string key = line.substr(0, eq);
            if (key == "canvas_region") {
                notPlain(s, "a canvas_region line under a later '[' header, which this videocomposer reads into this "
                            "section and the engine does not");
            } else if (key == "enabled") {
                // Both readers apply it to this section (the engine's _disabled_outputs reads enabled= this way).
                const std::string value = line.substr(eq + 1);
                s.enabled = (value == "true" || value == "1");
            }
            continue;
        }

        if (line.find('\r') != std::string::npos) {
            notPlain(s, "a line ends in a carriage return");
        }
        if (detail::isBlank(line)) {
            continue;
        }
        if (line[0] == '#') {
            continue;
        }
        if (line[0] == ' ' || line[0] == '\t') {
            notPlain(s, "an indented line, which the engine joins to the line before");
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            notPlain(s, "a line that is not key=value (a ':' for '=', or a ';' comment)");
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (!detail::isPlainKey(key)) {
            notPlain(s, "the key '" + key + "' holds a space or another character the engine reads differently");
            continue;
        }
        if (!keysSeen[s.name].insert(key).second) {
            notPlain(s, "the key " + key + " appears twice");
        }
        if (key == "enabled") {
            s.enabled = (value == "true" || value == "1");
        } else if (key == "canvas_region") {
            Rect r;
            if (detail::parsePlainRegion(value, r)) {
                s.hasRegion = true;
                s.region = r;
            } else {
                notPlain(s, "canvas_region is not four integers separated by commas");
            }
        }
    }

    for (auto& s : file.sections) {
        const int count = headerCount[s.name];
        if (count > 1) {
            notPlain(s, "the section appears " + std::to_string(count) + " times");
        }
        if (!s.hasRegion) {
            notPlain(s, "no canvas_region");
        }
        s.plain = s.notPlainBecause.empty();
        if (!s.plain) {
            s.hasRegion = false;
            s.region = Rect{};
        }
    }
    return file;
}

// ===== The canvas =====

/**
 * The canvas this videocomposer wants, before any allocation. `declared` is null when it does not use the file's
 * layout (canvas_layout is not custom, or there is no file).
 */
inline Size wantedCanvas(const DeclaredFile* declared, const std::vector<Driven>& driven) {
    std::vector<Rect> rects = regionsOf(driven);  // the floor: never smaller than what is driven
    if (declared) {
        for (const auto& s : declared->sections) {
            if (s.plain && s.enabled && s.hasRegion) {
                rects.push_back(s.region);  // counted as written, driven or not, as the engine counts it
            }
        }
    }
    return boundingBox(rects);
}

enum class CanvasSource {
    Driven,               // the bounding box of the driven regions (today's canvas; also when it equals the declared one)
    Declared,             // the declared canvas, larger than the driven regions
    DrivenBeyondLimit,    // declared canvas beyond the GPU's limit, not attempted
    DrivenAfterFailure,   // declared canvas attempted and failed
    None                  // no canvas could be had
};

struct CanvasChoice {
    Size size;                  // the canvas configured; 0x0 when source is None
    CanvasSource source = CanvasSource::None;
    Size driven;                // bounding box of the driven regions
    Size wanted;                // wantedCanvas()
    std::vector<Size> attempts; // every size passed to the allocation, in order
    std::string reason;         // why the declared canvas was not used; empty otherwise
};

/**
 * Decide the canvas and allocate it. `sizeLimit` is the GPU's largest side (0 = unknown); `tryAllocate` makes the
 * canvas of that size and says whether it exists (VirtualCanvas::configure in the renderer, a stub in the tests).
 */
inline CanvasChoice canvasToConfigure(const DeclaredFile* declared, const std::vector<Driven>& driven, int sizeLimit,
                                      const std::function<bool(int, int)>& tryAllocate) {
    CanvasChoice c;
    c.driven = boundingBox(regionsOf(driven));
    c.wanted = wantedCanvas(declared, driven);
    c.source = CanvasSource::Driven;

    if (c.wanted.valid() && c.wanted != c.driven) {
        if (sizeLimit > 0 && (c.wanted.width > sizeLimit || c.wanted.height > sizeLimit)) {
            c.source = CanvasSource::DrivenBeyondLimit;
            c.reason = "a side is beyond this GPU's limit of " + std::to_string(sizeLimit) + " px, not attempted";
        } else {
            c.attempts.push_back(c.wanted);
            if (tryAllocate(c.wanted.width, c.wanted.height)) {
                c.source = CanvasSource::Declared;
                c.size = c.wanted;
                return c;
            }
            c.source = CanvasSource::DrivenAfterFailure;
            c.reason = "the GPU refused a canvas of that size";
        }
    }

    // Today's canvas: the driven regions
    if (!c.driven.valid()) {
        c.source = CanvasSource::None;
        return c;
    }
    c.attempts.push_back(c.driven);
    if (tryAllocate(c.driven.width, c.driven.height)) {
        c.size = c.driven;
    } else {
        c.source = CanvasSource::None;
    }
    return c;
}

// ===== Which connected outputs get a surface (D4) =====

struct ConnectedOutput {
    std::string name;
    bool listed = false;   // display.conf has an [output:NAME] section, of any form
    bool enabled = true;   // its enabled= as read (meaningful when listed)
};

struct SurfacePlan {
    std::vector<std::string> create;     // connected outputs that get a surface, in the order given
    std::vector<std::string> disabled;   // enabled=false: no surface (unchanged)
    std::vector<std::string> unlisted;   // connected, not listed, skipped (D4, W6)
    bool exception = false;              // no listed, enabled output connected: unlisted ones driven as before (ERROR)
};

/** `customLayout`: this videocomposer uses the file's layout (canvas_layout=custom as it reads it). */
inline SurfacePlan surfacesToCreate(const std::vector<ConnectedOutput>& connected, bool customLayout) {
    SurfacePlan plan;
    bool anyListedEnabled = false;
    for (const auto& c : connected) {
        if (c.listed && c.enabled) {
            anyListedEnabled = true;
        }
    }
    for (const auto& c : connected) {
        if (c.listed && !c.enabled) {
            plan.disabled.push_back(c.name);
        } else if (!c.listed && customLayout && anyListedEnabled) {
            plan.unlisted.push_back(c.name);  // D4: dark, like enabled=false
        } else {
            if (!c.listed && customLayout) {
                plan.exception = true;  // no listed output connected: driven as before, never a black headless box
            }
            plan.create.push_back(c.name);
        }
    }
    return plan;
}

// ===== What the journal says (W1 to W6) =====

struct LayoutNote {
    enum class Level { Warning, Error };
    Level level = Level::Warning;
    std::string code;  // "W1" .. "W6", "D4-exception"
    std::string text;
};

/** The lines logged once at start, where the engine's canvas and this one can differ. */
inline std::vector<LayoutNote> layoutNotes(const DeclaredFile& file, bool customLayout,
                                           const std::vector<Driven>& driven, const SurfacePlan& surfaces) {
    std::vector<LayoutNote> notes;
    auto warn = [&notes](const char* code, const std::string& text) {
        notes.push_back(LayoutNote{LayoutNote::Level::Warning, code, text});
    };
    auto isDriven = [&driven](const std::string& name) {
        for (const auto& d : driven) {
            if (d.name == name) {
                return true;
            }
        }
        return false;
    };

    if (customLayout) {
        const Size canvas = wantedCanvas(&file, driven);
        for (const auto& s : file.sections) {
            if (!s.enabled) {
                continue;  // dark by design, counted by neither reader
            }
            const bool drivenHere = isDriven(s.name);
            if (s.plain && !drivenHere) {
                warn("W1", "[output:" + s.name + "] canvas_region=" + toString(s.region) + " is listed but no output " +
                               "is driven on " + s.name + " (not connected, or no surface): it stays in the canvas, " +
                               toString(canvas) + " as the engine's, and its region stays dark");
            } else if (!s.plain) {
                warn("W2", "[output:" + s.name + "] is not written in the plain form (" + s.notPlainBecause +
                               "): the engine may read it differently. It is " +
                               (drivenHere ? "counted in the canvas because it is driven"
                                           : "left out of the canvas because it is not driven"));
            } else if (s.region.width <= 0 || s.region.height <= 0) {
                warn("W3", "[output:" + s.name + "] canvas_region=" + toString(s.region) + " has a width or height " +
                               "of zero or less: the engine takes it as written, this videocomposer uses the output's " +
                               "own size, so the two canvases can differ");
            }
        }
        for (const auto& name : surfaces.unlisted) {
            warn("W6", name + " is connected but display.conf has no [output:" + name + "] section: no surface " +
                           "created, it stays dark (add a section for it to use it)");
        }
        if (surfaces.exception) {
            std::string names;
            for (const auto& name : surfaces.create) {
                names += (names.empty() ? "" : ", ") + name;
            }
            notes.push_back(LayoutNote{LayoutNote::Level::Error, "D4-exception",
                                       "no output listed in display.conf is connected; driving the connected " +
                                           names + " as before, which the file does not list. The file and the " +
                                           "connected outputs disagree: check the cabling or display.conf"});
        }
    } else if (file.regionLines) {
        warn("W5", "display.conf has canvas_region lines but this videocomposer does not use the file's layout (no " +
                       std::string("line written exactly canvas_layout=custom): the engine places cues by those ") +
                       "regions, this videocomposer lays the outputs out by itself");
    }
    if (file.canvasSizeLine) {
        warn("W4", "display.conf has a canvas_size line: the engine uses it, this videocomposer does not (ticket "
                   "1amfwm7j9xv), so the two canvases differ");
    }
    return notes;
}

}  // namespace canvas
}  // namespace videocomposer

#endif  // VIDEOCOMPOSER_CANVASSIZE_H
