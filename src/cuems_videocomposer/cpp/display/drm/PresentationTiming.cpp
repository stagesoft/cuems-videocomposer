/**
 * PresentationTiming.cpp - Frame pacing implementation
 */

#include "PresentationTiming.h"
#include "../../utils/Logger.h"
#include <ctime>

namespace videocomposer {

PresentationTiming::PresentationTiming() {
    reset();
}

void PresentationTiming::init(double refreshHz) {
    reset();
    
    if (refreshHz > 0) {
        // Calculate expected vsync duration in nanoseconds
        // e.g., 60Hz -> 16,666,667 ns
        expectedVsyncNs_ = static_cast<int64_t>(1e9 / refreshHz);
        displayHz_ = refreshHz;
        initialized_ = true;
        
        LOG_INFO << "PresentationTiming: Initialized for " << refreshHz 
                 << "Hz (vsync=" << (expectedVsyncNs_ / 1000000.0) << "ms)";
    }
}

void PresentationTiming::setVideoFramerate(double videoFps) {
    videoFps_ = videoFps;
    
    // Calculate expected vsyncs between flips
    // e.g., 60Hz display / 25fps video = 2.4 vsyncs per video frame
    if (videoFps > 0 && displayHz_ > 0) {
        double ratio = displayHz_ / videoFps;
        // Round up: we might see 2 or 3 vsyncs per frame for a 2.4 ratio
        expectedVsyncsPerFrame_ = static_cast<int>(ratio + 0.5);
        if (expectedVsyncsPerFrame_ < 1) {
            expectedVsyncsPerFrame_ = 1;
        }
        
        LOG_INFO << "PresentationTiming: Video framerate set to " << videoFps 
                 << " fps (expecting ~" << expectedVsyncsPerFrame_ << " vsyncs per frame)";
    } else {
        expectedVsyncsPerFrame_ = 1;
    }
}

void PresentationTiming::recordFlip(unsigned int sec, unsigned int usec, unsigned int msc) {
    // Store previous entry
    previous_ = current_;
    
    // Record new timing
    current_.ust = toNanoseconds(sec, usec);
    current_.msc = static_cast<int64_t>(msc);
    current_.display_time = getCurrentTimeNs();
    current_.valid = true;
    
    // Calculate vsync duration and skipped frames if we have a previous entry
    if (previous_.valid && previous_.ust > 0 && current_.ust > previous_.ust) {
        int64_t ust_delta = current_.ust - previous_.ust;
        int64_t msc_delta = current_.msc - previous_.msc;
        
        // Avoid division by zero
        if (msc_delta > 0) {
            // Calculate actual vsync duration
            current_.vsync_duration = ust_delta / msc_delta;
            
            // Detect skipped vsyncs (msc_delta > 1 means we missed frames)
            // msc_delta of 1 = perfect, 2 = 1 skipped, etc.
            current_.skipped_vsyncs = msc_delta - 1;
            
            if (current_.skipped_vsyncs > 0) {
                totalDroppedFrames_ += current_.skipped_vsyncs;
                
                // With xjadeo-style timing (video fps < display fps), some skips are expected
                // Only count as "unexpected" if we skip more than expected
                // e.g., 25fps on 60Hz: expected msc_delta = 2-3, skips = 1-2
                int64_t expectedSkips = expectedVsyncsPerFrame_ - 1;  // e.g., 2-1=1 or 3-1=2
                int64_t unexpectedSkips = current_.skipped_vsyncs - expectedSkips;
                
                // Allow 1 vsync tolerance for timing jitter
                if (unexpectedSkips > 1) {
                    totalUnexpectedDrops_ += unexpectedSkips;
                    // Only log actual problems, not expected timing
                    if (totalUnexpectedDrops_ <= 5 || totalUnexpectedDrops_ % 60 == 0) {
                        LOG_WARNING << "PresentationTiming: Dropped " << unexpectedSkips 
                                   << " frame(s) beyond expected (total unexpected: " 
                                   << totalUnexpectedDrops_ << ")";
                    }
                }
            }
        } else if (msc_delta == 0) {
            // Same vsync - this shouldn't happen with proper page flipping
            // Keep previous vsync duration
            current_.vsync_duration = previous_.vsync_duration;
            current_.skipped_vsyncs = 0;
        } else {
            // msc went backwards (counter wrapped or reset) - reset timing
            current_.vsync_duration = expectedVsyncNs_;
            current_.skipped_vsyncs = 0;
        }
    } else if (expectedVsyncNs_ > 0) {
        // First frame or invalid previous - use expected duration
        current_.vsync_duration = expectedVsyncNs_;
        current_.skipped_vsyncs = 0;
    }
}

PresentationEntry PresentationTiming::getInfo() const {
    return current_;
}

void PresentationTiming::reset() {
    current_ = PresentationEntry();
    previous_ = PresentationEntry();
    totalDroppedFrames_ = 0;
    totalUnexpectedDrops_ = 0;
    // Keep expectedVsyncNs_, displayHz_, videoFps_, expectedVsyncsPerFrame_, initialized_ - they're set by init()/setVideoFramerate()
}

int64_t PresentationTiming::toNanoseconds(unsigned int sec, unsigned int usec) {
    // Convert seconds + microseconds to nanoseconds
    return static_cast<int64_t>(sec) * 1000000000LL + 
           static_cast<int64_t>(usec) * 1000LL;
}

int64_t PresentationTiming::getCurrentTimeNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

} // namespace videocomposer

