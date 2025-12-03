#!/usr/bin/env python3
"""
Test script for H.264 hardware decoding.

Tests that hardware decoding is being used for H.264 video files.
1. Starts videocomposer
2. Loads an H.264 video file
3. Checks logs for hardware decoder detection and usage
4. Verifies hardware decoding is active
"""

import subprocess
import time
import sys
import os
from pathlib import Path
import argparse
import re

try:
    import pythonosc.udp_client
    from pythonosc import osc_message_builder
except ImportError:
    print("ERROR: python-osc not installed. Install with: pip install python-osc")
    sys.exit(1)

# Import shared MTC helper
try:
    from mtc_helper import MTCHelper, MTC_AVAILABLE
except ImportError:
    print("WARNING: mtc_helper not found. MTC testing will be skipped.")
    MTC_AVAILABLE = False
    MTCHelper = None


class H264HardwareDecodingTest:
    def __init__(self, videocomposer_bin, video_file, osc_port=7770, fps=25.0, mtc_port=0):
        self.videocomposer_bin = Path(videocomposer_bin)
        self.video_file = Path(video_file) if video_file else None
        self.osc_port = osc_port
        self.fps = fps
        self.mtc_port = mtc_port
        self.videocomposer_process = None
        self.osc_client = None
        self.log_lines = []
        self.mtc_helper = None
        
        if MTC_AVAILABLE:
            self.mtc_helper = MTCHelper(fps=fps, port=mtc_port, portname="H264HardwareTest")
        
    def start_videocomposer(self):
        """Start the videocomposer application."""
        if not self.videocomposer_bin.exists():
            print(f"ERROR: videocomposer not found at {self.videocomposer_bin}")
            return False
        
        cmd = [str(self.videocomposer_bin)]
        
        # Add video file if provided
        if self.video_file and self.video_file.exists():
            cmd.append(str(self.video_file))
        
        # Add OSC port
        cmd.extend(["--osc", str(self.osc_port)])
        
        # Add MIDI port for MTC (use -1 for autodetect)
        cmd.extend(["--midi", "-1"])
        
        # Add verbose for debugging
        cmd.append("--verbose")
        
        try:
            env = os.environ.copy()
            # Don't force DISPLAY - let the application use the environment's DISPLAY
            # or fall back to DRM/KMS/headless mode if no display server is available
            
            self.videocomposer_process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                env=env
            )
            
            print(f"Started videocomposer (PID: {self.videocomposer_process.pid})")
            print(f"OSC port: {self.osc_port}")
            print(f"Video file: {self.video_file}")
            
            # Start log reader thread
            import threading
            def read_logs():
                for line in iter(self.videocomposer_process.stdout.readline, ''):
                    if line:
                        line = line.strip()
                        self.log_lines.append(line)
                        print(f"[videocomposer] {line}")
            
            log_thread = threading.Thread(target=read_logs, daemon=True)
            log_thread.start()
            
            time.sleep(3)  # Give it time to initialize
            return True
            
        except Exception as e:
            print(f"ERROR: Failed to start videocomposer: {e}")
            return False
    
    def connect_osc(self):
        """Connect to OSC server."""
        try:
            self.osc_client = pythonosc.udp_client.SimpleUDPClient('127.0.0.1', self.osc_port)
            print(f"Connected to OSC server on port {self.osc_port}")
            return True
        except Exception as e:
            print(f"ERROR: Failed to connect to OSC server: {e}")
            return False

    def send_osc(self, address, *args):
        """Send an OSC message."""
        if not self.osc_client:
            print("ERROR: OSC client not connected.")
            return False
        
        try:
            msg = osc_message_builder.OscMessageBuilder(address=address)
            for arg in args:
                msg.add_arg(arg)
            built_msg = msg.build()
            self.osc_client.send(built_msg)
            print(f"Sent OSC: {address} {args}")
            return True
        except Exception as e:
            print(f"ERROR: Failed to send OSC message {address} {args}: {e}")
            return False

    def check_hardware_decoder_detected(self):
        """Check if hardware decoder was detected in logs."""
        print("\n=== Checking for hardware decoder detection ===")
        
        patterns = [
            r"Hardware decoder detected: (QSV|VAAPI|CUDA|VideoToolbox|DXVA2)",
            r"Hardware decoder detected: QSV \(Intel Quick Sync Video\)",
            r"Hardware decoder detected: VAAPI \(Intel/AMD\)",
            r"Hardware decoder detected: CUDA \(NVIDIA\)",
        ]
        
        found = False
        for line in self.log_lines:
            for pattern in patterns:
                if re.search(pattern, line, re.IGNORECASE):
                    print(f"✓ Found: {line}")
                    found = True
                    break
        
        if not found:
            print("✗ No hardware decoder detection found in logs")
            print("  This might mean:")
            print("    - No hardware decoder is available")
            print("    - Hardware decoder detection failed")
            print("    - Logs haven't been captured yet")
        
        return found

    def check_hardware_decoder_used(self):
        """Check if hardware decoder is being used for H.264."""
        print("\n=== Checking for hardware decoder usage ===")
        
        patterns = [
            r"Attempting to open hardware decoder for codec: h264",
            r"Found hardware decoder: h264_(qsv|vaapi|cuda|videotoolbox|dxva2)",
            r"Successfully opened hardware decoder: h264_(qsv|vaapi|cuda|videotoolbox|dxva2)",
            r"Using HARDWARE decoding for h264",
            r"Hardware decoder available for codec: h264",
        ]
        
        found = False
        for line in self.log_lines:
            for pattern in patterns:
                if re.search(pattern, line, re.IGNORECASE):
                    print(f"✓ Found: {line}")
                    found = True
                    break
        
        if not found:
            print("✗ No hardware decoder usage found in logs")
            print("  Checking for fallback messages...")
            
            # Check for fallback messages
            fallback_patterns = [
                r"No hardware decoder available",
                r"Hardware decoder not available",
                r"falling back to software",
            ]
            
            for line in self.log_lines:
                for pattern in fallback_patterns:
                    if re.search(pattern, line, re.IGNORECASE):
                        print(f"  Found fallback: {line}")
                        break
        
        return found

    def check_software_decoding_fallback(self):
        """Check if software decoding fallback occurred."""
        print("\n=== Checking for software decoding fallback ===")
        
        patterns = [
            r"falling back to software",
            r"Opening software decoder",
            r"Successfully opened software decoder",
        ]
        
        found = False
        for line in self.log_lines:
            for pattern in patterns:
                if re.search(pattern, line, re.IGNORECASE):
                    print(f"  Found: {line}")
                    found = True
                    break
        
        return found

    def setup_mtc(self):
        """Setup MTC timecode."""
        if not MTC_AVAILABLE or not self.mtc_helper:
            print("WARNING: MTC not available, skipping MTC setup")
            return False
        
        print(f"Setting up MTC sender (fps={self.fps}, port={self.mtc_port})...")
        if self.mtc_helper.setup():
            print("MTC sender created successfully")
            return True
        else:
            print("ERROR: Failed to create MTC sender")
            return False
    
    def start_mtc(self, start_frame=0):
        """Start MTC playback."""
        if not MTC_AVAILABLE or not self.mtc_helper:
            print("WARNING: MTC not available, skipping MTC start")
            return False
        
        if self.mtc_helper.start(start_frame):
            print(f"MTC playback started from frame {start_frame}")
            return True
        else:
            print("ERROR: Failed to start MTC")
            return False
    
    def stop_mtc(self):
        """Stop MTC playback."""
        if self.mtc_helper:
            self.mtc_helper.stop()
            print("MTC playback stopped")
    
    def test_load_h264_file(self):
        """Test loading an H.264 file via OSC."""
        print("\n=== Test: Load H.264 file via OSC ===")
        
        if not self.video_file or not self.video_file.exists():
            print(f"ERROR: Video file not found: {self.video_file}")
            return False
        
        # Generate a test cue ID
        cue_id = "test-h264-hardware-00000000-0000-0000-0000-000000000001"
        
        # Load file via OSC
        print(f"Loading file: {self.video_file}")
        result = self.send_osc("/videocomposer/layer/load", str(self.video_file), cue_id)
        
        if result:
            print("File load command sent successfully")
            time.sleep(2)  # Wait for file to load
            
            # Set layer offset to 0 to ensure it starts from frame 0
            print("Setting layer offset to 0...")
            self.send_osc(f"/videocomposer/layer/{cue_id}/offset", 0)
            time.sleep(1)  # Wait for offset to be applied
            
            return True
        else:
            print("Failed to send file load command")
            return False

    def run_tests(self):
        """Run all tests."""
        print("=" * 60)
        print("H.264 Hardware Decoding Test")
        print("=" * 60)
        
        # Step 0: Setup MTC (before starting videocomposer)
        print("\n--- Step 0: Setup MTC Timecode ---")
        if not self.setup_mtc():
            print("WARNING: MTC setup failed, but continuing test...")
        
        # Step 1: Start videocomposer
        print("\n--- Step 1: Start Videocomposer ---")
        if not self.start_videocomposer():
            return False
        
        if not self.connect_osc():
            self.stop()
            return False
        
        time.sleep(2)  # Wait for initialization
        
        # Step 1.5: Start MTC from frame 0 (before loading video)
        print("\n--- Step 1.5: Start MTC from Frame 0 ---")
        if not self.start_mtc(start_frame=0):
            print("WARNING: MTC start failed, but continuing test...")
        time.sleep(1)  # Give MTC time to initialize
        
        # Step 2: Check hardware decoder detection
        print("\n--- Step 2: Check Hardware Decoder Detection ---")
        hw_detected = self.check_hardware_decoder_detected()
        time.sleep(1)
        
        # Step 3: Load H.264 file
        print("\n--- Step 3: Load H.264 Video File ---")
        if not self.test_load_h264_file():
            self.stop()
            return False
        
        # Step 4: Check hardware decoder usage
        print("\n--- Step 4: Check Hardware Decoder Usage ---")
        time.sleep(2)  # Wait for decoding to start
        hw_used = self.check_hardware_decoder_used()
        
        # Step 5: Check for software fallback
        print("\n--- Step 5: Check for Software Fallback ---")
        sw_fallback = self.check_software_decoding_fallback()
        
        # Summary
        print("\n" + "=" * 60)
        print("Test Summary")
        print("=" * 60)
        print(f"Hardware decoder detected: {'✓ YES' if hw_detected else '✗ NO'}")
        print(f"Hardware decoder used:     {'✓ YES' if hw_used else '✗ NO'}")
        print(f"Software fallback:         {'⚠ YES' if sw_fallback else '✓ NO'}")
        
        if hw_detected and hw_used:
            print("\n✓ SUCCESS: Hardware decoding is working!")
            return True
        elif hw_detected and not hw_used:
            print("\n⚠ WARNING: Hardware decoder detected but not used")
            print("  This might mean:")
            print("    - The codec doesn't have a hardware decoder available")
            print("    - Hardware decoder initialization failed")
            return False
        else:
            print("\n✗ FAILED: Hardware decoding not working")
            print("  Possible reasons:")
            print("    - No hardware decoder available on this system")
            print("    - FFmpeg doesn't have hardware decoder support compiled")
            print("    - Hardware decoder detection failed")
            return False
    
    def stop(self):
        """Stop videocomposer and MTC."""
        if self.videocomposer_process:
            print("\nStopping videocomposer...")
            self.videocomposer_process.terminate()
            try:
                self.videocomposer_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.videocomposer_process.kill()
            print("Videocomposer stopped")
        
        self.stop_mtc()
        
        if self.mtc_helper:
            self.mtc_helper.cleanup()


def main():
    parser = argparse.ArgumentParser(description="Test H.264 hardware decoding")
    parser.add_argument("--videocomposer", default="scripts/cuems-videocomposer-wrapper.sh",
                       help="Path to videocomposer wrapper script")
    parser.add_argument("--video", required=True,
                       help="Path to H.264 test video file")
    parser.add_argument("--osc-port", type=int, default=7770,
                       help="OSC port (default: 7770)")
    parser.add_argument("--fps", type=float, default=25.0,
                       help="MTC framerate (default: 25.0)")
    parser.add_argument("--mtc-port", type=int, default=0,
                       help="ALSA MIDI port number for MTC (default: 0)")
    
    args = parser.parse_args()
    
    test = H264HardwareDecodingTest(
        args.videocomposer,
        args.video,
        args.osc_port,
        args.fps,
        args.mtc_port
    )
    
    try:
        success = test.run_tests()
        
        if success:
            print("\n✓ Test completed successfully!")
            print("Hardware decoding is working correctly.")
        else:
            print("\n✗ Test completed with issues.")
            print("Check the logs above for details.")
        
        # Keep running for a bit to see ongoing behavior
        print("\nKeeping videocomposer running for 5 seconds to observe behavior...")
        time.sleep(5)
        
    except KeyboardInterrupt:
        print("\n\nInterrupted by user")
    finally:
        test.stop()
        
    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())

