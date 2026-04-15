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
 * OutputSink.h - Abstract interface for virtual video outputs
 * 
 * Shared component for Multi-Display and NDI implementations.
 * Provides a unified interface for sending captured frames to various
 * virtual output destinations (NDI, streaming, file recording, etc.).
 */

#ifndef VIDEOCOMPOSER_OUTPUTSINK_H
#define VIDEOCOMPOSER_OUTPUTSINK_H

#include "FrameCapture.h"
#include <string>

namespace videocomposer {

/**
 * Output configuration for sinks
 */
struct OutputSinkConfig {
    int width = 1920;
    int height = 1080;
    double frameRate = 60.0;
    PixelFormat format = PixelFormat::RGBA32;
    std::string name;           // Sink name (e.g., NDI source name)
    
    // Additional codec/format settings (sink-specific)
    int bitrate = 0;            // For encoded outputs
    std::string codec;          // Codec name
    std::string container;      // Container format
};

/**
 * OutputSink - Abstract interface for virtual video outputs
 * 
 * Implementations:
 * - NDIVideoOutput: NDI streaming output
 * - StreamOutput: RTSP/WebRTC streaming (future)
 * - FileOutput: Recording to file (future)
 * - HardwareCaptureOutput: SDI/HDMI capture cards (future)
 */
class OutputSink {
public:
    /**
     * Output sink types
     */
    enum class Type {
        NDI,        // NDI network streaming
        HARDWARE,   // Hardware capture card output
        STREAMING,  // RTSP/WebRTC streaming
        FILE        // File recording
    };
    
    virtual ~OutputSink() = default;
    
    // ===== Lifecycle =====
    
    /**
     * Open the output sink
     * @param destination Sink-specific destination (NDI name, URL, path, etc.)
     * @param config Output configuration
     * @return true on success
     */
    virtual bool open(const std::string& destination, const OutputSinkConfig& config) = 0;
    
    /**
     * Close the output sink
     */
    virtual void close() = 0;
    
    /**
     * Check if the sink is open and ready
     */
    virtual bool isReady() const = 0;
    
    // ===== Frame Output =====
    
    /**
     * Write a frame to the output
     * Must be thread-safe - may be called from render thread
     * @param frame Frame data to write
     * @return true on success
     */
    virtual bool writeFrame(const FrameData& frame) = 0;
    
    // ===== Identification =====
    
    /**
     * Get sink type
     */
    virtual Type getType() const = 0;
    
    /**
     * Get unique ID for this sink
     */
    virtual std::string getId() const = 0;
    
    /**
     * Get human-readable description
     */
    virtual std::string getDescription() const = 0;
    
    // ===== Statistics =====
    
    /**
     * Get frames written since open
     */
    virtual int64_t getFramesWritten() const { return 0; }
    
    /**
     * Get frames dropped (couldn't be written in time)
     */
    virtual int64_t getFramesDropped() const { return 0; }
    
    /**
     * Get current output latency in milliseconds
     */
    virtual double getLatencyMs() const { return 0.0; }
};

/**
 * Type name helper
 */
inline const char* outputSinkTypeName(OutputSink::Type type) {
    switch (type) {
        case OutputSink::Type::NDI: return "NDI";
        case OutputSink::Type::HARDWARE: return "Hardware";
        case OutputSink::Type::STREAMING: return "Streaming";
        case OutputSink::Type::FILE: return "File";
        default: return "Unknown";
    }
}

} // namespace videocomposer

#endif // VIDEOCOMPOSER_OUTPUTSINK_H

