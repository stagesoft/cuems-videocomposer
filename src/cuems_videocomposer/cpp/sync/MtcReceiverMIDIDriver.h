/**
 * MtcReceiverMIDIDriver.h - Adapter for mtcreceiver to MIDIDriver interface
 *
 * This adapter wraps the mtcreceiver library to provide MTC synchronization
 * using the proven mtcreceiver implementation from cuems-audioplayer.
 */

#ifndef VIDEOCOMPOSER_MTCRECEIVER_MIDI_DRIVER_H
#define VIDEOCOMPOSER_MTCRECEIVER_MIDI_DRIVER_H

#include "MIDIDriver.h"
#include "../../mtcreceiver/mtcreceiver.h"
#include <memory>
#include <mutex>

namespace videocomposer {

/**
 * MIDI driver using mtcreceiver library
 */
class MtcReceiverMIDIDriver : public MIDIDriver {
public:
    MtcReceiverMIDIDriver();
    ~MtcReceiverMIDIDriver() override;

    bool open(const std::string& portId = "") override;
    void close() override;
    bool isConnected() const override;
    int64_t pollFrame() override;
    
    // MIDIDriver interface
    const char* getName() const override { return "mtcreceiver"; }
    bool isSupported() const override { return true; }  // Always supported if compiled in
    
    // Additional configuration (not in base interface, but used by MIDISyncSource)
    void setFramerate(double framerate);
    void setVerbose(bool verbose);
    void setClockAdjustment(bool enable);
    
    // Check if last update was likely a full SYSEX frame (for seeking)
    bool wasLastUpdateFullFrame() const;

private:
    std::unique_ptr<MtcReceiver> mtcReceiver_;
    double framerate_;
    bool verbose_;
    bool clockAdjustment_;
    mutable std::mutex mutex_;
    
    // Track previous mtcHead to detect full frame jumps
    mutable long int lastMtcHead_;
    mutable bool lastWasFullFrame_;
    
    // Convert mtcreceiver milliseconds to frame number
    int64_t millisecondsToFrame(long int ms, double fps) const;
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_MTCRECEIVER_MIDI_DRIVER_H

