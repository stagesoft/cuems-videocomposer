#include "LayerPlayback.h"
#include "../utils/Logger.h"
#include "../utils/SMPTEUtils.h"
#include "../sync/MIDISyncSource.h"
#include "../input/HAPVideoInput.h"
#include "../input/VideoFileInput.h"
#include <algorithm>
#include <cmath>

namespace videocomposer {

LayerPlayback::LayerPlayback()
    : playing_(false)
    , currentFrame_(-1)
    , lastSyncFrame_(-1)
    , timeOffset_(0)
    , timeScale_(1.0)
    , wraparound_(false)
    , mtcFollow_(true)  // Default: follow MTC
    , wasRolling_(false)
    , lastLoggedFrame_(-1)
    , debugCounter_(0)
    , loggedExceededDuration_(false)
    , frameOnGPU_(false)
{
}

LayerPlayback::~LayerPlayback() {
    pause();
}

void LayerPlayback::setInputSource(std::unique_ptr<InputSource> input) {
    pause();
    inputSource_ = std::move(input);
    currentFrame_ = -1;
    lastSyncFrame_ = -1;
    frameOnGPU_ = false;
}

void LayerPlayback::setSyncSource(std::unique_ptr<SyncSource> sync) {
    syncSource_ = std::move(sync);
    lastSyncFrame_ = -1;
}

bool LayerPlayback::play() {
    if (!isReady()) {
        return false;
    }
    playing_ = true;
    return true;
}

bool LayerPlayback::pause() {
    playing_ = false;
    return true;
}

bool LayerPlayback::seek(int64_t frameNumber) {
    if (!inputSource_) {
        return false;
    }
    
    if (inputSource_->seek(frameNumber)) {
        currentFrame_ = frameNumber;
        lastSyncFrame_ = -1;
        return true;
    }
    
    return false;
}

void LayerPlayback::update() {
    if (!isReady()) {
        return;
    }

    // Always check sync source, even when not playing
    // This allows automatic start when MTC is received
    if (syncSource_ && syncSource_->isConnected()) {
        updateFromSyncSource();
    } else if (playing_) {
        // No sync source but playing - manual playback mode
        // (This case can be handled separately if needed)
    }
}

void LayerPlayback::updateFromSyncSource() {
    if (!syncSource_ || !syncSource_->isConnected()) {
        // No sync source - manual playback or paused
        return;
    }
    
    // Check if MTC following is disabled for this layer
    if (!mtcFollow_) {
        // Layer ignores sync source - manual control only
        return;
    }

    // NOTE: These were previously static variables shared across ALL layers (bug!)
    // Now they are instance variables, each layer has its own state
    
    uint8_t rolling = 0;
    int64_t syncFrame = syncSource_->pollFrame(&rolling);
    
    // Debug: log frame and rolling state periodically
    debugCounter_++;
    if (debugCounter_ % 60 == 0) {  // Log every 60 calls (~1 second at 60fps)
        LOG_VERBOSE << "MTC poll: syncFrame=" << syncFrame << ", rolling=" << (int)rolling;
    }
    
    // Log MTC status changes
    if (rolling != 0 && !wasRolling_) {
        // MTC started rolling
        LOG_INFO << "MTC: Started rolling - playback starting (frame=" << syncFrame << ")";
        wasRolling_ = true;
    } else if (rolling == 0 && wasRolling_) {
        // MTC stopped rolling
        LOG_INFO << "MTC: Stopped rolling - playback paused";
        wasRolling_ = false;
    }
    
    // Also log when we have a frame but not rolling (for debugging)
    if (syncFrame >= 0 && rolling == 0 && debugCounter_ % 300 == 0) {
        LOG_VERBOSE << "MTC: Has frame " << syncFrame << " but not rolling (waiting for MTC to start)";
    }
    
    // Automatically start playing when MTC is rolling (rolling != 0)
    // OR when we have valid frame data (MTC is providing timecode)
    // This ensures playback starts automatically when MTC is received on startup
    // even if the rolling detection is temporarily false due to timing
    if ((rolling != 0 || syncFrame >= 0) && !playing_) {
        // Start playing if MTC is rolling OR we have valid frame data
        // This handles cases where rolling detection is temporarily false
        // but we're still receiving valid MTC timecode
        playing_ = true;
        if (syncFrame >= 0 && rolling == 0) {
            LOG_VERBOSE << "MTC: Starting playback with frame " << syncFrame << " (rolling detection pending)";
        }
    }
    
    // Update playing state based on rolling
    // Only pause if we truly have no MTC data (not just rolling=false)
    // If rolling stops but we still have frame data, keep playing
    // (MTC might be paused but still providing position)
    if (rolling == 0 && syncFrame < 0 && playing_) {
        // No MTC data at all - pause playback
        playing_ = false;
    }
    
    // Log MTC timecode periodically (every 30 frames or when frame changes significantly)
    if (syncFrame >= 0 && rolling != 0) {
        if (lastLoggedFrame_ < 0 || std::abs(syncFrame - lastLoggedFrame_) >= 30) {
            // Format timecode for display
            FrameInfo info = getFrameInfo();
            if (info.framerate > 0.0) {
                std::string smpte = SMPTEUtils::frameToSmpteString(syncFrame, info.framerate);
                LOG_INFO << "MTC: " << smpte << " (frame " << syncFrame << ", rolling)";
            } else {
                LOG_INFO << "MTC: frame " << syncFrame << " (rolling)";
            }
            lastLoggedFrame_ = syncFrame;
        }
    } else if (syncFrame >= 0 && rolling == 0) {
        // Log when we have a frame but not rolling (stopped)
        if (lastLoggedFrame_ != syncFrame) {
            FrameInfo info = getFrameInfo();
            if (info.framerate > 0.0) {
                std::string smpte = SMPTEUtils::frameToSmpteString(syncFrame, info.framerate);
                LOG_INFO << "MTC: " << smpte << " (frame " << syncFrame << ", stopped)";
            } else {
                LOG_INFO << "MTC: frame " << syncFrame << " (stopped)";
            }
            lastLoggedFrame_ = syncFrame;
        }
    }
    
    // Process frame updates only if we have a valid frame
    if (syncFrame >= 0) {
        // Apply time-scaling: multiply by timescale, then add offset
        // Note: Framerate conversion is handled by FramerateConverterSyncSource wrapper
        // LayerPlayback doesn't need to know about framerate conversion
        int64_t adjustedFrame = static_cast<int64_t>(std::floor(static_cast<double>(syncFrame) * timeScale_)) + timeOffset_;
        
        // Clamp frame to valid range (no automatic wrapping - use loop instead)
        if (inputSource_) {
            FrameInfo info = inputSource_->getFrameInfo();
            int64_t totalFrames = info.totalFrames;
            
            if (totalFrames > 0) {
                // Clamp to valid range instead of wrapping
                // Only log once to avoid flooding output
                if (adjustedFrame >= totalFrames) {
                    if (!loggedExceededDuration_) {
                        LOG_INFO << "Frame " << adjustedFrame << " exceeds video duration (" << totalFrames << "), clamping to " << (totalFrames - 1) << " (will not log again)";
                        loggedExceededDuration_ = true;
                    }
                    adjustedFrame = totalFrames - 1;
                } else if (adjustedFrame < 0) {
                    LOG_VERBOSE << "Frame " << adjustedFrame << " is negative, clamping to 0";
                    adjustedFrame = 0;
                    loggedExceededDuration_ = false; // Reset since we're back in valid range
                } else {
                    // Frame is in valid range, reset the flag
                    loggedExceededDuration_ = false;
                }
            }
        }
        
        // Match xjadeo's approach: always try to load the frame if it changed
        // xjadeo calls display_frame() every loop iteration, but only processes if frame changed
        // We do the same: poll MTC every update, but only load if frame changed
        // The seek logic inside loadFrame/readFrame will handle optimization
        // (no seek for consecutive frames, etc.)
        // xjadeo doesn't check for full SYSEX frames - it just uses the frame number
        // and lets seek_frame() decide whether to seek based on frame relationships
        if (adjustedFrame != lastSyncFrame_) {
            if (loadFrame(adjustedFrame)) {
                // Normal update - loadFrame() handles seek optimization internally
                // (no seek for consecutive frames, seeks for backwards/non-consecutive)
                currentFrame_ = adjustedFrame;
                lastSyncFrame_ = adjustedFrame;
            } else {
                // If load fails, try seeking first (helps with keyframe-based codecs)
                LOG_WARNING << "Failed to load frame " << adjustedFrame << ", trying seek first";
                if (inputSource_ && inputSource_->seek(adjustedFrame)) {
                    if (loadFrame(adjustedFrame)) {
                        currentFrame_ = adjustedFrame;
                        lastSyncFrame_ = adjustedFrame;
                    } else {
                        LOG_WARNING << "Failed to load frame " << adjustedFrame << " even after seek";
                    }
                } else {
                    LOG_WARNING << "Failed to seek to frame " << adjustedFrame;
                }
            }
        }
    } else {
        // No valid sync frame yet (MTC not received or not rolling)
        // Show frame 0 as fallback so video is visible while waiting for MTC
        // This matches original xjadeo behavior when newFrame < 0 (uses userFrame)
        if (inputSource_ && currentFrame_ < 0) {
            // Only load frame 0 once if we haven't loaded any frame yet
            // This ensures video is visible even when waiting for MTC
            if (loadFrame(0)) {
                currentFrame_ = 0;
                LOG_INFO << "Loaded frame 0 as fallback (waiting for MTC)";
                // Keep lastSyncFrame_ as -1 so we reload when MTC arrives
            } else {
                LOG_WARNING << "Failed to load frame 0 as fallback";
            }
        }
    }
}

bool LayerPlayback::loadFrame(int64_t frameNumber) {
    if (!inputSource_ || !inputSource_->isReady()) {
        return false;
    }

    // Check if this is a HAP codec
    HAPVideoInput* hapInput = dynamic_cast<HAPVideoInput*>(inputSource_.get());
    if (hapInput) {
#ifdef ENABLE_HAP_DIRECT
        // HAP with direct texture upload: decode directly to compressed DXT GPU texture
        if (hapInput->readFrameToTexture(frameNumber, gpuFrameBuffer_)) {
            frameOnGPU_ = true;
            LOG_VERBOSE << "Loaded HAP frame " << frameNumber << " to GPU (direct DXT upload)";
            return true;
        }
        // If direct upload fails, fall through to FFmpeg fallback (handled in readFrameToTexture)
        LOG_WARNING << "HAP direct decode failed for frame " << frameNumber;
        return false;
#else
        // HAP without direct upload: decode to CPU buffer as RGBA (FFmpeg fallback)
        if (hapInput->readFrame(frameNumber, cpuFrameBuffer_)) {
            frameOnGPU_ = false;
            return true;
        }
        return false;
#endif
    }
    
    // Check if this is VideoFileInput with hardware decoding
    VideoFileInput* videoInput = dynamic_cast<VideoFileInput*>(inputSource_.get());
    if (videoInput) {
        // Check if hardware decoding is available and should be used
        InputSource::DecodeBackend backend = videoInput->getOptimalBackend();
        if (backend == InputSource::DecodeBackend::GPU_HARDWARE) {
            // Hardware decoding: decode directly to GPU texture
            if (videoInput->readFrameToTexture(frameNumber, gpuFrameBuffer_)) {
                frameOnGPU_ = true;
                return true;
            }
            // If hardware decoding fails, fall through to software decoding
            LOG_WARNING << "Hardware decoding failed for frame " << frameNumber << ", falling back to software";
        }
        
        // Software decoding: use CPU frame buffer
        if (videoInput->readFrame(frameNumber, cpuFrameBuffer_)) {
            frameOnGPU_ = false;
            return true;
        }
        return false;
    }
    
    // Other input sources: use CPU frame buffer (default)
    if (inputSource_->readFrame(frameNumber, cpuFrameBuffer_)) {
        frameOnGPU_ = false;
        LOG_VERBOSE << "Loaded frame " << frameNumber << " to CPU buffer";
        return true;
    }
    return false;
}

bool LayerPlayback::getFrameBuffer(const FrameBuffer*& cpuBuffer, const GPUTextureFrameBuffer*& gpuBuffer) const {
    if (frameOnGPU_) {
        // Frame is on GPU - return pointer to GPU buffer
        gpuBuffer = &gpuFrameBuffer_;
        cpuBuffer = nullptr;
        return true; // true = on GPU
    } else {
        // Frame is on CPU - return pointer to CPU buffer
        cpuBuffer = &cpuFrameBuffer_;
        gpuBuffer = nullptr;
        return false; // false = on CPU
    }
}

bool LayerPlayback::isHAPCodec() const {
    if (!inputSource_) {
        return false;
    }
    
    InputSource::CodecType codec = inputSource_->detectCodec();
    return codec == InputSource::CodecType::HAP ||
           codec == InputSource::CodecType::HAP_Q ||
           codec == InputSource::CodecType::HAP_ALPHA;
}

bool LayerPlayback::isReady() const {
    return inputSource_ != nullptr && inputSource_->isReady();
}

FrameInfo LayerPlayback::getFrameInfo() const {
    if (inputSource_) {
        return inputSource_->getFrameInfo();
    }
    return FrameInfo();
}

void LayerPlayback::reverse() {
    // Reverse playback: multiply timescale by -1.0 and adjust offset
    // to keep current frame displayed
    if (currentFrame_ >= 0) {
        // Calculate new offset to maintain current frame position
        // newFrame = (syncFrame * -timeScale) + newOffset
        // We want: currentFrame = (syncFrame * -timeScale) + newOffset
        // So: newOffset = currentFrame - (syncFrame * -timeScale)
        // But we don't have syncFrame here, so we use a simpler approach:
        // Set offset to currentFrame and negate timescale
        timeOffset_ = currentFrame_;
        timeScale_ = -timeScale_;
    } else {
        // Just negate timescale if no current frame
        timeScale_ = -timeScale_;
    }
}

bool LayerPlayback::checkPlaybackEnd() const {
    if (!inputSource_ || !inputSource_->isReady()) {
        return false;
    }
    
    if (currentFrame_ < 0) {
        return false; // No frame loaded yet
    }
    
    FrameInfo info = inputSource_->getFrameInfo();
    int64_t totalFrames = info.totalFrames;
    
    if (totalFrames <= 0) {
        return false; // Unknown duration
    }
    
    // Check if current frame is at or beyond the end
    return currentFrame_ >= totalFrames;
}

} // namespace videocomposer

