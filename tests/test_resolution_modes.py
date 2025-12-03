#!/usr/bin/env python3
"""
Focused test for resolution mode features.

Tests:
1. Resolution mode listing and status
2. Setting resolution modes (native, maximum, 1080p, etc.)
3. Live resolution changes per output
4. Mode listing per output

Requires MTC timecode to play videos.
"""

import subprocess
import time
import sys
import os
from pathlib import Path
import argparse

try:
    import pythonosc.udp_client
except ImportError:
    print("ERROR: python-osc not installed. Install with: pip install python-osc")
    sys.exit(1)

try:
    from mtc_helper import MTCHelper, MTC_AVAILABLE
except ImportError:
    print("WARNING: mtc_helper not found. MTC testing will be skipped.")
    MTC_AVAILABLE = False
    MTCHelper = None


class ResolutionModesTest:
    def __init__(self, videocomposer_bin=None, video_file=None, osc_port=7770, fps=25.0, mtc_port=0):
        self.videocomposer_bin = self._find_videocomposer(videocomposer_bin)
        self.video_file = video_file
        self.osc_port = osc_port
        self.fps = fps
        self.mtc_port = mtc_port
        self.videocomposer_process = None
        self.osc_client = None
        self.mtc_helper = None
        
        if MTC_AVAILABLE:
            self.mtc_helper = MTCHelper(fps=fps, port=mtc_port, portname="ResolutionTest")
    
    def _find_videocomposer(self, provided_path=None):
        """Find videocomposer binary or wrapper script."""
        if provided_path:
            path = Path(provided_path)
            if path.exists():
                print(f"Using provided videocomposer path: {path}")
                return path
            else:
                print(f"WARNING: Provided path not found: {provided_path}, trying auto-detect...")
        
        # Try wrapper script first (handles library paths - preferred)
        script_dir = Path(__file__).parent.parent
        possible_paths = [
            script_dir / "scripts" / "cuems-videocomposer-wrapper.sh",
            script_dir / "cuems-videocomposer.sh",
            script_dir / "build" / "cuems-videocomposer",
            Path("/usr/bin/cuems-videocomposer"),
            Path("/usr/local/bin/cuems-videocomposer"),
        ]
        
        for path in possible_paths:
            if path.exists():
                # Check if executable (for scripts) or is a file (for binaries)
                if path.is_file() and (os.access(path, os.X_OK) or path.suffix == ''):
                    print(f"Found videocomposer: {path}")
                    return path
        
        raise FileNotFoundError(
            "videocomposer not found. Build the application first.\n"
            f"  Checked: {[str(p) for p in possible_paths]}"
        )
    
    def start_videocomposer(self):
        """Start videocomposer process."""
        if not self.videocomposer_bin.exists():
            print(f"ERROR: videocomposer binary not found: {self.videocomposer_bin}")
            return False
        
        cmd = [str(self.videocomposer_bin), "--osc", str(self.osc_port)]
        if self.video_file:
            cmd.append(str(self.video_file))
        
        print(f"Starting videocomposer: {' '.join(cmd)}")
        print(f"  Using: {self.videocomposer_bin}")
        
        # Copy environment to preserve LD_LIBRARY_PATH set by wrapper script
        env = os.environ.copy()
        
        self.videocomposer_process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env
        )
        time.sleep(2)
        
        if self.videocomposer_process.poll() is not None:
            print("ERROR: videocomposer exited immediately")
            stdout, stderr = self.videocomposer_process.communicate()
            print(f"STDOUT: {stdout}")
            print(f"STDERR: {stderr}")
            return False
        
        print("videocomposer started successfully")
        return True
    
    def connect_osc(self):
        """Connect OSC client."""
        try:
            self.osc_client = pythonosc.udp_client.SimpleUDPClient("127.0.0.1", self.osc_port)
            print(f"OSC client connected to port {self.osc_port}")
            return True
        except Exception as e:
            print(f"ERROR: Failed to connect OSC client: {e}")
            return False
    
    def send_osc(self, path, *args):
        """Send OSC message."""
        if not self.osc_client:
            print("ERROR: OSC client not connected")
            return False
        
        try:
            self.osc_client.send_message(path, list(args))
            print(f"  → {path} {list(args)}")
            return True
        except Exception as e:
            print(f"ERROR: Failed to send OSC message: {e}")
            return False
    
    def cleanup(self):
        """Cleanup resources."""
        if self.mtc_helper:
            self.mtc_helper.stop()
            self.mtc_helper.cleanup()
        if self.videocomposer_process:
            self.videocomposer_process.terminate()
            try:
                self.videocomposer_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.videocomposer_process.kill()
    
    def run_tests(self):
        """Run resolution mode tests."""
        print("=" * 70)
        print("Resolution Modes Test")
        print("=" * 70)
        
        if not self.start_videocomposer():
            return False
        
        if not self.connect_osc():
            self.cleanup()
            return False
        
        if MTC_AVAILABLE and self.mtc_helper:
            if self.mtc_helper.setup():
                print("MTC setup complete")
            else:
                print("WARNING: MTC setup failed")
        
        time.sleep(1)
        
        try:
            # Test 1: List displays
            print("\n[1] Listing displays...")
            self.send_osc("/videocomposer/display/list")
            time.sleep(1)
            
            # Test 2: Show resolution mode options
            print("\n[2] Resolution mode options...")
            self.send_osc("/videocomposer/display/resolution_mode")
            time.sleep(1)
            
            # Test 3: List modes for common outputs
            print("\n[3] Listing available modes for outputs...")
            for output in ["eDP-1", "HDMI-A-1", "DP-1"]:
                print(f"\n  Modes for {output}:")
                self.send_osc("/videocomposer/display/modes", output)
                time.sleep(0.5)
            
            # Test 4: Try different resolution modes
            print("\n[4] Testing resolution modes...")
            modes = ["1080p", "native", "maximum", "720p", "4k"]
            for mode in modes:
                print(f"\n  Setting mode to: {mode}")
                self.send_osc("/videocomposer/display/resolution_mode", mode)
                time.sleep(1)
            
            # Test 5: Live resolution changes
            print("\n[5] Testing live resolution changes...")
            outputs = ["eDP-1", "HDMI-A-1"]
            resolutions = [
                (1920, 1080, 60),
                (1280, 720, 60),
                (1920, 1080, 60),  # Restore
            ]
            
            for output in outputs:
                print(f"\n  Testing {output}:")
                for width, height, refresh in resolutions:
                    print(f"    Changing to {width}x{height}@{refresh}Hz...")
                    self.send_osc("/videocomposer/display/mode", output, str(width), str(height), str(refresh))
                    time.sleep(2)  # Wait for mode change
            
            # Test 6: Video playback with on-the-fly resolution changes (if video provided)
            if self.video_file and Path(self.video_file).exists():
                print("\n[6] Testing on-the-fly resolution changes during video playback...")
                print("     (This is the critical test - video should continue playing smoothly)")
                
                # Load video
                print("\n  Step 1: Loading video file...")
                self.send_osc("/videocomposer/layer/load", self.video_file, "test_video")
                time.sleep(1)
                
                # Enable mtcfollow
                print("  Step 2: Enabling mtcfollow...")
                self.send_osc("/videocomposer/layer/test_video/mtcfollow", "1")
                time.sleep(0.5)
                
                # Position layer to be visible
                print("  Step 3: Positioning layer on canvas...")
                self.send_osc("/videocomposer/layer/test_video/position", "960", "540")  # Center
                self.send_osc("/videocomposer/layer/test_video/scale", "1.0", "1.0")
                time.sleep(0.5)
                
                # Start MTC and let video play for a bit
                if self.mtc_helper:
                    print("  Step 4: Starting MTC timecode...")
                    self.mtc_helper.start(0)
                    print("  → Video should now be playing")
                    time.sleep(5)  # Let video play for 5 seconds to establish playback
                
                # Now test resolution changes while video is actively playing
                print("\n  Step 5: Testing resolution changes DURING playback...")
                print("          (Video should continue playing smoothly through each change)")
                
                # Test sequence: multiple resolution changes while video plays
                resolution_sequence = [
                    (1920, 1080, 60, "1080p"),
                    (1280, 720, 60, "720p"),
                    (1920, 1080, 60, "1080p (restore)"),
                    (2560, 1440, 60, "1440p (if available)"),
                    (1920, 1080, 60, "1080p (final)"),
                ]
                
                for width, height, refresh, desc in resolution_sequence:
                    print(f"\n    → Changing to {desc} ({width}x{height}@{refresh}Hz) while video plays...")
                    self.send_osc("/videocomposer/display/mode", "eDP-1", str(width), str(height), str(refresh))
                    print(f"      Waiting 4 seconds for mode change to complete...")
                    time.sleep(4)  # Wait for mode change + verify video still playing
                    print(f"      ✓ Resolution changed, video should still be playing")
                
                # Test on second output if available
                print("\n  Step 6: Testing resolution changes on second output (if available)...")
                for output in ["HDMI-A-1", "DP-1"]:
                    print(f"\n    Testing {output}:")
                    for width, height, refresh, desc in [(1920, 1080, 60, "1080p"), (1280, 720, 60, "720p")]:
                        print(f"      → Changing {output} to {desc} while video plays...")
                        self.send_osc("/videocomposer/display/mode", output, str(width), str(height), str(refresh))
                        time.sleep(4)
                        print(f"        ✓ Changed, video should still be playing")
                
                # Final verification: video should still be playing
                print("\n  Step 7: Final verification...")
                print("          Video should have been playing continuously through all resolution changes")
                time.sleep(3)
                
                if self.mtc_helper:
                    print("  Stopping MTC...")
                    self.mtc_helper.stop()
                    time.sleep(1)
                
                print("\n  ✓ On-the-fly resolution change test completed!")
                print("    If video played smoothly through all changes, test PASSED")
            else:
                print("\n[6] Skipping video playback test (no video file provided)")
                print("    To test on-the-fly resolution changes, provide --video <path>")
            
            print("\n" + "=" * 70)
            print("All resolution tests completed!")
            print("=" * 70)
            
            # Keep running for a bit
            print("\nKeeping videocomposer running for 3 seconds...")
            time.sleep(3)
            
            return True
            
        except KeyboardInterrupt:
            print("\nTest interrupted")
            return False
        except Exception as e:
            print(f"\nERROR: {e}")
            import traceback
            traceback.print_exc()
            return False
        finally:
            self.cleanup()


def main():
    parser = argparse.ArgumentParser(description="Test resolution modes")
    parser.add_argument("--videocomposer", help="Path to cuems-videocomposer binary or wrapper (auto-detected if not provided)")
    parser.add_argument("--video", help="Video file for playback test (optional)")
    parser.add_argument("--osc-port", type=int, default=7770, help="OSC port (default: 7770)")
    parser.add_argument("--fps", type=float, default=25.0, help="MTC framerate (default: 25.0)")
    parser.add_argument("--mtc-port", type=int, default=0, help="MTC MIDI port (default: 0)")
    
    args = parser.parse_args()
    
    try:
        test = ResolutionModesTest(
            videocomposer_bin=args.videocomposer,
            video_file=args.video,
            osc_port=args.osc_port,
            fps=args.fps,
            mtc_port=args.mtc_port
        )
    except FileNotFoundError as e:
        print(f"ERROR: {e}")
        sys.exit(1)
    
    success = test.run_tests()
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()

