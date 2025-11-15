/**
 * TestMTCDecoder.cpp - Unit test for MTCDecoder class
 * 
 * This test verifies that the MTCDecoder correctly decodes MTC quarter-frame
 * messages and converts them to frame numbers.
 */

#include "sync/MTCDecoder.h"
#include "TestFramework.h"
#include <iostream>

using namespace videocomposer;

/**
 * Test MTC decoder directly
 */
bool test_MTCDecoder() {
    std::cout << "\n=== Testing MTCDecoder ===" << std::endl;
    
    MTCDecoder decoder;
    
    // Test quarter-frame messages for frame 1234 at 25fps
    // Frame 1234 = 00:00:49:09 (25fps)
    // Hour: 0, Min: 0, Sec: 49, Frame: 9
    
    // Quarter-frame messages (in order 0-7):
    // MTC format: upper nibble = quarter-frame index (0-7), lower nibble = data
    // Frame 1234 = 00:00:49:09 (25fps)
    // Hour: 0, Min: 0, Sec: 49 (0x31), Frame: 9 (0x09)
    
    uint8_t quarterFrames[] = {
        0x09,  // Quarter-frame 0: Frame low nibble (9)
        0x10,  // Quarter-frame 1: Frame high nibble (0)
        0x21,  // Quarter-frame 2: Seconds low nibble (1)
        0x33,  // Quarter-frame 3: Seconds high nibble (3) -> 49 seconds (0x31)
        0x40,  // Quarter-frame 4: Minutes low nibble (0)
        0x50,  // Quarter-frame 5: Minutes high nibble (0)
        0x60,  // Quarter-frame 6: Hours low nibble (0)
        0x72   // Quarter-frame 7: Hours high nibble bit 0 (0), type bits 1-2 (type 1 = 01)
    };
    
    // Process all quarter-frames
    bool complete = false;
    for (int i = 0; i < 8; i++) {
        complete = decoder.processByte(quarterFrames[i]);
    }
    
    // Verify complete timecode was received
    if (!complete) {
        std::cerr << "MTCDecoder test FAILED: Complete timecode not received" << std::endl;
        return false;
    }
    
    // Debug: Check decoded timecode
    const auto& tc = decoder.getTimecode();
    std::cout << "Decoded timecode: " << tc.hour << ":" << tc.min << ":" << tc.sec << ":" << tc.frame 
              << " (type=" << tc.type << ")" << std::endl;
    
    // Check result
    int64_t frame = decoder.timecodeToFrame(25.0);
    
    std::cout << "Decoded frame: " << frame << std::endl;
    std::cout << "Expected frame: 1234" << std::endl;
    
    if (frame == 1234) {
        std::cout << "MTCDecoder test PASSED" << std::endl;
        return true;
    } else {
        std::cerr << "MTCDecoder test FAILED: expected 1234, got " << frame << std::endl;
        return false;
    }
}

