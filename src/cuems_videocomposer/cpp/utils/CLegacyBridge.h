#ifndef VIDEOCOMPOSER_CLEGACYBRIDGE_H
#define VIDEOCOMPOSER_CLEGACYBRIDGE_H

/**
 * CLegacyBridge - Bridge between C++ code and legacy C code
 * 
 * Provides access to C globals that are still used by the legacy C display
 * backend code (display.c, display_x11.c, display_glx.c).
 * 
 * NOTE: This is a minimal bridge for the remaining C display code.
 * - Video file globals have been removed (C++ uses per-layer FrameInfo)
 * - MIDI functions have been removed (C++ uses MIDISyncSource directly)
 * - SMPTEWrapper.cpp still uses some globals for C compatibility
 */

extern "C" {
    // Framerate (used by C display code for screensaver timing, and SMPTEWrapper)
    extern double framerate;
    extern int have_dropframes;
    
    // Configuration flags (used by C display code for logging)
    extern int want_quiet;
    extern int want_verbose;
    extern int want_debug;
}

#endif // VIDEOCOMPOSER_CLEGACYBRIDGE_H

