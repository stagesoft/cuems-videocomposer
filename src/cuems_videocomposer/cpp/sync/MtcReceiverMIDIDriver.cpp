/**
 * MtcReceiverMIDIDriver.cpp - Adapter for mtcreceiver to MIDIDriver interface
 */

#include "MtcReceiverMIDIDriver.h"
#include "../utils/Logger.h"
#include <cmath>
#include <algorithm>
#include <rtmidi/RtMidi.h>
#include <thread>
#include <chrono>

namespace videocomposer {

MtcReceiverMIDIDriver::MtcReceiverMIDIDriver()
    : mtcReceiver_(nullptr)
    , framerate_(25.0)
    , verbose_(false)
    , clockAdjustment_(false)
{
}

MtcReceiverMIDIDriver::~MtcReceiverMIDIDriver() {
    close();
}

bool MtcReceiverMIDIDriver::open(const std::string& portId) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (mtcReceiver_) {
        // Already open
        return true;
    }
    
    // Retry logic similar to cuems-audioplayer (proven working pattern)
    bool errorFlag = true;
    int errorCount = 0;
    const int maxRetries = 10;
    
    while (errorFlag && errorCount < maxRetries) {
        try {
            // Create mtcreceiver instance using default parameters (like cuems-audioplayer)
            // Note: mtcreceiver automatically opens port 0 by default in constructor
            // This uses RtMidi which may not show up in aconnect the same way as ALSA Sequencer
            if (errorCount == 0) {
                printf("MTC: Initializing mtcreceiver (RtMidi)...\n");
                fflush(stdout);
            }
            
            // Create mtcreceiver with custom client name for aconnect -l
            // Use "cuems-videocomposer" to match ALSA Sequencer naming
            mtcReceiver_ = std::make_unique<MtcReceiver>(
                RtMidi::LINUX_ALSA,
                "cuems-videocomposer",
                100  // queueSizeLimit
            );
            
            errorFlag = false;
            
            // mtcreceiver constructor opens port 0 automatically
            printf("MTC: mtcreceiver initialized successfully, port opened\n");
            printf("MTC: Note: RtMidi ports may not appear in 'aconnect' - use 'aconnect -l' to see ALSA Sequencer ports\n");
            printf("MTC: Waiting for MIDI Time Code...\n");
            fflush(stdout);
            
            if (verbose_) {
                LOG_INFO << "MTC: mtcreceiver initialized successfully";
                LOG_INFO << "MTC: Waiting for MIDI Time Code...";
            }
            
            return true;
        } catch (const RtMidiError& error) {
            // Specific handling for RtMidi errors (like cuems-audioplayer)
            ++errorCount;
            if (errorCount < maxRetries) {
                printf("MTC: DRIVER_ERROR caught %d times, retrying...\n", errorCount);
                fflush(stdout);
                if (verbose_) {
                    LOG_WARNING << "MTC: DRIVER_ERROR caught " << errorCount << " times, retrying";
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } else {
                printf("MTC: ERROR - Failed to initialize mtcreceiver after %d retries: %s\n", 
                       maxRetries, error.getMessage().c_str());
                fflush(stdout);
                LOG_ERROR << "Failed to initialize mtcreceiver after " << maxRetries << " retries: " << error.getMessage();
                mtcReceiver_.reset();
                return false;
            }
        } catch (const std::exception& e) {
            // Handle other exceptions
            printf("MTC: ERROR - Failed to initialize mtcreceiver: %s\n", e.what());
            fflush(stdout);
            LOG_ERROR << "Failed to initialize mtcreceiver: " << e.what();
            mtcReceiver_.reset();
            return false;
        }
    }
    
    return false;
}

void MtcReceiverMIDIDriver::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (mtcReceiver_) {
        mtcReceiver_.reset();
        if (verbose_) {
            LOG_INFO << "MTC: mtcreceiver closed";
        }
    }
}

bool MtcReceiverMIDIDriver::isConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Consider connected if mtcreceiver instance exists (initialized and ready)
    // The actual MTC reception status is checked via pollFrame() and rolling state
    return mtcReceiver_ != nullptr;
}

int64_t MtcReceiverMIDIDriver::pollFrame() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!mtcReceiver_) {
        return -1;
    }
    
    // Get current timecode frame directly (like xjadeo - discrete updates)
    // xjadeo uses smpte_to_frame() which calculates: frame = f + fps * (s + 60*m + 3600*h)
    // This matches xjadeo's approach: only update when complete timecode is received
    // No incremental updates, no backwards jumps from mtcHead resets
    MtcFrame curFrame = MtcReceiver::getCurFrame();
    
    // Check if we have valid timecode by checking mtcHead (mtcreceiver sets this when timecode is received)
    // Note: curFrame can be 00:00:00:00 which is valid, so we check mtcHead instead
    long int mtcHeadMs = MtcReceiver::mtcHead;
    if (mtcHeadMs == 0) {
        return -1;
    }
    
    // Check if timecode is running (for rolling state)
    bool isRunning = MtcReceiver::isTimecodeRunning;
    
    // Debug: log timecode running state changes
    static bool lastRunningState = false;
    if (isRunning != lastRunningState) {
        printf("MTC: isTimecodeRunning changed: %s\n", isRunning ? "true" : "false");
        fflush(stdout);
        lastRunningState = isRunning;
    }
    
    // Get frame rate from MTC type (matches xjadeo's smpte_to_frame logic)
    double fps = framerate_;
    switch (curFrame.rate) {
        case 0: fps = 24.0; break;
        case 1: fps = 25.0; break;
        case 2: fps = 29.97; break;  // Drop-frame (29.97 fps)
        case 3: fps = 30.0; break;
        default: fps = framerate_; break;
    }
    
    // Calculate frame number directly from timecode components (like xjadeo)
    // This matches xjadeo's smpte_to_frame() calculation for non-dropframe:
    // frame = f + fps * (s + 60*m + 3600*h)
    // xjadeo only updates when complete timecode is received, so no backwards jumps
    int64_t totalSeconds = curFrame.hours * 3600 + curFrame.minutes * 60 + curFrame.seconds;
    int64_t frame = curFrame.frames + static_cast<int64_t>(fps * totalSeconds);
    
    // Return frame even if not "running" - we have valid MTC data
    // The rolling state will be determined separately
    
    if (verbose_ && frame >= 0) {
        // Log frame updates periodically
        LOG_INFO << "MTC: frame " << frame << " (fps=" << fps << ", rolling)";
    }
    
    // Apply clock adjustment if enabled
    if (clockAdjustment_) {
        // mtcreceiver already handles timing internally
        // We might need to add adjustment here if needed
    }
    
    return frame;
}

void MtcReceiverMIDIDriver::setFramerate(double framerate) {
    std::lock_guard<std::mutex> lock(mutex_);
    framerate_ = framerate;
}

void MtcReceiverMIDIDriver::setVerbose(bool verbose) {
    std::lock_guard<std::mutex> lock(mutex_);
    verbose_ = verbose;
}

void MtcReceiverMIDIDriver::setClockAdjustment(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    clockAdjustment_ = enable;
}

} // namespace videocomposer

