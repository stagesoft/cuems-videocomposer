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

#ifndef VIDEOCOMPOSER_INPUTSOURCE_H
#define VIDEOCOMPOSER_INPUTSOURCE_H

#include "../video/FrameBuffer.h"
#include "../video/FrameFormat.h"
#include "../video/GPUTextureFrameBuffer.h"
#include <string>
#include <cstdint>

namespace videocomposer {

/**
 * Abstract base class for all input sources.
 * 
 * This interface allows different input types (video files, live video, streaming, etc.)
 * to be used interchangeably. Each layer can have its own input source.
 */
class InputSource {
public:
    virtual ~InputSource() = default;

    /**
     * Open the input source
     * @param source Path or identifier for the source
     * @return true on success, false on failure
     */
    virtual bool open(const std::string& source) = 0;

    /**
     * Close the input source and release resources
     */
    virtual void close() = 0;

    /**
     * Check if input source is ready/opened
     * @return true if ready, false otherwise
     */
    virtual bool isReady() const = 0;

    /**
     * Read a frame at the given frame number
     * @param frameNumber Frame number to read
     * @param buffer FrameBuffer to store the decoded frame
     * @return true on success, false on failure
     */
    virtual bool readFrame(int64_t frameNumber, FrameBuffer& buffer) = 0;

    /**
     * Seek to a specific frame number
     * @param frameNumber Target frame number
     * @return true on success, false on failure
     */
    virtual bool seek(int64_t frameNumber) = 0;
    
    /**
     * Reset internal seek optimization state
     * Call this before seek() to force a full seek even if the frame number
     * is the same as the current position. Used for MTC full frame SYSEX
     * position commands where we must seek regardless of current position.
     */
    virtual void resetSeekState() {}

    /**
     * Get information about the video source
     * @return FrameInfo structure with video properties
     */
    virtual FrameInfo getFrameInfo() const = 0;

    /**
     * Get the current frame number (last frame read)
     * @return Current frame number, or -1 if not available
     */
    virtual int64_t getCurrentFrame() const = 0;

    /**
     * Codec types supported by the system
     */
    enum class CodecType {
        HAP,           // HAP codec (standard)
        HAP_Q,         // HAP Q variant (higher quality)
        HAP_ALPHA,     // HAP Alpha variant (with alpha channel)
        H264,          // H.264/AVC
        HEVC,          // H.265/HEVC
        AV1,           // AV1
        SOFTWARE       // Software codec (fallback)
    };

    /**
     * Decode backend types
     */
    enum class DecodeBackend {
        HAP_DIRECT,    // HAP direct GPU texture (zero-copy)
        GPU_HARDWARE,  // GPU hardware decoder (NVDEC, VAAPI, etc.)
        CPU_SOFTWARE   // CPU software decoder
    };

    /**
     * Detect the codec type of this input source
     * @return CodecType enum value
     */
    virtual CodecType detectCodec() const = 0;

    /**
     * Check if this input source supports direct GPU texture decoding
     * HAP codecs can decode directly to OpenGL textures (zero-copy)
     * @return true if direct GPU texture is supported
     */
    virtual bool supportsDirectGPUTexture() const = 0;

    /**
     * Get the optimal decode backend for this input source
     * @return DecodeBackend enum value indicating best decoding method
     */
    virtual DecodeBackend getOptimalBackend() const = 0;

    /**
     * Check if this is a live stream (no seeking, continuous reading)
     * @return true for live streams (NDI, V4L2, RTSP), false for files
     */
    virtual bool isLiveStream() const { return false; }  // Default: not live

    /**
     * For live streams: get the latest available frame
     * Default implementation calls readFrame(0, buffer)
     * @param buffer FrameBuffer to store the decoded frame
     * @return true on success, false on failure
     */
    virtual bool readLatestFrame(FrameBuffer& buffer) {
        return readFrame(0, buffer);
    }

    // --- Shared decoder cache ---
    // Used by driver layers to store decoded frames for secondary layers to read.
    // Cache stores frames by value inside InputSource (not raw pointers to external buffers).
    // Implemented in the base class — all subclasses inherit cache support automatically.

    bool hasCachedFrame() const { return hasCachedFrame_; }
    const GPUTextureFrameBuffer* getCachedGPUTexture() const { return cachedOnGPU_ ? &cachedGPU_ : nullptr; }
    const FrameBuffer* getCachedCPUFrame() const { return !cachedOnGPU_ ? &cachedCPU_ : nullptr; }
    int64_t getCachedFrameNumber() const { return cachedFrameNumber_; }
    bool isCachedOnGPU() const { return cachedOnGPU_; }

    void setCachedFrame(int64_t frameNumber, const GPUTextureFrameBuffer& gpu) {
        cachedGPU_ = gpu;               // non-owning copy (ownsTexture_=false via copy ctor)
        cachedFrameNumber_ = frameNumber;
        cachedOnGPU_ = true;
        hasCachedFrame_ = true;
    }

    void setCachedFrame(int64_t frameNumber, const FrameBuffer& cpu) {
        cachedCPU_.copyFrom(cpu);        // non-owning shallow copy (pointer + metadata, NOT pixels)
        cachedFrameNumber_ = frameNumber;
        cachedOnGPU_ = false;
        hasCachedFrame_ = true;
    }

    void invalidateCache() {
        hasCachedFrame_ = false;
        cachedFrameNumber_ = -1;
    }

private:
    // Shared decoder cache storage (only populated when this InputSource is used as a shared driver)
    bool hasCachedFrame_ = false;
    int64_t cachedFrameNumber_ = -1;
    GPUTextureFrameBuffer cachedGPU_;
    FrameBuffer cachedCPU_;
    bool cachedOnGPU_ = false;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_INPUTSOURCE_H

