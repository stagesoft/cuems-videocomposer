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
 * dump_blit_glsl - writes the output blit programs' GLSL to a directory, so the
 * build can run glslangValidator on exactly the text the compositor compiles
 * (CMake target validate_blit_glsl). GL-free host tool, never installed.
 *
 *   dump_blit_glsl <dir>   ->  <dir>/blit.vert, <dir>/A.frag, <dir>/B.frag
 *
 * Exits non-zero when B cannot be assembled from A (an anchor not found exactly
 * once), which is the same condition that makes the compositor run without color.
 */

#include "display/OutputBlitGlsl.h"

#include <fstream>
#include <iostream>
#include <string>

namespace {

bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "dump_blit_glsl: cannot open " << path << " for writing\n";
        return false;
    }
    out << text;
    out.close();
    if (!out) {
        std::cerr << "dump_blit_glsl: write to " << path << " failed\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: dump_blit_glsl <output-directory>\n";
        return 2;
    }
    const std::string dir = argv[1];

    using namespace videocomposer;
    const std::string b = OutputBlitGlsl::assembleFragmentB();
    if (b.empty()) {
        std::cerr << "dump_blit_glsl: program B could not be assembled from A "
                     "(an anchor is not in A exactly once)\n";
        return 1;
    }

    if (!writeFile(dir + "/blit.vert", OutputBlitGlsl::kVertex) ||
        !writeFile(dir + "/A.frag", OutputBlitGlsl::fragmentA()) ||
        !writeFile(dir + "/B.frag", b)) {
        return 1;
    }
    return 0;
}
