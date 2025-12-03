#!/usr/bin/env python3
"""
Test script for Virtual Canvas features.

Tests:
1. Display listing and resolution modes
2. Resolution changes (live and policy-based)
3. Output region configuration
4. Edge blending
5. Geometric warping
6. Canvas layout management
7. Configuration save/load

Requires MTC timecode to play videos.
"""

import subprocess
import time
import sys
import os
from pathlib import Path
import argparse
import threading

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


class VirtualCanvasFeaturesTest:
    def __init__(self, videocomposer_bin=None, video_file=None, osc_port=7770, fps=25.0, mtc_port=0):
        self.videocomposer_bin = self._find_videocomposer(videocomposer_bin)
        self.video_file = video_file
        self.osc_port = osc_port
        self.fps = fps
        self.mtc_port = mtc_port
        self.videocomposer_process = None
        self.osc_client = None
        self.mtc_helper = None
        self.stop_monitoring = threading.Event()
        
        if MTC_AVAILABLE:
            self.mtc_helper = MTCHelper(fps=fps, port=mtc_port, portname="VirtualCanvasTest")
    
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
        
        print(f"Starting MTC playback from frame {start_frame}...")
        if self.mtc_helper.start(start_frame):
            print("MTC playback started")
            return True
        else:
            print("ERROR: Failed to start MTC playback")
            return False
    
    def stop_mtc(self):
        """Stop MTC playback."""
        if self.mtc_helper:
            self.mtc_helper.stop()
    
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
        
        # Wait a bit for startup
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
    
    def send_osc(self, path, *args, verbose=True):
        """Send OSC message."""
        if not self.osc_client:
            if verbose:
                print("ERROR: OSC client not connected")
            return False
        
        try:
            self.osc_client.send_message(path, list(args))
            if verbose:
                print(f"Sent: {path} {list(args)}")
            return True
        except Exception as e:
            if verbose:
                print(f"ERROR: Failed to send OSC message: {e}")
            return False
    
    def cleanup(self):
        """Cleanup resources."""
        self.stop_mtc()
        if self.videocomposer_process:
            self.videocomposer_process.terminate()
            try:
                self.videocomposer_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.videocomposer_process.kill()
        if self.mtc_helper:
            self.mtc_helper.cleanup()
    
    # ============================================================================
    # Test Functions
    # ============================================================================
    
    def test_display_list(self):
        """Test /videocomposer/display/list"""
        print("\n=== Test: Display List ===")
        return self.send_osc("/videocomposer/display/list")
    
    def test_display_modes(self, output_name):
        """Test /videocomposer/display/modes <outputName>"""
        print(f"\n=== Test: Display Modes for {output_name} ===")
        return self.send_osc("/videocomposer/display/modes", output_name)
    
    def test_resolution_mode(self, mode):
        """Test /videocomposer/display/resolution_mode <mode>"""
        print(f"\n=== Test: Set Resolution Mode to {mode} ===")
        return self.send_osc("/videocomposer/display/resolution_mode", mode)
    
    def test_resolution_mode_status(self):
        """Test /videocomposer/display/resolution_mode (no args = show options)"""
        print("\n=== Test: Resolution Mode Status ===")
        return self.send_osc("/videocomposer/display/resolution_mode")
    
    def test_display_mode_change(self, output_name, width, height, refresh=0):
        """Test /videocomposer/display/mode <name> <width> <height> [refresh]"""
        print(f"\n=== Test: Change {output_name} to {width}x{height}@{refresh}Hz ===")
        args = [output_name, str(width), str(height)]
        if refresh > 0:
            args.append(str(refresh))
        return self.send_osc("/videocomposer/display/mode", *args)
    
    def test_display_region(self, output_name, canvas_x, canvas_y, width=0, height=0):
        """Test /videocomposer/display/region <name> <x> <y> [width] [height]"""
        print(f"\n=== Test: Set {output_name} region to ({canvas_x}, {canvas_y}) size {width}x{height} ===")
        args = [output_name, str(canvas_x), str(canvas_y)]
        if width > 0:
            args.append(str(width))
        if height > 0:
            args.append(str(height))
        return self.send_osc("/videocomposer/display/region", *args)
    
    def test_display_blend(self, output_name, left, right, top, bottom, gamma=2.2):
        """Test /videocomposer/display/blend <name> <left> <right> <top> <bottom> [gamma]"""
        print(f"\n=== Test: Set blend for {output_name}: L={left} R={right} T={top} B={bottom} gamma={gamma} ===")
        return self.send_osc("/videocomposer/display/blend", 
                            output_name, str(left), str(right), str(top), str(bottom), str(gamma))
    
    def test_display_warp(self, output_name, mesh_path):
        """Test /videocomposer/display/warp <name> <meshPath>"""
        print(f"\n=== Test: Set warp mesh for {output_name}: {mesh_path} ===")
        return self.send_osc("/videocomposer/display/warp", output_name, mesh_path)
    
    def test_config_save(self, path=""):
        """Test /videocomposer/display/save [path]"""
        print(f"\n=== Test: Save Configuration ===")
        if path:
            return self.send_osc("/videocomposer/display/save", path)
        else:
            return self.send_osc("/videocomposer/display/save")
    
    def test_config_load(self, path=""):
        """Test /videocomposer/display/load [path]"""
        print(f"\n=== Test: Load Configuration ===")
        if path:
            return self.send_osc("/videocomposer/display/load", path)
        else:
            return self.send_osc("/videocomposer/display/load")
    
    def test_output_list(self):
        """Test /videocomposer/output/list"""
        print("\n=== Test: Output List ===")
        return self.send_osc("/videocomposer/output/list")
    
    def test_output_capture(self, action, width=0, height=0):
        """Test /videocomposer/output/capture <enable|disable|status> [width] [height]"""
        print(f"\n=== Test: Output Capture {action} ===")
        args = [action]
        if width > 0 and height > 0:
            args.extend([str(width), str(height)])
        return self.send_osc("/videocomposer/output/capture", *args)
    
    def run_all_tests(self):
        """Run all Virtual Canvas feature tests."""
        print("=" * 70)
        print("Virtual Canvas Features Test Suite")
        print("=" * 70)
        
        # Start videocomposer
        if not self.start_videocomposer():
            return False
        
        # Connect OSC
        if not self.connect_osc():
            self.cleanup()
            return False
        
        # Setup MTC
        if not self.setup_mtc():
            print("WARNING: MTC setup failed, but continuing with tests")
        
        # Wait for initialization
        time.sleep(1)
        
        try:
            # ====================================================================
            # Phase 1: Display Discovery and Information
            # ====================================================================
            print("\n" + "=" * 70)
            print("PHASE 1: Display Discovery")
            print("=" * 70)
            
            self.test_display_list()
            time.sleep(0.5)
            
            self.test_output_list()
            time.sleep(0.5)
            
            # Get available modes for first output (assuming eDP-1 or HDMI-A-1)
            # We'll try common names
            for output_name in ["eDP-1", "HDMI-A-1", "DP-1"]:
                self.test_display_modes(output_name)
                time.sleep(0.5)
            
            # ====================================================================
            # Phase 2: Resolution Mode Testing
            # ====================================================================
            print("\n" + "=" * 70)
            print("PHASE 2: Resolution Mode Testing")
            print("=" * 70)
            
            self.test_resolution_mode_status()
            time.sleep(0.5)
            
            # Test different resolution modes
            for mode in ["1080p", "native", "maximum", "720p", "4k"]:
                self.test_resolution_mode(mode)
                time.sleep(1)
            
            # Set back to 1080p for consistency
            self.test_resolution_mode("1080p")
            time.sleep(1)
            
            # ====================================================================
            # Phase 3: Live Resolution Changes
            # ====================================================================
            print("\n" + "=" * 70)
            print("PHASE 3: Live Resolution Changes")
            print("=" * 70)
            
            # Try changing resolution on first output
            for output_name in ["eDP-1", "HDMI-A-1"]:
                # Try different resolutions
                resolutions = [
                    (1920, 1080, 60),
                    (1280, 720, 60),
                    (2560, 1440, 60),
                ]
                
                for width, height, refresh in resolutions:
                    self.test_display_mode_change(output_name, width, height, refresh)
                    time.sleep(2)  # Wait for mode change to complete
                
                # Restore to 1080p
                self.test_display_mode_change(output_name, 1920, 1080, 60)
                time.sleep(2)
            
            # ====================================================================
            # Phase 4: Output Region Configuration
            # ====================================================================
            print("\n" + "=" * 70)
            print("PHASE 4: Output Region Configuration")
            print("=" * 70)
            
            # Configure regions for multi-display setup
            # Display 1 at (0, 0)
            self.test_display_region("eDP-1", 0, 0, 1920, 1080)
            time.sleep(0.5)
            
            # Display 2 at (1920, 0) - side by side
            self.test_display_region("HDMI-A-1", 1920, 0, 1920, 1080)
            time.sleep(0.5)
            
            # Try stacked layout
            self.test_display_region("HDMI-A-1", 0, 1080, 1920, 1080)
            time.sleep(0.5)
            
            # Restore side-by-side
            self.test_display_region("HDMI-A-1", 1920, 0, 1920, 1080)
            time.sleep(0.5)
            
            # ====================================================================
            # Phase 5: Edge Blending
            # ====================================================================
            print("\n" + "=" * 70)
            print("PHASE 5: Edge Blending")
            print("=" * 70)
            
            # Test blend on left edge (for overlapping projectors)
            self.test_display_blend("eDP-1", 100, 0, 0, 0, 2.2)
            time.sleep(0.5)
            
            # Test blend on right edge
            self.test_display_blend("HDMI-A-1", 0, 100, 0, 0, 2.2)
            time.sleep(0.5)
            
            # Test blend on all edges
            self.test_display_blend("eDP-1", 50, 50, 50, 50, 2.2)
            time.sleep(0.5)
            
            # Disable blending
            self.test_display_blend("eDP-1", 0, 0, 0, 0, 2.2)
            self.test_display_blend("HDMI-A-1", 0, 0, 0, 0, 2.2)
            time.sleep(0.5)
            
            # ====================================================================
            # Phase 6: Geometric Warping
            # ====================================================================
            print("\n" + "=" * 70)
            print("PHASE 6: Geometric Warping")
            print("=" * 70)
            
            # Note: Warp mesh files would need to exist
            # This is a placeholder test
            print("NOTE: Warp mesh testing requires actual mesh files")
            # self.test_display_warp("eDP-1", "/path/to/warp_mesh.json")
            # time.sleep(0.5)
            
            # ====================================================================
            # Phase 7: Configuration Persistence
            # ====================================================================
            print("\n" + "=" * 70)
            print("PHASE 7: Configuration Persistence")
            print("=" * 70)
            
            # Save configuration
            config_path = "/tmp/videocomposer_test_config.conf"
            self.test_config_save(config_path)
            time.sleep(0.5)
            
            # Load configuration
            self.test_config_load(config_path)
            time.sleep(0.5)
            
            # ====================================================================
            # Phase 8: Virtual Output Capture
            # ====================================================================
            print("\n" + "=" * 70)
            print("PHASE 8: Virtual Output Capture")
            print("=" * 70)
            
            # Check capture status
            self.test_output_capture("status")
            time.sleep(0.5)
            
            # Enable capture
            self.test_output_capture("enable", 1920, 1080)
            time.sleep(0.5)
            
            # Check status again
            self.test_output_capture("status")
            time.sleep(0.5)
            
            # Disable capture
            self.test_output_capture("disable")
            time.sleep(0.5)
            
            # ====================================================================
            # Phase 9: Video Playback with MTC
            # ====================================================================
            if self.video_file and Path(self.video_file).exists():
                print("\n" + "=" * 70)
                print("PHASE 9: Video Playback with MTC")
                print("=" * 70)
                
                # Load video file
                print(f"\nLoading video: {self.video_file}")
                self.send_osc("/videocomposer/layer/load", self.video_file, "test_video")
                time.sleep(1)
                
                # Enable mtcfollow
                print("\nEnabling mtcfollow")
                self.send_osc("/videocomposer/layer/test_video/mtcfollow", "1")
                time.sleep(0.5)
                
                # Start MTC playback
                print("\nStarting MTC playback...")
                if self.start_mtc(0):
                    print("MTC started - video should be playing now")
                    time.sleep(5)  # Let video play for 5 seconds
                    
                    # Test layer positioning on virtual canvas
                    print("\n9a. Positioning layer on virtual canvas...")
                    self.send_osc("/videocomposer/layer/test_video/position", 960, 540)  # Center of display 1
                    time.sleep(2)
                    
                    self.send_osc("/videocomposer/layer/test_video/position", 2880, 540)  # Center of display 2
                    time.sleep(2)
                    
                    self.send_osc("/videocomposer/layer/test_video/position", 1920, 540)  # Spanning both displays
                    time.sleep(2)
                    
                    # Test 9b: On-the-fly resolution changes during playback
                    print("\n9b. CRITICAL TEST: On-the-fly resolution changes during video playback")
                    print("    Video should continue playing smoothly through each resolution change")
                    
                    # Sequence of resolution changes while video plays
                    resolution_changes = [
                        (1920, 1080, 60, "1080p (baseline)"),
                        (1280, 720, 60, "720p"),
                        (1920, 1080, 60, "1080p (restore)"),
                        (2560, 1440, 60, "1440p (if available)"),
                        (1920, 1080, 60, "1080p (final)"),
                    ]
                    
                    for width, height, refresh, desc in resolution_changes:
                        print(f"\n      → Changing to {desc} ({width}x{height}@{refresh}Hz) while video plays...")
                        self.test_display_mode_change("eDP-1", width, height, refresh)
                        print(f"        Waiting 4 seconds for mode change to complete...")
                        time.sleep(4)  # Wait for mode change + verify video still playing
                        print(f"        ✓ Resolution changed - video should still be playing smoothly")
                    
                    # Test on second output if available
                    print("\n      Testing resolution changes on second output while video plays...")
                    for output in ["HDMI-A-1", "DP-1"]:
                        print(f"\n        Testing {output}:")
                        for width, height, refresh, desc in [(1920, 1080, 60, "1080p"), (1280, 720, 60, "720p")]:
                            print(f"          → Changing {output} to {desc} while video plays...")
                            self.test_display_mode_change(output, width, height, refresh)
                            time.sleep(4)
                            print(f"            ✓ Changed - video should still be playing")
                    
                    print("\n    ✓ On-the-fly resolution change test completed")
                    print("      If video played continuously through all changes, test PASSED")
                    time.sleep(2)
                    
                    self.stop_mtc()
                else:
                    print("WARNING: MTC start failed, video won't play")
            else:
                print("\nSkipping video playback test (no video file provided)")
            
            print("\n" + "=" * 70)
            print("All tests completed!")
            print("=" * 70)
            
            return True
            
        except KeyboardInterrupt:
            print("\nTest interrupted by user")
            return False
        except Exception as e:
            print(f"\nERROR during testing: {e}")
            import traceback
            traceback.print_exc()
            return False
        finally:
            # Keep running for a bit to see results
            print("\nKeeping videocomposer running for 5 seconds...")
            time.sleep(5)
            self.cleanup()


def main():
    parser = argparse.ArgumentParser(description="Test Virtual Canvas features")
    parser.add_argument("--videocomposer", help="Path to cuems-videocomposer binary or wrapper (auto-detected if not provided)")
    parser.add_argument("--video", help="Video file to test playback (optional)")
    parser.add_argument("--osc-port", type=int, default=7770, help="OSC port (default: 7770)")
    parser.add_argument("--fps", type=float, default=25.0, help="MTC framerate (default: 25.0)")
    parser.add_argument("--mtc-port", type=int, default=0, help="MTC MIDI port (default: 0)")
    
    args = parser.parse_args()
    
    try:
        test = VirtualCanvasFeaturesTest(
            videocomposer_bin=args.videocomposer,
            video_file=args.video,
            osc_port=args.osc_port,
            fps=args.fps,
            mtc_port=args.mtc_port
        )
    except FileNotFoundError as e:
        print(f"ERROR: {e}")
        sys.exit(1)
    
    success = test.run_all_tests()
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()

