#ifndef VIDEOCOMPOSER_FRAMERATECONVERTERSYNCSOURCE_H
#define VIDEOCOMPOSER_FRAMERATECONVERTERSYNCSOURCE_H

#include "SyncSource.h"
#include "../input/InputSource.h"

namespace videocomposer {

/**
 * FramerateConverterSyncSource - Wraps any SyncSource and converts framerate
 * 
 * This adapter converts frames from the sync source's framerate to the input
 * source's framerate using Option 2 (resample): videoFrame = rint(syncFrame * inputFps / syncFps)
 * 
 * This is timecode-agnostic and works with:
 * - Any sync source (MIDI, LTC, JACK, etc.)
 * - Any input source (video files, live feeds, streams, etc.)
 * 
 * The conversion is applied automatically if both framerates are known and different.
 */
class FramerateConverterSyncSource : public SyncSource {
public:
    /**
     * Create a framerate converter that wraps a sync source (non-owning reference)
     * @param syncSource The sync source to wrap (keeps reference, does not take ownership)
     * @param inputSource The input source to get target framerate from (keeps reference)
     */
    FramerateConverterSyncSource(SyncSource* syncSource, InputSource* inputSource);
    
    virtual ~FramerateConverterSyncSource() = default;

    // SyncSource interface - delegates to wrapped sync source
    bool connect(const char* param = nullptr) override;
    void disconnect() override;
    bool isConnected() const override;
    int64_t pollFrame(uint8_t* rolling = nullptr) override;
    int64_t getCurrentFrame() const override;
    const char* getName() const override;
    double getFramerate() const override;

    /**
     * Update the input source reference (e.g., when input source changes)
     * @param inputSource New input source (can be nullptr)
     */
    void setInputSource(InputSource* inputSource) { inputSource_ = inputSource; }

private:
    SyncSource* wrappedSyncSource_;  // Non-owning reference to sync source
    InputSource* inputSource_;  // Non-owning reference
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_FRAMERATECONVERTERSYNCSOURCE_H

