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
        initialized_ = true;
        
        LOG_INFO << "PresentationTiming: Initialized for " << refreshHz 
                 << "Hz (vsync=" << (expectedVsyncNs_ / 1000000.0) << "ms)";
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
                LOG_WARNING << "PresentationTiming: Dropped " << current_.skipped_vsyncs 
                           << " frame(s) (total: " << totalDroppedFrames_ << ")";
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
    // Keep expectedVsyncNs_ and initialized_ - they're set by init()
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

