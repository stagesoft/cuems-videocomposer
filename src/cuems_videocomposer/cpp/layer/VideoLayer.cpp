#include "VideoLayer.h"
#include "../utils/Logger.h"
#include "../utils/SMPTEUtils.h"
#include "../sync/MIDISyncSource.h"
#include <algorithm>
#include <cmath>

namespace videocomposer {

VideoLayer::VideoLayer()
    : layerId_(-1)
    , frameBufferCacheValid_(false)
{
    // Set frame info in display when it changes
    // This will be updated when input source is set
}

VideoLayer::~VideoLayer() {
    pause();
}

void VideoLayer::setInputSource(std::unique_ptr<InputSource> input) {
    pause();
    playback_.setInputSource(std::move(input));
    
    // Update display with frame info
    if (playback_.isReady()) {
        FrameInfo info = playback_.getFrameInfo();
        display_.setFrameInfo(info);
        // Also set properties width/height from frame info if not set
        if (display_.getProperties().width == 0 && info.width > 0) {
            display_.getProperties().width = info.width;
        }
        if (display_.getProperties().height == 0 && info.height > 0) {
            display_.getProperties().height = info.height;
        }
    }
    
    // Invalidate frame buffer cache
    frameBufferCacheValid_ = false;
}

void VideoLayer::setSyncSource(std::unique_ptr<SyncSource> sync) {
    playback_.setSyncSource(std::move(sync));
}

bool VideoLayer::play() {
    if (!isReady()) {
        return false;
    }
    return playback_.play();
}

bool VideoLayer::pause() {
    return playback_.pause();
}

bool VideoLayer::seek(int64_t frameNumber) {
    bool result = playback_.seek(frameNumber);
    if (result) {
        // Invalidate frame buffer cache
        frameBufferCacheValid_ = false;
    }
    return result;
}

void VideoLayer::update() {
    if (!isReady()) {
        return;
    }

    // Update playback (polls sync source and loads frames)
    playback_.update();
    
    // Check for playback end and handle looping/auto-unload
    if (playback_.checkPlaybackEnd()) {
        auto& props = properties();
        FrameInfo info = playback_.getFrameInfo();
        int64_t currentFrame = playback_.getCurrentFrame();
        int64_t totalFrames = info.totalFrames;
        
        // Check for region loop first
        if (props.loopRegion.enabled && currentFrame >= props.loopRegion.endFrame) {
            // Region loop: check loop count
            if (props.loopRegion.currentLoopCount == -1) {
                // Infinite loop - always loop
                playback_.seek(props.loopRegion.startFrame);
                LOG_VERBOSE << "Region loop: seeking to startFrame " << props.loopRegion.startFrame;
            } else if (props.loopRegion.currentLoopCount > 0) {
                // Finite loop - decrement and loop if not zero
                props.loopRegion.currentLoopCount--;
                if (props.loopRegion.currentLoopCount > 0) {
                    playback_.seek(props.loopRegion.startFrame);
                    LOG_VERBOSE << "Region loop: " << props.loopRegion.currentLoopCount 
                                << " loops remaining, seeking to startFrame " << props.loopRegion.startFrame;
                } else {
                    // Loop count reached - disable region loop and continue
                    props.loopRegion.enabled = false;
                    LOG_VERBOSE << "Region loop: loop count reached, continuing playback";
                }
            } else {
                // Loop count is 0 - disable region loop
                props.loopRegion.enabled = false;
                LOG_VERBOSE << "Region loop: disabled, continuing playback";
            }
        } else if (currentFrame >= totalFrames) {
            // Full file end reached
            // Check if wraparound is enabled (full file loop)
            if (playback_.getWraparound()) {
                // Full file loop: check loop count
                if (props.fullFileLoopCount == -1) {
                    // Infinite loop - always loop
                    playback_.seek(0);
                    LOG_VERBOSE << "Full file loop: seeking to frame 0 (infinite)";
                } else if (props.fullFileLoopCount > 0) {
                    // Finite loop - decrement and loop if not zero
                    if (props.currentFullFileLoopCount == -1) {
                        props.currentFullFileLoopCount = props.fullFileLoopCount;
                    }
                    props.currentFullFileLoopCount--;
                    if (props.currentFullFileLoopCount > 0) {
                        playback_.seek(0);
                        LOG_VERBOSE << "Full file loop: " << props.currentFullFileLoopCount 
                                    << " loops remaining, seeking to frame 0";
                    } else {
                        // Loop count reached - disable wraparound and continue
                        playback_.setWraparound(false);
                        props.fullFileLoopCount = 0;
                        LOG_VERBOSE << "Full file loop: loop count reached, continuing playback";
                    }
                } else {
                    // Loop count is 0 - disable wraparound
                    playback_.setWraparound(false);
                    LOG_VERBOSE << "Full file loop: disabled, continuing playback";
                }
            } else {
                // No loop - playback has ended
                // Auto-unload will be handled by LayerManager::updateAll()
                LOG_VERBOSE << "Playback ended at frame " << currentFrame << " (total: " << totalFrames << ")";
            }
        }
    }
    
    // Get frame from playback (CPU or GPU)
    FrameBuffer cpuFrame;
    GPUTextureFrameBuffer gpuFrame;
    bool isFrameOnGPU = playback_.getFrameBuffer(cpuFrame, gpuFrame);
    
    // Prepare frame for display (apply modifications)
    bool isHAP = playback_.isHAPCodec();
    display_.setFrameInfo(playback_.getFrameInfo());
    bool prepared = display_.prepareFrame(cpuFrame, gpuFrame, isFrameOnGPU, isHAP);
    if (!prepared && (cpuFrame.isValid() || gpuFrame.isValid())) {
        LOG_WARNING << "VideoLayer::update() - frame is valid but prepareFrame() returned false";
    }
    
    // Invalidate frame buffer cache (will be rebuilt on demand)
    frameBufferCacheValid_ = false;
}

// updateFromSyncSource and loadFrame are now handled by LayerPlayback

bool VideoLayer::render(FrameBuffer& outputBuffer) {
    // Render is now handled by OpenGLRenderer using getPreparedFrame
    // This method is kept for backward compatibility
    return display_.isReady();
}

bool VideoLayer::isReady() const {
    // Layer is ready if playback is ready
    // Display doesn't need to be ready until we have a frame
    return playback_.isReady();
}

FrameInfo VideoLayer::getFrameInfo() const {
    return playback_.getFrameInfo();
}

bool VideoLayer::isHAPCodec() const {
    return playback_.isHAPCodec();
}

void VideoLayer::reverse() {
    playback_.reverse();
}

// Delegate property access to display
LayerProperties& VideoLayer::properties() {
    return display_.getProperties();
}

const LayerProperties& VideoLayer::properties() const {
    return display_.getProperties();
}

// Delegate time-scaling to playback
void VideoLayer::setTimeOffset(int64_t offset) {
    playback_.setTimeOffset(offset);
}

int64_t VideoLayer::getTimeOffset() const {
    return playback_.getTimeOffset();
}

void VideoLayer::setTimeScale(double scale) {
    playback_.setTimeScale(scale);
}

double VideoLayer::getTimeScale() const {
    return playback_.getTimeScale();
}

void VideoLayer::setWraparound(bool enabled) {
    playback_.setWraparound(enabled);
}

bool VideoLayer::getWraparound() const {
    return playback_.getWraparound();
}

void VideoLayer::setMtcFollow(bool enabled) {
    playback_.setMtcFollow(enabled);
}

bool VideoLayer::getMtcFollow() const {
    return playback_.getMtcFollow();
}

bool VideoLayer::isPlaying() const {
    return playback_.isPlaying();
}

int64_t VideoLayer::getCurrentFrame() const {
    return playback_.getCurrentFrame();
}

InputSource* VideoLayer::getInputSource() const {
    return playback_.getInputSource();
}

SyncSource* VideoLayer::getSyncSource() const {
    return playback_.getSyncSource();
}

// Backward compatibility: getFrameBuffer returns CPU frame buffer
const FrameBuffer& VideoLayer::getFrameBuffer() const {
    // Rebuild cache if needed
    if (!frameBufferCacheValid_) {
        FrameBuffer cpuBuffer;
        GPUTextureFrameBuffer gpuBuffer;
        bool isOnGPU = display_.getPreparedFrame(cpuBuffer, gpuBuffer);
        
        if (!isOnGPU && cpuBuffer.isValid()) {
            // Frame is on CPU - use it directly
            frameBufferCache_ = cpuBuffer;
            frameBufferCacheValid_ = true;
        } else if (isOnGPU) {
            // Frame is on GPU - we can't return it as CPU buffer
            // For now, return empty buffer (callers should use getPreparedFrame)
            frameBufferCache_.release();
            frameBufferCacheValid_ = false;
        } else {
            // No frame available
            frameBufferCache_.release();
            frameBufferCacheValid_ = false;
        }
    }
    
    return frameBufferCache_;
}

bool VideoLayer::getPreparedFrame(FrameBuffer& cpuBuffer, GPUTextureFrameBuffer& gpuBuffer) const {
    return display_.getPreparedFrame(cpuBuffer, gpuBuffer);
}

bool VideoLayer::isFrameOnGPU() const {
    return display_.isFrameOnGPU();
}

} // namespace videocomposer

