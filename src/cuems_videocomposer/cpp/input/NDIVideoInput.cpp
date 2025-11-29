#include "NDIVideoInput.h"
#include "../utils/Logger.h"
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

    // Poll for sources with timeout (more responsive than sleep)
    auto startTime = std::chrono::steady_clock::now();
    const NDIlib_source_t* selectedSource = nullptr;
    
    while (true) {
        // Check for sources
        uint32_t numSources = 0;
        const NDIlib_source_t* sources = NDIlib_find_get_current_sources(ndiFinder_, &numSources);
        
        // Look for matching source (exact match or partial match)
        for (uint32_t i = 0; i < numSources; i++) {
            std::string ndiName = sources[i].p_ndi_name;
            if (ndiName == sourceName || ndiName.find(sourceName) != std::string::npos) {
                selectedSource = &sources[i];
                LOG_INFO << "NDI: Found source '" << ndiName << "'";
                break;
            }
        }
        
        if (selectedSource) break;
        
        // Check timeout
        auto elapsed = std::chrono::steady_clock::now() - startTime;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >= discoveryTimeoutMs_) {
            LOG_ERROR << "NDI: Source not found: " << sourceName;
            
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

    LOG_INFO << "NDI: Connected to source: " << sourceName;
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

    if (!connectToSource(sourceName_)) {
        shutdownNDI();
        return false;
    }

    // Wait for first frame to get format info
#ifdef HAVE_NDI_SDK
    NDIlib_video_frame_v2_t video_frame;
    NDIlib_audio_frame_v2_t audio_frame;
    NDIlib_metadata_frame_t metadata_frame;
    
    LOG_INFO << "NDI: Waiting for first frame (timeout: " << connectionTimeoutMs_ << "ms)";
    
    // Try to capture a frame with configurable timeout
    NDIlib_frame_type_e frame_type = NDIlib_recv_capture_v2(
        ndiReceiver_, &video_frame, &audio_frame, &metadata_frame, connectionTimeoutMs_);
    
    if (frame_type == NDIlib_frame_type_video) {
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
        
        NDIlib_recv_free_video_v2(ndiReceiver_, &video_frame);
        
        LOG_INFO << "NDI: Source format: " << frameInfo_.width << "x" << frameInfo_.height 
                 << " @ " << frameInfo_.framerate << " fps (BGRA)";
    } else {
        LOG_WARNING << "NDI: No video frame received, using defaults (1920x1080 @25fps)";
        frameInfo_.width = 1920;
        frameInfo_.height = 1080;
        frameInfo_.aspect = 16.0f / 9.0f;
        frameInfo_.framerate = 25.0;
        frameInfo_.format = PixelFormat::BGRA32;  // BGRA matches OpenGL expectation
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
}

bool NDIVideoInput::isReady() const {
    return ready_;
}

FrameInfo NDIVideoInput::getFrameInfo() const {
    return frameInfo_;
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
    NDIlib_audio_frame_v2_t audio_frame;
    NDIlib_metadata_frame_t metadata_frame;

    // Non-blocking capture with short timeout
    NDIlib_frame_type_e frame_type = NDIlib_recv_capture_v2(
        ndiReceiver_, &video_frame, &audio_frame, &metadata_frame, 100);

    if (frame_type == NDIlib_frame_type_video) {
        // Allocate buffer for frame
        FrameInfo info;
        info.width = video_frame.xres;
        info.height = video_frame.yres;
        info.aspect = static_cast<float>(video_frame.xres) / static_cast<float>(video_frame.yres);
        info.format = PixelFormat::BGRA32;  // BGRA matches OpenGL expectation
        
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

