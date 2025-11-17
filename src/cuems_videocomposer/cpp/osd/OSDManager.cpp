#include "OSDManager.h"
#include "../utils/SMPTEUtils.h"
#include <sstream>
#include <iomanip>
#include <cmath>

namespace videocomposer {

OSDManager::OSDManager()
    : mode_(BOX)  // BOX enabled by default (matches xjadeo: OSD_mode = OSD_BOX in ui_osd_clear)
    , frameXAlign_(1)  // Center by default (OSD_CENTER = -2 in xjadeo, we use 1)
    , frameYPercent_(95)  // Default 95% from top (matches xjadeo's default when frame enabled)
    , smpteXAlign_(1)  // Center by default (OSD_CENTER = -2 in xjadeo, we use 1)
    , smpteYPercent_(89)  // Default 89% from top (matches xjadeo's default when SMPTE enabled)
    , textXAlign_(1)   // Center by default (OSD_CENTER = -2 in xjadeo, we use 1)
    , textYPercent_(50)  // Default 50% from top (center, matches xjadeo's default for text)
{
}

OSDManager::~OSDManager() {
}

void OSDManager::setFramePosition(int xAlign, int yPercent) {
    frameXAlign_ = xAlign;
    frameYPercent_ = yPercent;
}

void OSDManager::setFrameNumber(int64_t frame) {
    formatFrameNumber(frame);
}

void OSDManager::formatFrameNumber(int64_t frame) {
    std::ostringstream oss;
    oss << frame;
    frameText_ = oss.str();
}

void OSDManager::setSMPTEPosition(int xAlign, int yPercent) {
    smpteXAlign_ = xAlign;
    smpteYPercent_ = yPercent;
}

void OSDManager::setSMPTETimecode(const std::string& tc) {
    smpteText_ = tc;
}

void OSDManager::setText(const std::string& text) {
    text_ = text;
    if (!text_.empty()) {
        enableMode(TEXT);
    }
}

void OSDManager::setTextPosition(int xAlign, int yPercent) {
    textXAlign_ = xAlign;
    textYPercent_ = yPercent;
}

void OSDManager::setFontFile(const std::string& fontFile) {
    fontFile_ = fontFile;
}

void OSDManager::setMessage(const std::string& msg) {
    message_ = msg;
    if (!message_.empty()) {
        enableMode(MSG);
    }
}

void OSDManager::clear() {
    mode_ = BOX;  // Matches xjadeo: OSD_mode = OSD_BOX when clearing
    frameText_.clear();
    smpteText_.clear();
    text_.clear();
    message_.clear();
}

void OSDManager::formatSMPTE(int64_t frame, double framerate) {
    if (framerate <= 0.0) {
        smpteText_ = "00:00:00:00";
        return;
    }
    
    // Use SMPTEUtils for proper timecode formatting (handles drop-frames, etc.)
    smpteText_ = SMPTEUtils::frameToSmpteString(frame, framerate, false, false, false, true);
}

} // namespace videocomposer

