#include "VideoLayer.h"
#include "../utils/Logger.h"
#include "../utils/SMPTEUtils.h"
#include "../sync/MIDISyncSource.h"
#include <algorithm>
#include <cmath>

namespace videocomposer {

VideoLayer::VideoLayer()
    : playing_(false)
    , currentFrame_(-1)
    , lastSyncFrame_(-1)
    , timeOffset_(0)
    , timeScale_(1.0)
    , wraparound_(false)
    , layerId_(-1)
{
}

VideoLayer::~VideoLayer() {
    pause();
}

void VideoLayer::setInputSource(std::unique_ptr<InputSource> input) {
    pause();
    inputSource_ = std::move(input);
    currentFrame_ = -1;
    lastSyncFrame_ = -1;
}

void VideoLayer::setSyncSource(std::unique_ptr<SyncSource> sync) {
    syncSource_ = std::move(sync);
    lastSyncFrame_ = -1;
}

bool VideoLayer::play() {
    if (!isReady()) {
        return false;
    }
    playing_ = true;
    return true;
}

bool VideoLayer::pause() {
    playing_ = false;
    return true;
}

bool VideoLayer::seek(int64_t frameNumber) {
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

void VideoLayer::update() {
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

void VideoLayer::updateFromSyncSource() {
    if (!syncSource_ || !syncSource_->isConnected()) {
        // No sync source - manual playback or paused
        return;
    }

    static bool wasRolling = false;
    static int64_t lastLoggedFrame = -1;
    
    uint8_t rolling = 0;
    int64_t syncFrame = syncSource_->pollFrame(&rolling);
    
    // Check if last update was a full SYSEX frame (for seeking)
    // Full frames are used for positioning/seek operations (like original xjadeo)
    bool isFullFrame = false;
    MIDISyncSource* midiSyncSource = dynamic_cast<MIDISyncSource*>(syncSource_.get());
    if (midiSyncSource) {
        isFullFrame = midiSyncSource->wasLastUpdateFullFrame();
    }
    
    // Log MTC status changes
    if (rolling != 0 && !wasRolling) {
        // MTC started rolling
        LOG_INFO << "MTC: Started rolling - playback starting";
        wasRolling = true;
    } else if (rolling == 0 && wasRolling) {
        // MTC stopped rolling
        LOG_INFO << "MTC: Stopped rolling - playback paused";
        wasRolling = false;
    }
    
    // Automatically start playing when MTC is rolling (rolling != 0)
    // This ensures playback starts automatically when MTC is received on startup
    if (rolling != 0 && !playing_) {
        playing_ = true;
    }
    
    // Update playing state based on rolling
    // If rolling stops, pause playback
    if (rolling == 0 && playing_) {
        // Not rolling - pause playback
        playing_ = false;
    }
    
    // Log MTC timecode periodically (every 30 frames or when frame changes significantly)
    if (syncFrame >= 0 && rolling != 0) {
        if (lastLoggedFrame < 0 || std::abs(syncFrame - lastLoggedFrame) >= 30) {
            // Format timecode for display
            FrameInfo info = getFrameInfo();
            if (info.framerate > 0.0) {
                std::string smpte = SMPTEUtils::frameToSmpteString(syncFrame, info.framerate);
                LOG_INFO << "MTC: " << smpte << " (frame " << syncFrame << ", rolling)";
            } else {
                LOG_INFO << "MTC: frame " << syncFrame << " (rolling)";
            }
            lastLoggedFrame = syncFrame;
        }
    } else if (syncFrame >= 0 && rolling == 0) {
        // Log when we have a frame but not rolling (stopped)
        if (lastLoggedFrame != syncFrame) {
            FrameInfo info = getFrameInfo();
            if (info.framerate > 0.0) {
                std::string smpte = SMPTEUtils::frameToSmpteString(syncFrame, info.framerate);
                LOG_INFO << "MTC: " << smpte << " (frame " << syncFrame << ", stopped)";
            } else {
                LOG_INFO << "MTC: frame " << syncFrame << " (stopped)";
            }
            lastLoggedFrame = syncFrame;
        }
    }
    
    // Process frame updates only if we have a valid frame
    if (syncFrame >= 0) {
        // Apply time-scaling: multiply by timescale, then add offset
        // Note: Framerate conversion is handled by FramerateConverterSyncSource wrapper
        // VideoLayer doesn't need to know about framerate conversion
        int64_t adjustedFrame = static_cast<int64_t>(std::floor(static_cast<double>(syncFrame) * timeScale_)) + timeOffset_;
        
        // Always apply wraparound if frame exceeds video duration
        // This allows video to loop when MTC timecode exceeds video length
        if (inputSource_) {
            FrameInfo info = inputSource_->getFrameInfo();
            int64_t totalFrames = info.totalFrames;
            
            if (totalFrames > 0) {
                // Wrap around if frame is beyond duration
                if (adjustedFrame >= totalFrames) {
                    adjustedFrame = adjustedFrame % totalFrames;
                }
                // Wrap around if frame is negative
                if (adjustedFrame < 0) {
                    adjustedFrame = (adjustedFrame % totalFrames + totalFrames) % totalFrames;
                }
            }
        }
        
        // Only load frame if it's different from the last one
        // This avoids unnecessary seeks and frame loads
        if (adjustedFrame != lastSyncFrame_) {
            // Check for large jumps (more than 1 frame difference)
            // This might indicate a seek or transport jump
            bool isLargeJump = (lastSyncFrame_ >= 0 && 
                               std::abs(adjustedFrame - lastSyncFrame_) > 1);
            
            // Full SYSEX frames are used for seeking (like original xjadeo)
            // Always seek first when a full frame is received
            if (isFullFrame && inputSource_) {
                // Full frame received - seek first (matches original xjadeo behavior)
                if (inputSource_->seek(adjustedFrame)) {
                    if (loadFrame(adjustedFrame)) {
                        currentFrame_ = adjustedFrame;
                        lastSyncFrame_ = adjustedFrame;
                    }
                }
            } else if (loadFrame(adjustedFrame)) {
                // Normal quarter-frame update - try loading directly first
                currentFrame_ = adjustedFrame;
                lastSyncFrame_ = adjustedFrame;
            } else if (isLargeJump) {
                // On large jumps, if load fails, try seeking first
                // This helps with keyframe-based codecs
                if (inputSource_ && inputSource_->seek(adjustedFrame)) {
                    if (loadFrame(adjustedFrame)) {
                        currentFrame_ = adjustedFrame;
                        lastSyncFrame_ = adjustedFrame;
                    }
                }
            }
        }
    }
}

bool VideoLayer::loadFrame(int64_t frameNumber) {
    if (!inputSource_ || !inputSource_->isReady()) {
        return false;
    }

    return inputSource_->readFrame(frameNumber, frameBuffer_);
}

bool VideoLayer::render(FrameBuffer& outputBuffer) {
    if (!isReady() || !frameBuffer_.isValid()) {
        return false;
    }

    // For now, simple copy - full compositing will be done in display backend
    // This is a placeholder that ensures the frame is loaded
    if (currentFrame_ >= 0 && frameBuffer_.isValid()) {
        // Frame is ready for compositing
        return true;
    }

    return false;
}

bool VideoLayer::isReady() const {
    return inputSource_ != nullptr && inputSource_->isReady();
}

FrameInfo VideoLayer::getFrameInfo() const {
    if (inputSource_) {
        return inputSource_->getFrameInfo();
    }
    return FrameInfo();
}

void VideoLayer::reverse() {
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

} // namespace videocomposer

