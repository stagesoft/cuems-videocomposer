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
     * Health of this input's decode path.
     *
     * A layer that loads but cannot produce frames used to be indistinguishable
     * from a healthy one: the engine's OSC load is fire-and-forget, so the node
     * reports the cue armed either way. This is the machine-readable state a
     * load-time health ping answers from.
     */
    enum class Health {
        ok,               // decoding as intended
        // Two ways to reach this, and they are found at different moments:
        //   - open() asked for hardware and was refused outright (the ladder's
        //     tier 3), known before any frame exists; or
        //   - open() got hardware and the FIRST DECODED FRAME came back in
        //     software anyway - a 4:2:2 clip on VAAPI does this. That one
        //     cannot be known until a frame exists, so it is DERIVED at read
        //     time in VideoFileInput::getHealth() rather than stored. Defect
        //     6(b): before that derivation the layer reported ok while the CPU
        //     did the work.
        // Either way the layer PLAYS. sw_fallback is not a failure state.
        sw_fallback,      // hardware refused this file, or quietly went soft
        // CURRENTLY NEVER EMITTED - it has no writer. It meant "recovered onto
        // a reduced surface pool"; that ladder was retired once measurement
        // showed no platform we run answers pool pressure with a decode error
        // (amdgpu evicts to GTT, the Intel iGPUs are UMA). Kept as part of the
        // health contract surface, but do not build on it without giving it a
        // writer first.
        degraded,
        declared_failed,  // recovery over; holding the last frame until reload
        load_failed       // never produced a first frame
    };

    /**
     * Current decode health. Default ok - inputs that cannot fail this way
     * (and those that do not track it yet) are honest to report ok.
     */
    virtual Health getHealth() const { return Health::ok; }

    /**
     * Human-readable detail behind getHealth(), or empty when healthy.
     */
    virtual std::string getHealthReason() const { return std::string(); }

    /**
     * Whether this source counts as a 4K-class decode session for the hang
     * guard.
     *
     * Decided once, when the file is opened and its metadata is known, so the
     * check at reveal is a counter comparison and never a probe. The default
     * is false and that is correct for every path that does not drive the
     * VCN: HAP layers decode DXT blobs on the CPU and upload textures, so
     * they neither consume a decode session nor risk the ring hang.
     */
    virtual bool isFourKClass() const { return false; }

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

