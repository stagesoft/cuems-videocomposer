/**
 * MIDIBridge.cpp - C wrapper for C++ MIDI implementation
 * 
 * Bridges the legacy C MIDI API to the new C++ MIDISyncSource implementation.
 * This allows the old C code to use the new C++ MIDI driver.
 */

#include "../sync/MIDISyncSource.h"
#include "CLegacyBridge.h"
#include <cstring>
#include <cstdio>
#include <memory>

namespace videocomposer {

// Global MIDI sync source instance
static std::unique_ptr<MIDISyncSource> g_midiSyncSource;
static bool g_midiInitialized = false;

// Initialize MIDI sync source if not already done
static void initMIDI() {
    if (!g_midiInitialized) {
        g_midiSyncSource = std::make_unique<MIDISyncSource>();
        g_midiInitialized = true;
    }
}

} // namespace videocomposer

// Access to C global for driver name
extern char g_midi_driver_name[64];

extern "C" {

// C-compatible MIDI functions that bridge to C++ implementation

int midi_connected(void) {
    if (!videocomposer::g_midiInitialized || !videocomposer::g_midiSyncSource) {
        return 0;
    }
    return videocomposer::g_midiSyncSource->isConnected() ? 1 : 0;
}

int64_t midi_poll_frame(void) {
    if (!videocomposer::g_midiInitialized || !videocomposer::g_midiSyncSource) {
        return -1;
    }
    uint8_t rolling = 0;
    return videocomposer::g_midiSyncSource->pollFrame(&rolling);
}

void midi_open(char *midiid) {
    videocomposer::initMIDI();
    
    if (!videocomposer::g_midiSyncSource) {
        return;
    }
    
           // Ensure a MIDI driver is selected (if not already selected)
           // This handles the case where midi_open() is called before midi_choose_driver()
           extern char g_midi_driver_name[64];
           if (strlen(g_midi_driver_name) == 0 || 
               (strcmp(g_midi_driver_name, "ALSA-Sequencer") != 0 && 
                strcmp(g_midi_driver_name, "ALSA-Seq") != 0 &&
                strcmp(g_midi_driver_name, "ALSA") != 0 &&
                strcmp(g_midi_driver_name, "mtcreceiver") != 0 &&
                strcmp(g_midi_driver_name, "mtc") != 0)) {
               // Try to select mtcreceiver first (preferred), then ALSA Sequencer as fallback
               if (!videocomposer::g_midiSyncSource->chooseDriver("mtcreceiver")) {
                   if (videocomposer::g_midiSyncSource->chooseDriver("ALSA-Sequencer")) {
                       strncpy(g_midi_driver_name, "ALSA-Sequencer", sizeof(g_midi_driver_name) - 1);
                       g_midi_driver_name[sizeof(g_midi_driver_name) - 1] = '\0';
                   }
               } else {
                   strncpy(g_midi_driver_name, "mtcreceiver", sizeof(g_midi_driver_name) - 1);
                   g_midi_driver_name[sizeof(g_midi_driver_name) - 1] = '\0';
               }
           }
    
    // Set framerate from global (if available)
    extern double framerate;
    if (framerate > 0) {
        videocomposer::g_midiSyncSource->setFramerate(framerate);
    }
    
    // Set verbose mode
    extern int want_verbose;
    videocomposer::g_midiSyncSource->setVerbose(want_verbose != 0);
    
    // Set clock adjustment from global
    extern int midi_clkadj;
    videocomposer::g_midiSyncSource->setClockAdjustment(midi_clkadj != 0);
    
    // Connect to MIDI port
    const char* port = midiid ? midiid : "-1";
    
    // Debug: show what we're trying to do
    extern int want_verbose;
    if (want_verbose) {
        printf("midi_open: connecting to port '%s'\n", port);
        fflush(stdout);
    }
    
    bool connected = videocomposer::g_midiSyncSource->connect(port);
    
    if (connected) {
        if (!want_quiet) {
            printf("MIDI connected to ALSA Sequencer port\n");
            fflush(stdout);
        }
    } else {
        if (!want_quiet) {
            fprintf(stderr, "Failed to connect to MIDI port (port may still be visible in aconnect)\n");
            fflush(stderr);
        }
    }
}

void midi_close(void) {
    if (videocomposer::g_midiInitialized && videocomposer::g_midiSyncSource) {
        videocomposer::g_midiSyncSource->disconnect();
    }
}

const char* midi_driver_name(void) {
    extern char g_midi_driver_name[64];
    return g_midi_driver_name;
}

int midi_choose_driver(const char *driver) {
    videocomposer::initMIDI();
    
    if (!videocomposer::g_midiSyncSource) {
        return 0;
    }
    
    // JACK-MIDI support removed - reject it
    if (driver && !strcmp(driver, "JACK-MIDI")) {
        if (!want_quiet) {
            fprintf(stderr, "Warning: JACK-MIDI support has been removed. Using mtcreceiver instead.\n");
        }
        driver = "mtcreceiver";
    }
    
    // Try to choose the requested driver (mtcreceiver preferred, falls back to ALSA-Sequencer)
    if (driver) {
        bool success = videocomposer::g_midiSyncSource->chooseDriver(driver);
        if (success) {
            extern char g_midi_driver_name[64];
            strncpy(g_midi_driver_name, driver, sizeof(g_midi_driver_name) - 1);
            g_midi_driver_name[sizeof(g_midi_driver_name) - 1] = '\0';
            if (!want_quiet) {
                printf("selected MIDI driver: %s\n", driver);
            }
            return 1;
        } else {
            // If mtcreceiver failed, try ALSA-Sequencer as fallback
            if (driver && (!strcmp(driver, "mtcreceiver") || !strcmp(driver, "mtc"))) {
                if (!want_quiet) {
                    fprintf(stderr, "Warning: mtcreceiver driver not available, trying ALSA-Sequencer\n");
                }
                driver = "ALSA-Sequencer";
            }
            
            // Try ALSA-Sequencer and variants
            if (driver && (!strcmp(driver, "ALSA-Sequencer") || !strcmp(driver, "ALSA-Seq") || !strcmp(driver, "ALSA"))) {
                success = videocomposer::g_midiSyncSource->chooseDriver("ALSA-Sequencer");
                if (success) {
                    extern char g_midi_driver_name[64];
                    strncpy(g_midi_driver_name, "ALSA-Sequencer", sizeof(g_midi_driver_name) - 1);
                    g_midi_driver_name[sizeof(g_midi_driver_name) - 1] = '\0';
                    if (!want_quiet) {
                        printf("selected MIDI driver: ALSA-Sequencer\n");
                    }
                    return 1;
                } else {
                    if (!want_quiet) {
                        fprintf(stderr, "Warning: ALSA-Sequencer driver not available\n");
                    }
                }
            }
            
            // Reject unknown drivers
            if (!want_quiet) {
                fprintf(stderr, "Warning: MIDI driver '%s' not supported. Use mtcreceiver or ALSA-Sequencer.\n", driver);
            }
        }
    } else {
        // No driver specified - use default (mtcreceiver preferred)
        bool success = videocomposer::g_midiSyncSource->chooseDriver("mtcreceiver");
        if (!success) {
            success = videocomposer::g_midiSyncSource->chooseDriver("ALSA-Sequencer");
        }
        if (success) {
            extern char g_midi_driver_name[64];
            const char* selected = videocomposer::g_midiSyncSource->getCurrentDriverName();
            if (selected) {
                strncpy(g_midi_driver_name, selected, sizeof(g_midi_driver_name) - 1);
                g_midi_driver_name[sizeof(g_midi_driver_name) - 1] = '\0';
            }
            return 1;
        }
    }
    
    return 0;
}

} // extern "C"

