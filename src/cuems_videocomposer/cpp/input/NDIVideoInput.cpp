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

#include "NDIVideoInput.h"
#include "../remote/JournalContract.h"
#include "../utils/Logger.h"
#include <cmath>
#include <cstring>
#include <thread>
#include <chrono>

namespace videocomposer {

NDIVideoInput::NDIVideoInput()
    : ready_(false)
    , frameCount_(0)
{
#ifdef HAVE_NDI_SDK
    ndiReceiver_ = nullptr;
    ndiFinder_ = nullptr;
#endif
    frameInfo_ = {};
}

NDIVideoInput::~NDIVideoInput() {
    close();
}

bool NDIVideoInput::initializeNDI() {
#ifdef HAVE_NDI_SDK
    if (!NDIlib_initialize()) {
        LOG_ERROR << "Failed to initialize NDI SDK";
        return false;
    }
    LOG_INFO << "NDI SDK initialized";
    return true;
#else
    LOG_ERROR << "NDI SDK not available (compiled without HAVE_NDI_SDK)";
    return false;
#endif
}

void NDIVideoInput::shutdownNDI() {
#ifdef HAVE_NDI_SDK
    if (ndiFinder_) {
        NDIlib_find_destroy(ndiFinder_);
        ndiFinder_ = nullptr;
    }
    NDIlib_destroy();
    LOG_INFO << "NDI SDK shutdown";
#endif
}

bool NDIVideoInput::connectToSource(const std::string& sourceName) {
#ifdef HAVE_NDI_SDK
    // Create finder to discover sources
    if (!ndiFinder_) {
        ndiFinder_ = NDIlib_find_create_v2();
        if (!ndiFinder_) {
            LOG_ERROR << "NDI: Failed to create finder";
            return false;
        }
    }

    LOG_INFO << "NDI: Searching for source '" << sourceName << "' (timeout: " << discoveryTimeoutMs_ << "ms)";

    // Poll for sources with timeout (more responsive than sleep).
    // An exact name wins for the whole discovery window: a substring hit
    // ("MYLAPTOP (OBS)" for "LAPTOP (OBS)") must not shadow the exact source
    // just because it was announced first. A substring match is accepted
    // only in one final pass after the window has run out.
    auto startTime = std::chrono::steady_clock::now();
    const NDIlib_source_t* selectedSource = nullptr;
    bool finalPass = false;
    
    while (true) {
        // Check for sources
        uint32_t numSources = 0;
        const NDIlib_source_t* sources = NDIlib_find_get_current_sources(ndiFinder_, &numSources);
        
        for (uint32_t i = 0; i < numSources && !selectedSource; i++) {
            if (sourceName == sources[i].p_ndi_name) {
                selectedSource = &sources[i];
            }
        }
        for (uint32_t i = 0; i < numSources && !selectedSource && finalPass; i++) {
            if (std::string(sources[i].p_ndi_name).find(sourceName) != std::string::npos) {
                selectedSource = &sources[i];
            }
        }
        
        if (selectedSource) {
            LOG_INFO << "NDI: Found source '" << selectedSource->p_ndi_name << "'";
            break;
        }
        
        if (!finalPass) {
            auto elapsed = std::chrono::steady_clock::now() - startTime;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >= discoveryTimeoutMs_) {
                finalPass = true;
                continue;
            }
        } else {
            LOG_ERROR << journal::ndiSourceNotFound(sourceName);
            
            // List available sources
            uint32_t finalNumSources = 0;
            const NDIlib_source_t* finalSources = NDIlib_find_get_current_sources(ndiFinder_, &finalNumSources);
            if (finalNumSources > 0) {
                LOG_INFO << "NDI: Available sources (" << finalNumSources << "):";
                for (uint32_t i = 0; i < finalNumSources; i++) {
                    LOG_INFO << "  - " << finalSources[i].p_ndi_name;
                }
            } else {
                LOG_INFO << "NDI: No sources found on network";
            }
            return false;
        }
        
        // Wait a bit before polling again
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    const std::string connectedName = selectedSource->p_ndi_name;

    // Create receiver
    NDIlib_recv_create_v3_t recv_desc;
    recv_desc.source_to_connect_to = *selectedSource;
    recv_desc.color_format = NDIlib_recv_color_format_BGRX_BGRA;  // BGRA format (matches OpenGL expectation)
    recv_desc.bandwidth = NDIlib_recv_bandwidth_highest;
    recv_desc.allow_video_fields = false;  // Progressive only
    recv_desc.p_ndi_recv_name = "cuems-videocomposer";  // Identify ourselves

    ndiReceiver_ = NDIlib_recv_create_v3(&recv_desc);
    if (!ndiReceiver_) {
        LOG_ERROR << "NDI: Failed to create receiver";
        return false;
    }

    LOG_INFO << journal::ndiConnected(connectedName);
    return true;
#else
    (void)sourceName;  // Unused
    return false;
#endif
}

bool NDIVideoInput::open(const std::string& source) {
    if (!initializeNDI()) {
        return false;
    }

    sourceName_ = source;
    
    // Remove "ndi://" prefix if present
    if (sourceName_.find("ndi://") == 0) {
        sourceName_ = sourceName_.substr(6);
    }
    if (sourceName_.empty()) {
        // An empty name is a substring of every source: refuse it rather
        // than connect to whichever source happens to be found.
        LOG_ERROR << "NDI: empty source name";
        shutdownNDI();
        return false;
    }

    if (!connectToSource(sourceName_)) {
        shutdownNDI();
        return false;
    }

    // Wait for first frame to get format info
#ifdef HAVE_NDI_SDK
    NDIlib_video_frame_v2_t video_frame;
    
    LOG_INFO << "NDI: Waiting for first frame (timeout: " << connectionTimeoutMs_ << "ms)";
    
    // The first capture right after connecting usually returns a status
    // change, not video: keep waiting for an actual video frame until the
    // timeout. Audio and metadata are not requested (NULL): this input only
    // shows video, and a requested audio frame must be freed by the caller.
    NDIlib_frame_type_e frame_type = NDIlib_frame_type_none;
    const auto firstFrameDeadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(connectionTimeoutMs_);
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            firstFrameDeadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            break;
        }
        frame_type = NDIlib_recv_capture_v2(ndiReceiver_, &video_frame, nullptr, nullptr,
                                            static_cast<uint32_t>(left));
        if (frame_type == NDIlib_frame_type_video || frame_type == NDIlib_frame_type_error) {
            break;
        }
    }
    
    if (frame_type == NDIlib_frame_type_video) {
        std::lock_guard<std::mutex> lock(frameInfoMutex_);
        frameInfo_.width = video_frame.xres;
        frameInfo_.height = video_frame.yres;
        frameInfo_.aspect = static_cast<float>(video_frame.xres) / static_cast<float>(video_frame.yres);
        
        if (video_frame.frame_rate_N > 0 && video_frame.frame_rate_D > 0) {
            frameInfo_.framerate = static_cast<double>(video_frame.frame_rate_N) / static_cast<double>(video_frame.frame_rate_D);
        } else {
            frameInfo_.framerate = 25.0;  // Default
        }
        
        frameInfo_.format = PixelFormat::BGRA32;  // BGRA matches OpenGL expectation
        frameInfo_.totalFrames = 0;  // Live stream, no total frames
        frameInfo_.duration = 0.0;
        frameInfoInvented_ = false;
        
        NDIlib_recv_free_video_v2(ndiReceiver_, &video_frame);
        
        LOG_INFO << journal::ndiSourceFormat(frameInfo_.width, frameInfo_.height, frameInfo_.framerate);
    } else {
        // Not a failure: the capture thread keeps receiving, and the first
        // frame that arrives replaces this guess (ndiSourceFormatUpdated).
        LOG_WARNING << journal::ndiNoVideoFrame();
        std::lock_guard<std::mutex> lock(frameInfoMutex_);
        frameInfo_.width = 1920;
        frameInfo_.height = 1080;
        frameInfo_.aspect = 16.0f / 9.0f;
        frameInfo_.framerate = 25.0;
        frameInfo_.format = PixelFormat::BGRA32;  // BGRA matches OpenGL expectation
        frameInfo_.totalFrames = 0;
        frameInfo_.duration = 0.0;
        frameInfoInvented_ = true;
    }
#endif

    ready_ = true;
    startCaptureThread();  // Start async capture
    return true;
}

void NDIVideoInput::close() {
    stopCaptureThread();  // Stop async capture first

#ifdef HAVE_NDI_SDK
    if (ndiReceiver_) {
        NDIlib_recv_destroy(ndiReceiver_);
        ndiReceiver_ = nullptr;
    }
#endif

    shutdownNDI();
    ready_ = false;
    sourceName_.clear();
    frameCount_ = 0;
    std::lock_guard<std::mutex> lock(frameInfoMutex_);
    frameInfoInvented_ = false;
}

bool NDIVideoInput::isReady() const {
    return ready_;
}

FrameInfo NDIVideoInput::getFrameInfo() const {
    std::lock_guard<std::mutex> lock(frameInfoMutex_);
    return frameInfo_;
}

bool NDIVideoInput::isFrameInfoProvisional() const {
    std::lock_guard<std::mutex> lock(frameInfoMutex_);
    return frameInfoInvented_;
}

void NDIVideoInput::updateFrameInfoFromCapture(int width, int height, double fps) {
    std::lock_guard<std::mutex> lock(frameInfoMutex_);
    // Frame rates are compared loosely: N/D jitter is not a format change.
    const bool fpsChanged = fps > 0.0 && std::abs(fps - frameInfo_.framerate) > 0.01;
    if (!frameInfoInvented_ && width == frameInfo_.width && height == frameInfo_.height && !fpsChanged) {
        return;
    }
    const bool wasInvented = frameInfoInvented_;
    frameInfo_.width = width;
    frameInfo_.height = height;
    frameInfo_.aspect = static_cast<float>(width) / static_cast<float>(height);
    if (fps > 0.0) {
        frameInfo_.framerate = fps;
    }
    frameInfoInvented_ = false;
    if (wasInvented) {
        LOG_INFO << journal::ndiSourceFormatUpdated(width, height, frameInfo_.framerate);
    } else {
        LOG_INFO << journal::ndiSourceFormatChanged(width, height, frameInfo_.framerate);
    }
}

int64_t NDIVideoInput::getCurrentFrame() const {
    return frameCount_.load();
}

InputSource::CodecType NDIVideoInput::detectCodec() const {
    // NDI streams are typically H.264 or similar, but we receive them as RGBA
    return CodecType::SOFTWARE;
}

InputSource::DecodeBackend NDIVideoInput::getOptimalBackend() const {
    return DecodeBackend::CPU_SOFTWARE;
}

bool NDIVideoInput::captureFrame(FrameBuffer& buffer) {
#ifdef HAVE_NDI_SDK
    if (!ndiReceiver_) {
        return false;
    }

    NDIlib_video_frame_v2_t video_frame;

    // Capture with a short timeout. Video only: a requested audio or
    // metadata frame would have to be freed here (OBS sends audio).
    NDIlib_frame_type_e frame_type = NDIlib_recv_capture_v2(
        ndiReceiver_, &video_frame, nullptr, nullptr, 100);

    if (frame_type == NDIlib_frame_type_video) {
        // Allocate buffer for frame
        FrameInfo info;
        info.width = video_frame.xres;
        info.height = video_frame.yres;
        info.aspect = static_cast<float>(video_frame.xres) / static_cast<float>(video_frame.yres);
        info.format = PixelFormat::BGRA32;  // BGRA matches OpenGL expectation
        double fps = 0.0;
        if (video_frame.frame_rate_N > 0 && video_frame.frame_rate_D > 0) {
            fps = static_cast<double>(video_frame.frame_rate_N) / static_cast<double>(video_frame.frame_rate_D);
            info.framerate = fps;
        }
        updateFrameInfoFromCapture(video_frame.xres, video_frame.yres, fps);
        
        if (!buffer.allocate(info)) {
            NDIlib_recv_free_video_v2(ndiReceiver_, &video_frame);
            return false;
        }

        // Copy frame data (NDI provides BGRA)
        size_t frameSize = video_frame.xres * video_frame.yres * 4;  // BGRA = 4 bytes per pixel
        if (buffer.size() >= frameSize) {
            memcpy(buffer.data(), video_frame.p_data, frameSize);
            frameCount_++;
            NDIlib_recv_free_video_v2(ndiReceiver_, &video_frame);
            return true;
        }

        NDIlib_recv_free_video_v2(ndiReceiver_, &video_frame);
        return false;
    }

    return false;
#else
    (void)buffer;  // Unused
    return false;
#endif
}

std::vector<std::string> NDIVideoInput::discoverSources(int timeoutMs) {
    std::vector<std::string> sources;

#ifdef HAVE_NDI_SDK
    if (!NDIlib_initialize()) {
        LOG_ERROR << "Failed to initialize NDI SDK for discovery";
        return sources;
    }

    NDIlib_find_instance_t finder = NDIlib_find_create_v2();
    if (!finder) {
        NDIlib_destroy();
        return sources;
    }

    // Wait for sources to be discovered
    std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));

    uint32_t numSources = 0;
    const NDIlib_source_t* foundSources = NDIlib_find_get_current_sources(finder, &numSources);

    for (uint32_t i = 0; i < numSources; i++) {
        sources.push_back(foundSources[i].p_ndi_name);
    }

    NDIlib_find_destroy(finder);
    NDIlib_destroy();
#endif

    return sources;
}

void NDIVideoInput::setTallyState(bool onProgram, bool onPreview) {
#ifdef HAVE_NDI_SDK
    if (ndiReceiver_) {
        NDIlib_tally_t tally;
        tally.on_program = onProgram;
        tally.on_preview = onPreview;
        NDIlib_recv_set_tally(ndiReceiver_, &tally);
    }
#else
    (void)onProgram;
    (void)onPreview;
#endif
}

} // namespace videocomposer

