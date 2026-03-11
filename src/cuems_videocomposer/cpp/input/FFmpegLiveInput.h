/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (C) 2020-2026 Stage Lab Coop.
 * Author: Ion Reguera <ion@stagelab.coop>
 *
 * This file is part of cuems-videocomposer.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef VIDEOCOMPOSER_FFMPEGLIVEINPUT_H
#define VIDEOCOMPOSER_FFMPEGLIVEINPUT_H

#include "LiveInputSource.h"
#include <cuems_mediadecoder/MediaFileReader.h>
#include <cuems_mediadecoder/VideoDecoder.h>
#include <string>

namespace videocomposer {

/**
 * FFmpegLiveInput - Live input via FFmpeg (V4L2, RTSP, etc.)
 * 
 * Stopgap implementation using FFmpeg for live sources.
 * Later can be replaced with dedicated V4L2Input if needed.
 */
class FFmpegLiveInput : public LiveInputSource {
public:
    FFmpegLiveInput();
    virtual ~FFmpegLiveInput();

    // InputSource interface
    bool open(const std::string& source) override;
    void close() override;
    bool isReady() const override;
    FrameInfo getFrameInfo() const override;
    int64_t getCurrentFrame() const override;
    CodecType detectCodec() const override;
    bool supportsDirectGPUTexture() const override { return false; }
    DecodeBackend getOptimalBackend() const override;

    // Set FFmpeg format (e.g., "v4l2", "rtsp")
    void setFormat(const std::string& format) { format_ = format; }

protected:
    // LiveInputSource interface
    bool captureFrame(FrameBuffer& buffer) override;
    const char* getSourceTypeName() const override { return format_.empty() ? "FFmpegLive" : format_.c_str(); }

private:
    cuems_mediadecoder::MediaFileReader mediaReader_;
    cuems_mediadecoder::VideoDecoder videoDecoder_;
    FrameInfo frameInfo_;
    std::string format_;
    bool ready_;
    std::atomic<int64_t> frameCount_;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_FFMPEGLIVEINPUT_H

