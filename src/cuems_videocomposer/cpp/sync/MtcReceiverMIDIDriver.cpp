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
    , lastFullFrameReceived_(false)
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
    
    // Check if a full frame was just received
    bool fullFrameReceived = MtcReceiver::wasLastUpdateFullFrame;
    
    // Get current timecode frame directly (like xjadeo - discrete updates)
    // xjadeo uses smpte_to_frame() which calculates: frame = f + fps * (s + 60*m + 3600*h)
    // This matches xjadeo's approach: only update when complete timecode is received
    // No incremental updates, no backwards jumps from mtcHead resets
    MtcFrame curFrame = MtcReceiver::getCurFrame();
    
    // For detecting resync vs seek: remember last frame we reported
    static int64_t lastReportedFrame = -1;
    
    // Check if we have valid timecode
    // Note: mtcHeadMs == 0 can be valid for timecode 00:00:00:00
    // So we also accept full frames (explicit position commands) even if mtcHead is 0
    long int mtcHeadMs = MtcReceiver::mtcHead.load();
    if (mtcHeadMs == 0 && !fullFrameReceived) {
        // No timecode received yet (never had any MTC data)
        // But if a full frame was just received, process it even if it's 00:00:00:00
        return -1;
    }
    
    // Check if timecode is running (for rolling state)
    bool isRunning = MtcReceiver::isTimecodeRunning.load();
    
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
    
    // Use mtcHead (milliseconds) for smooth interpolation instead of discrete SMPTE
    // This gives us sub-frame precision which is essential for smooth 60Hz playback
    // mtcHead is updated by libmtcmaster based on MTC quarter-frames + elapsed time
    double mtcSeconds = static_cast<double>(mtcHeadMs) / 1000.0;
    int64_t frame = static_cast<int64_t>(std::floor(mtcSeconds * fps));
    
    // Detect if full frame is a RESYNC (periodic, position matches) vs SEEK (position jump)
    // Resync full frames are sent periodically for network reliability (rtpmidid)
    // and should NOT trigger a forced seek if we're already at the right position
    bool isSeekFullFrame = false;
    if (fullFrameReceived) {
        // Check if the full frame position matches where we expect to be
        // Allow tolerance of 2 frames (accounts for MTC 8-quarter-frame delay)
        int64_t frameDiff = std::abs(frame - lastReportedFrame);
        if (lastReportedFrame < 0 || frameDiff > 2) {
            // Position jump or first full frame - this is a SEEK
            isSeekFullFrame = true;
            printf("MTC: Full frame SEEK - frame=%lld (was %lld), timecode=%s\n", 
                   (long long)frame, (long long)lastReportedFrame, curFrame.toString().c_str());
            fflush(stdout);
            if (verbose_) {
                LOG_INFO << "MTC: Full frame SEEK to frame " << frame 
                         << " (" << curFrame.toString() << ")";
            }
        } else {
            // Position matches - this is just a RESYNC for network reliability
            // Don't trigger seek, just update timing
            if (verbose_) {
                static int resyncCount = 0;
                if (++resyncCount % 10 == 0) {  // Log every 10th resync (~20 sec)
                    LOG_INFO << "MTC: Full frame resync at frame " << frame 
                             << " (network keepalive)";
                }
            }
        }
    }
    
    // Update last reported frame
    lastReportedFrame = frame;
    
    // Return frame even if not "running" - we have valid MTC data
    // The rolling state will be determined separately
    
    if (verbose_ && frame >= 0 && !fullFrameReceived) {
        // Log frame updates periodically (but not for full frames, already logged above)
        static int logCounter = 0;
        if (++logCounter % 60 == 0) {
            LOG_INFO << "MTC: frame " << frame << " (fps=" << fps << ", rolling)";
        }
    }
    
    // Apply clock adjustment if enabled
    if (clockAdjustment_) {
        // mtcreceiver already handles timing internally
        // We might need to add adjustment here if needed
    }
    
    // Store the full frame flag so it can be checked by the sync source
    // Only mark as "full frame received" if it's a SEEK, not a resync
    lastFullFrameReceived_ = isSeekFullFrame;
    
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

bool MtcReceiverMIDIDriver::wasFullFrameReceived() {
    std::lock_guard<std::mutex> lock(mutex_);
    bool result = lastFullFrameReceived_;
    // Reset the flag after checking (one-time notification)
    if (result) {
        lastFullFrameReceived_ = false;
        // Also reset the mtcreceiver flag so it doesn't trigger again
        MtcReceiver::wasLastUpdateFullFrame = false;
    }
    return result;
}

} // namespace videocomposer

