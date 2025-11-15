#include "MIDIDriver.h"
#include "NullMIDIDriver.h"
#include "ALSASeqMIDIDriver.h"
#ifdef HAVE_MTCRECEIVER
#include "MtcReceiverMIDIDriver.h"
#endif
#include <algorithm>

namespace videocomposer {

std::unique_ptr<MIDIDriver> MIDIDriverFactory::create(const std::string& driverName) {
    if (driverName == "None" || driverName.empty()) {
        return std::make_unique<NullMIDIDriver>();
    }
    
    // Try case-insensitive match
    std::string lowerName = driverName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
    
#ifdef HAVE_MTCRECEIVER
    // mtcreceiver driver (preferred, proven implementation)
    if (lowerName == "mtcreceiver" || lowerName == "mtc-receiver" || lowerName == "mtc") {
        return std::make_unique<MtcReceiverMIDIDriver>();
    }
#endif
    
    // ALSA Sequencer driver
    if (lowerName == "alsa-sequencer" || lowerName == "alsa-seq" || lowerName == "alsa") {
        auto driver = std::make_unique<ALSASeqMIDIDriver>();
        if (driver->isSupported()) {
            return driver;
        }
        return nullptr;
    }
    
    return nullptr;
}

std::unique_ptr<MIDIDriver> MIDIDriverFactory::createFirstAvailable() {
    // Try drivers in order of preference
    // Following cuems-audioplayer pattern: mtcreceiver is preferred (proven working)
    
#ifdef HAVE_MTCRECEIVER
    // 1. mtcreceiver (preferred, proven working implementation from cuems-audioplayer)
    auto mtcDriver = std::make_unique<MtcReceiverMIDIDriver>();
    if (mtcDriver->isSupported()) {
        return mtcDriver;
    }
#endif
    
    // 2. ALSA Sequencer (fallback, Linux only, works with aconnect/Midi Through)
    auto alsaDriver = std::make_unique<ALSASeqMIDIDriver>();
    if (alsaDriver->isSupported()) {
        return alsaDriver;
    }
    
    // Fallback to null driver (always available)
    return std::make_unique<NullMIDIDriver>();
}

std::vector<std::string> MIDIDriverFactory::getAvailableDrivers() {
    std::vector<std::string> drivers;
    
    // Null driver is always available
    drivers.push_back("None");
    
#ifdef HAVE_MTCRECEIVER
    // mtcreceiver (preferred)
    auto mtcDriver = std::make_unique<MtcReceiverMIDIDriver>();
    if (mtcDriver->isSupported()) {
        drivers.push_back("mtcreceiver");
    }
#endif
    
    // ALSA Sequencer (check if supported)
    auto alsaDriver = std::make_unique<ALSASeqMIDIDriver>();
    if (alsaDriver->isSupported()) {
        drivers.push_back("ALSA-Sequencer");
    }
    
    return drivers;
}

} // namespace videocomposer

