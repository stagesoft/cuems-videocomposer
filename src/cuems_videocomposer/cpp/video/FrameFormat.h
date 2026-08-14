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

#ifndef VIDEOCOMPOSER_FRAMEFORMAT_H
#define VIDEOCOMPOSER_FRAMEFORMAT_H

#include <cstdint>

namespace videocomposer {

enum class PixelFormat {
    YUV420P,
    RGB24,
    RGBA32,
    BGRA32,
    UYVY422
};

struct FrameInfo {
    int width = 0;
    int height = 0;
    float aspect = 0.0f;
    double framerate = 0.0;
    int64_t totalFrames = 0;
    double duration = 0.0;
    int64_t fileFrameOffset = 0;
    PixelFormat format = PixelFormat::YUV420P;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_FRAMEFORMAT_H

