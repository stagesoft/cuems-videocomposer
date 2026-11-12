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

// 1amfwm7hrb9: the software readFrame() freed its AVPacket only on a read error
// (since 248b8d8, v0.1.2-9), so every decoded frame leaked the last compressed
// packet: ~8.3 MB/s on a 1080p30 ProRes layer, the VC degraded after ~7 min.
// Measured here as malloc's in-use bytes (mallinfo2: main arena + mmap'd chunks) across 100
// sequential software reads of a ProRes clip with large packets: a leak shows
// as about 100 packets; a correct read returns to its steady state.

#include "TestFramework.h"
#include "../input/VideoFileInput.h"
#include "../video/FrameBuffer.h"
#include <cstdlib>
#include <iostream>
#include <malloc.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace videocomposer;
using namespace videocomposer::test;

namespace {

bool haveProres() {
    return std::system("ffmpeg -version >/dev/null 2>&1") == 0 &&
           std::system("ffmpeg -hide_banner -encoders 2>/dev/null | grep -q prores_ks") == 0;
}

// 640x360, 25 fps, 6 s (150 frames), ProRes 422 HQ of noise: packets of a few
// hundred kB, so 100 leaked packets are tens of MB, far above allocator noise.
std::string makeNoiseClip(const std::string& dir) {
    const std::string path = dir + "/noise.mov";
    const std::string cmd =
        "ffmpeg -nostdin -hide_banner -loglevel error -y "
        "-f lavfi -i color=c=gray:s=640x360:r=25:d=6 -vf noise=alls=100:allf=t "
        "-c:v prores_ks -profile:v 3 " + path;
    return std::system(cmd.c_str()) == 0 ? path : std::string();
}

// In use in the main arena plus mmap'd chunks, so a packet counts wherever malloc put it.
size_t inUseBytes() {
    const struct mallinfo2 mi = mallinfo2();
    return mi.uordblks + mi.hblkhd;
}

// Removes the temp dir on every exit, including a failed assertion's early return.
struct TempDir {
    std::string path;
    TempDir() {
        char tmpl[] = "/tmp/vc-leak-XXXXXX";
        const char* d = mkdtemp(tmpl);
        if (d) path = d;
    }
    ~TempDir() {
        if (!path.empty()) {
            const std::string rm = "rm -rf " + path;
            (void)std::system(rm.c_str());
        }
    }
};

} // namespace

bool test_SoftwareDecode_ReadFrameDoesNotLeakPackets() {
    if (!haveProres()) {
        std::cout << "(skipped: no ffmpeg/prores_ks) ";
        return true;
    }
    TempDir tmp;
    TEST_ASSERT_FALSE(tmp.path.empty());
    const std::string clip = makeNoiseClip(tmp.path);
    TEST_ASSERT_FALSE(clip.empty());

    struct stat st {};
    TEST_ASSERT_EQ(::stat(clip.c_str(), &st), 0);
    const double meanPacket = static_cast<double>(st.st_size) / 150.0;

    bool ok = true;
    {
        VideoFileInput in;
        in.setHardwareDecodePreference(VideoFileInput::HardwareDecodePreference::SOFTWARE_ONLY);
        TEST_ASSERT_TRUE(in.open(clip));
        FrameBuffer buf;
        // Warm up: decoder frame pool, threads, sws context, the BGRA buffer.
        for (int f = 0; f < 20; ++f) TEST_ASSERT_TRUE(in.readFrame(f, buf));
        malloc_trim(0);
        const size_t before = inUseBytes();
        const int reads = 100;
        for (int f = 20; f < 20 + reads; ++f) TEST_ASSERT_TRUE(in.readFrame(f, buf));
        malloc_trim(0);
        const size_t after = inUseBytes();
        const double grown = static_cast<double>(after) - static_cast<double>(before);
        const double leakIfBroken = reads * meanPacket;
        std::cout << "\n  mean packet " << static_cast<long>(meanPacket) << " B, in-use growth over "
                  << reads << " reads " << static_cast<long>(grown) << " B (a packet leak would be ~"
                  << static_cast<long>(leakIfBroken) << " B)  ";
        // Pass: under a quarter of what one leaked packet per read would hold.
        ok = grown < 0.25 * leakIfBroken;
        in.close();
    }
    TEST_ASSERT_TRUE(ok);
    return true;
}
