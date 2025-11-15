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
    , lastMtcHead_(0)
    , lastWasFullFrame_(false)
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
            
            // Use default constructor parameters like cuems-audioplayer does
            // Default: RtMidi::LINUX_ALSA, "Cuems Mtc Receiver", queueSizeLimit=100
            mtcReceiver_ = std::make_unique<MtcReceiver>();
            
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
    
    // Get current MTC head in milliseconds
    long int mtcHeadMs = MtcReceiver::mtcHead;
    
    // If mtcHead is 0, we haven't received any complete timecode yet
    if (mtcHeadMs == 0) {
        lastMtcHead_ = 0;
        lastWasFullFrame_ = false;
        return -1;
    }
    
    // Use explicit marker from mtcreceiver (feature_full_frame_marker branch)
    // This is 100% accurate - no heuristics needed
    // mtcreceiver sets wasLastUpdateFullFrame=true in decodeFullFrame()
    // and wasLastUpdateFullFrame=false in decodeQuarterFrame() when complete
    bool isFullFrame = MtcReceiver::wasLastUpdateFullFrame;
    
    lastWasFullFrame_ = isFullFrame;
    lastMtcHead_ = mtcHeadMs; // Keep tracking for potential fallback/debugging
    
    // Check if timecode is running (for rolling state)
    bool isRunning = MtcReceiver::isTimecodeRunning;
    
    // Debug: log timecode running state changes
    static bool lastRunningState = false;
    if (isRunning != lastRunningState) {
        printf("MTC: isTimecodeRunning changed: %s\n", isRunning ? "true" : "false");
        fflush(stdout);
        lastRunningState = isRunning;
    }
    
    // Get current frame rate from mtcreceiver
    unsigned char mtcFrameRate = MtcReceiver::curFrameRate;
    
    // Use mtcreceiver's frame rate if available, otherwise use our framerate
    double fps = framerate_;
    if (mtcFrameRate == 0) fps = 24.0;
    else if (mtcFrameRate == 1) fps = 25.0;
    else if (mtcFrameRate == 2) fps = 29.97;
    else if (mtcFrameRate == 3) fps = 30.0;
    
    // Convert milliseconds to frame number
    int64_t frame = millisecondsToFrame(mtcHeadMs, fps);
    
    // Return frame even if not "running" - we have valid MTC data
    // The rolling state will be determined separately
    
    if (verbose_ && frame >= 0) {
        // Log frame updates periodically
        LOG_INFO << "MTC: frame " << frame << " (mtcHead=" << mtcHeadMs << "ms, fps=" << fps << ", rolling)";
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

bool MtcReceiverMIDIDriver::wasLastUpdateFullFrame() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastWasFullFrame_;
}

int64_t MtcReceiverMIDIDriver::millisecondsToFrame(long int ms, double fps) const {
    if (fps <= 0.0) {
        return -1;
    }
    
    // Convert milliseconds to seconds, then to frames
    double seconds = ms / 1000.0;
    int64_t frame = static_cast<int64_t>(std::round(seconds * fps));
    
    return frame;
}

} // namespace videocomposer

