#include "FramerateConverterSyncSource.h"
#include <cmath>

namespace videocomposer {

FramerateConverterSyncSource::FramerateConverterSyncSource(
    std::unique_ptr<SyncSource> syncSource,
    InputSource* inputSource)
    : wrappedSyncSource_(std::move(syncSource))
    , inputSource_(inputSource)
{
}

bool FramerateConverterSyncSource::connect(const char* param) {
    return wrappedSyncSource_->connect(param);
}

void FramerateConverterSyncSource::disconnect() {
    wrappedSyncSource_->disconnect();
}

bool FramerateConverterSyncSource::isConnected() const {
    return wrappedSyncSource_->isConnected();
}

int64_t FramerateConverterSyncSource::pollFrame(uint8_t* rolling) {
    // Get frame from wrapped sync source
    int64_t syncFrame = wrappedSyncSource_->pollFrame(rolling);
    
    // Match xjadeo's framerate conversion logic:
    // - Default (midi_clkconvert == 0): Use MTC fps info directly (no conversion)
    // - Only convert if framerates are different AND conversion is needed
    // - Use floor() for timescale (handled in LayerPlayback), rint() only for explicit resample mode
    // For now, we only convert if framerates differ significantly
    // This matches xjadeo's default behavior of using MTC fps directly
    if (syncFrame >= 0 && inputSource_) {
        double syncFps = wrappedSyncSource_->getFramerate();
        if (syncFps > 0) {
            FrameInfo info = inputSource_->getFrameInfo();
            double inputFps = info.framerate;
            
            // Only convert if both framerates are known and significantly different
            // Use floor() instead of rint() to match xjadeo's timescale behavior
            // This prevents rounding issues that cause frame skips
            if (inputFps > 0 && std::abs(syncFps - inputFps) > 0.01) {
                // Convert: inputFrame = floor(syncFrame * inputFps / syncFps)
                // Using floor() matches xjadeo's timescale approach and prevents rounding skips
                syncFrame = static_cast<int64_t>(std::floor(static_cast<double>(syncFrame) * inputFps / syncFps));
            }
        }
    }
    
    return syncFrame;
}

int64_t FramerateConverterSyncSource::getCurrentFrame() const {
    return wrappedSyncSource_->getCurrentFrame();
}

const char* FramerateConverterSyncSource::getName() const {
    return wrappedSyncSource_->getName();
}

double FramerateConverterSyncSource::getFramerate() const {
    // Return the input source's framerate if available, otherwise sync source's framerate
    if (inputSource_) {
        FrameInfo info = inputSource_->getFrameInfo();
        if (info.framerate > 0) {
            return info.framerate;
        }
    }
    return wrappedSyncSource_->getFramerate();
}

} // namespace videocomposer

