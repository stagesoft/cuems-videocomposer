/**
 * PresentationTiming.h - Frame pacing and vsync timing (like mpv's present_sync)
 * 
 * Tracks presentation timestamps from DRM page flip events to:
 * - Calculate actual vsync duration
 * - Detect skipped vsyncs (dropped frames)
 * - Provide accurate frame timing information
 */

#ifndef VIDEOCOMPOSER_PRESENTATIONTIMING_H
#define VIDEOCOMPOSER_PRESENTATIONTIMING_H

#include <cstdint>
#include <chrono>

namespace videocomposer {

/**
 * Stores timing information for a single presented frame
 */
struct PresentationEntry {
    int64_t ust = 0;              // Presentation timestamp (nanoseconds, monotonic)
    int64_t msc = 0;              // Vsync counter (frame number from display)
    int64_t vsync_duration = 0;   // Calculated vsync interval (ns)
    int64_t skipped_vsyncs = 0;   // Number of vsyncs skipped (dropped frames)
    int64_t display_time = 0;     // When frame was actually displayed (ns)
    bool valid = false;
};

/**
 * Tracks presentation timing for smooth frame pacing
 * 
 * Usage:
 *   PresentationTiming timing;
 *   timing.init(60.0);  // Expected 60fps
 *   
 *   // In page flip callback:
 *   timing.recordFlip(sec, usec, msc);
 *   
 *   // To check timing:
 *   auto info = timing.getInfo();
 *   if (info.skipped_vsyncs > 0) {
 *       LOG_WARNING << "Dropped " << info.skipped_vsyncs << " frame(s)";
 *   }
 */
class PresentationTiming {
public:
    PresentationTiming();
    ~PresentationTiming() = default;
    
    /**
     * Initialize with expected refresh rate
     * @param refreshHz Display refresh rate (e.g., 60.0)
     */
    void init(double refreshHz);
    
    /**
     * Set expected video framerate (for xjadeo-style timing)
     * When video fps < display fps, some vsync skips are expected and normal.
     * This suppresses warnings for expected skips.
     * @param videoFps Video framerate (e.g., 25.0), or 0 to expect every vsync
     */
    void setVideoFramerate(double videoFps);
    
    /**
     * Record a page flip event (called from DRM page flip handler)
     * @param sec Seconds from DRM event
     * @param usec Microseconds from DRM event
     * @param msc Vsync counter from DRM event
     */
    void recordFlip(unsigned int sec, unsigned int usec, unsigned int msc);
    
    /**
     * Get current presentation timing info
     */
    PresentationEntry getInfo() const;
    
    /**
     * Get calculated vsync duration (actual, not expected)
     * @return Vsync duration in nanoseconds, or 0 if not yet calculated
     */
    int64_t getVsyncDuration() const { return current_.vsync_duration; }
    
    /**
     * Get expected vsync duration based on refresh rate
     */
    int64_t getExpectedVsyncDuration() const { return expectedVsyncNs_; }
    
    /**
     * Get number of skipped vsyncs in last flip
     */
    int64_t getSkippedVsyncs() const { return current_.skipped_vsyncs; }
    
    /**
     * Get total dropped frames since init
     */
    int64_t getTotalDroppedFrames() const { return totalDroppedFrames_; }
    
    /**
     * Check if we're running behind (frames being dropped)
     */
    bool isRunningBehind() const { return current_.skipped_vsyncs > 0; }
    
    /**
     * Reset statistics
     */
    void reset();

private:
    PresentationEntry current_;
    PresentationEntry previous_;
    
    int64_t expectedVsyncNs_ = 0;      // Expected vsync duration based on refresh rate
    int64_t totalDroppedFrames_ = 0;   // Total dropped frames since init
    int64_t totalUnexpectedDrops_ = 0; // Drops beyond expected (actual problems)
    double displayHz_ = 0.0;           // Display refresh rate
    double videoFps_ = 0.0;            // Expected video framerate (0 = match display)
    int expectedVsyncsPerFrame_ = 1;   // Expected vsyncs between flips (display_hz / video_fps)
    bool initialized_ = false;
    
    // Convert DRM timestamp to nanoseconds
    static int64_t toNanoseconds(unsigned int sec, unsigned int usec);
    
    // Get current monotonic time in nanoseconds
    static int64_t getCurrentTimeNs();
};

} // namespace videocomposer

#endif // VIDEOCOMPOSER_PRESENTATIONTIMING_H

