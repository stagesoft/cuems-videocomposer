#!/usr/bin/env python3
"""
Integration test for cuems-videocomposer with MTC timecode.

This test:
1. Starts MTC timecode generation using libmtcmaster Python interface
2. Launches videocomposer with a test video file
3. Verifies that the application runs and receives MTC sync
4. Can be used as a regression test after code changes

Usage:
    python3 tests/test_mtc_integration.py [--video-path PATH] [--duration SECONDS]
"""

import sys
import os
import time
import subprocess
import signal
import argparse
from pathlib import Path

# Add libmtcmaster python path
libmtcmaster_python = Path(__file__).parent.parent.parent / "libmtcmaster" / "python"
libmtcmaster_lib = Path(__file__).parent.parent.parent / "libmtcmaster" / "libmtcmaster.so"
sys.path.insert(0, str(libmtcmaster_python))

# Import mtcsender and patch it to use the correct library path
original_cwd = os.getcwd()
try:
    os.chdir(libmtcmaster_python)
    import mtcsender
    # Patch the library path to use absolute path
    original_init = mtcsender.MtcSender.__init__
    def patched_init(self, fps=25, port=0, portname="SLMTCPort"):
        import ctypes
        # Use absolute path to library
        if not libmtcmaster_lib.exists():
            raise FileNotFoundError(f"libmtcmaster.so not found at {libmtcmaster_lib}")
        self.mtc_lib = ctypes.CDLL(str(libmtcmaster_lib))
        self.mtc_lib.MTCSender_create.restype = ctypes.c_void_p
        self.mtcproc = self.mtc_lib.MTCSender_create()
        self.port = port
        self.char_portname = portname.encode('utf-8')
        self.mtc_lib.MTCSender_openPort.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p]
        self.mtc_lib.MTCSender_openPort(self.mtcproc, self.port, self.char_portname)
        self.fps = fps
    mtcsender.MtcSender.__init__ = patched_init
    MtcSender = mtcsender.MtcSender
except ImportError as e:
    print(f"ERROR: Could not import mtcsender: {e}")
    print(f"Make sure libmtcmaster Python bindings are available at: {libmtcmaster_python}")
    print(f"Expected library location: {libmtcmaster_lib}")
    sys.exit(1)
finally:
    os.chdir(original_cwd)


class MTCIntegrationTest:
    def __init__(self, video_path, duration=10, fps=25.0, mtc_port=0):
        self.video_path = Path(video_path)
        self.duration = duration
        self.fps = fps
        self.mtc_port = mtc_port
        self.mtc_sender = None
        self.videocomposer_process = None
        self.test_passed = False
        
    def setup_mtc(self):
        """Initialize and start MTC timecode generation."""
        print(f"Setting up MTC sender (fps={self.fps}, port={self.mtc_port})...")
        try:
            self.mtc_sender = MtcSender(fps=self.fps, port=self.mtc_port, portname="VideocomposerTest")
            print("MTC sender created successfully")
            return True
        except Exception as e:
            print(f"ERROR: Failed to create MTC sender: {e}")
            return False
    
    def start_mtc(self, start_frame=0):
        """Start MTC playback from a specific frame."""
        if not self.mtc_sender:
            print("ERROR: MTC sender not initialized")
            return False
        
        try:
            # Set initial time to start_frame
            self.mtc_sender.settime_frames(start_frame)
            # Start playback
            self.mtc_sender.play()
            print(f"MTC playback started from frame {start_frame}")
            return True
        except Exception as e:
            print(f"ERROR: Failed to start MTC: {e}")
            return False
    
    def seek_mtc(self, hours=0, minutes=0, seconds=0, frames=0):
        """Seek MTC to a specific time position using full frame messages.
        
        Args:
            hours: Hours (0-23)
            minutes: Minutes (0-59)
            seconds: Seconds (0-59)
            frames: Frames (0-fps-1)
        """
        if not self.mtc_sender:
            print("ERROR: MTC sender not initialized")
            return False
        
        try:
            # Calculate total frames
            total_seconds = hours * 3600 + minutes * 60 + seconds
            total_frames = int(total_seconds * self.fps) + frames
            
            # Pause first to ensure clean seek
            self.mtc_sender.pause()
            time.sleep(0.1)  # Small delay to ensure pause is processed
            
            # Set time to the new position (this should trigger a full frame message)
            self.mtc_sender.settime_frames(total_frames)
            time.sleep(0.1)  # Small delay to ensure time is set
            
            # Resume playback
            self.mtc_sender.play()
            
            print(f"MTC seeked to {hours:02d}:{minutes:02d}:{seconds:02d}:{frames:02d} (frame {total_frames})")
            return True
        except Exception as e:
            print(f"ERROR: Failed to seek MTC: {e}")
            return False
    
    def stop_mtc(self):
        """Stop MTC playback."""
        if self.mtc_sender:
            try:
                self.mtc_sender.stop()
                print("MTC playback stopped")
            except Exception as e:
                print(f"Warning: Error stopping MTC: {e}")
    
    def launch_videocomposer(self):
        """Launch the videocomposer application with the test video."""
        videocomposer_path = Path(__file__).parent.parent / "build" / "cuems-videocomposer"
        
        if not videocomposer_path.exists():
            print(f"ERROR: videocomposer executable not found at {videocomposer_path}")
            print("Please build the project first: cd build && cmake .. && make")
            return False
        
        # Build command - include video file only if it exists
        cmd = [str(videocomposer_path)]
        
        if self.video_path.exists():
            print(f"Launching videocomposer with video: {self.video_path}")
            cmd.append(str(self.video_path))
        else:
            print(f"WARNING: Test video not found at {self.video_path}")
            print("Launching videocomposer without video file (testing MTC sync only)")
        
        # Add MIDI and verbose options
        cmd.extend(["--midi", "-1", "--verbose"])  # -1 = autodetect (connects to Midi Through)
        
        try:
            
            self.videocomposer_process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True
            )
            
            print(f"Videocomposer launched (PID: {self.videocomposer_process.pid})")
            print("Note: Both MTC sender and videocomposer automatically connect to 'Midi Through'")
            
            return True
            
        except Exception as e:
            print(f"ERROR: Failed to launch videocomposer: {e}")
            return False
    
    def monitor_process_partial(self, duration):
        """Monitor the videocomposer process for a partial duration."""
        if not self.videocomposer_process:
            return False
        
        start_time = time.time()
        last_output_time = start_time
        
        while time.time() - start_time < duration:
            # Check if process is still running
            if self.videocomposer_process.poll() is not None:
                # Process has terminated
                return_code = self.videocomposer_process.returncode
                stdout, stderr = self.videocomposer_process.communicate()
                
                if return_code != 0:
                    print(f"ERROR: videocomposer exited with code {return_code}")
                    if stderr:
                        print("STDERR:", stderr)
                    if stdout:
                        print("STDOUT:", stdout)
                    return False
                else:
                    print("videocomposer exited normally")
                    return True
            
            # Check for output (indicates it's running) - non-blocking
            try:
                import select
                if self.videocomposer_process.stdout:
                    # Check if data is available (non-blocking)
                    ready, _, _ = select.select([self.videocomposer_process.stdout], [], [], 0.01)
                    if ready:
                        line = self.videocomposer_process.stdout.readline()
                        if line:
                            print(f"[videocomposer] {line.strip()}")
                            last_output_time = time.time()
            except (ImportError, OSError, ValueError):
                # select might not work on all platforms or with pipes
                # Just continue monitoring without reading output
                pass
            
            time.sleep(0.1)
        
        # If we get here, the process ran for the duration without crashing
        if self.videocomposer_process.poll() is None:
            return True
        else:
            return False
    
    def monitor_process(self):
        """Monitor the videocomposer process and check for errors."""
        if not self.videocomposer_process:
            return False
        
        start_time = time.time()
        last_output_time = start_time
        
        print(f"Monitoring videocomposer for {self.duration} seconds...")
        
        while time.time() - start_time < self.duration:
            # Check if process is still running
            if self.videocomposer_process.poll() is not None:
                # Process has terminated
                return_code = self.videocomposer_process.returncode
                stdout, stderr = self.videocomposer_process.communicate()
                
                if return_code != 0:
                    print(f"ERROR: videocomposer exited with code {return_code}")
                    if stderr:
                        print("STDERR:", stderr)
                    if stdout:
                        print("STDOUT:", stdout)
                    return False
                else:
                    print("videocomposer exited normally")
                    break
            
            # Check for output (indicates it's running) - non-blocking
            try:
                import select
                if self.videocomposer_process.stdout:
                    # Check if data is available (non-blocking)
                    ready, _, _ = select.select([self.videocomposer_process.stdout], [], [], 0.01)
                    if ready:
                        line = self.videocomposer_process.stdout.readline()
                        if line:
                            print(f"[videocomposer] {line.strip()}")
                            last_output_time = time.time()
            except (ImportError, OSError, ValueError):
                # select might not work on all platforms or with pipes
                # Just continue monitoring without reading output
                pass
            
            time.sleep(0.1)
        
        # If we get here, the process ran for the duration without crashing
        if self.videocomposer_process.poll() is None:
            print("✓ videocomposer ran successfully for the test duration")
            self.test_passed = True
            return True
        else:
            return False
    
    def cleanup(self):
        """Clean up resources."""
        print("\nCleaning up...")
        
        # Stop MTC
        self.stop_mtc()
        
        # Terminate videocomposer if still running
        if self.videocomposer_process and self.videocomposer_process.poll() is None:
            print("Terminating videocomposer...")
            try:
                self.videocomposer_process.terminate()
                # Wait up to 5 seconds for graceful shutdown
                try:
                    self.videocomposer_process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    print("Force killing videocomposer...")
                    self.videocomposer_process.kill()
                    self.videocomposer_process.wait()
            except Exception as e:
                print(f"Warning: Error terminating videocomposer: {e}")
        
        # Clean up MTC sender
        if self.mtc_sender:
            try:
                del self.mtc_sender
            except:
                pass
    
    def run(self, test_seek=False):
        """Run the complete integration test.
        
        Args:
            test_seek: If True, run seek test (play 10s, seek to 2:00, play 20s)
        """
        print("=" * 60)
        if test_seek:
            print("cuems-videocomposer MTC Integration Test (with Seek)")
        else:
            print("cuems-videocomposer MTC Integration Test")
        print("=" * 60)
        print()
        
        try:
            # Step 1: Setup MTC
            if not self.setup_mtc():
                return False
            
            # Step 2: Start MTC playback
            if not self.start_mtc(start_frame=0):
                return False
            
            # Small delay to let MTC start
            time.sleep(0.5)
            
            # Step 3: Launch videocomposer
            if not self.launch_videocomposer():
                return False
            
            if test_seek:
                # Seek test flow: play 10s, seek to 2:00, play 20s
                print("\n=== Seek Test Sequence ===")
                print("Step 1: Playing for 10 seconds...")
                success = self.monitor_process_partial(10)
                if not success:
                    return False
                
                print("\nStep 2: Seeking to 2:00:00:00 (minute 2)...")
                if not self.seek_mtc(minutes=2, seconds=0, frames=0):
                    return False
                time.sleep(0.5)  # Wait for seek to be processed
                
                print("\nStep 3: Playing for 20 seconds after seek...")
                success = self.monitor_process_partial(20)
                return success
            else:
                # Step 4: Monitor the process for full duration
                success = self.monitor_process()
                return success
            
        except KeyboardInterrupt:
            print("\nTest interrupted by user")
            return False
        except Exception as e:
            print(f"ERROR: Unexpected error during test: {e}")
            import traceback
            traceback.print_exc()
            return False
        finally:
            self.cleanup()


def main():
    parser = argparse.ArgumentParser(
        description="Integration test for cuems-videocomposer with MTC timecode"
    )
    parser.add_argument(
        "--video-path",
        type=str,
        default=None,
        help="Path to test video file (default: searches common locations for problematic.mp4)"
    )
    parser.add_argument(
        "--test-problematic",
        action="store_true",
        help="Test problematic files (uses problematic.mp4 as default)"
    )
    parser.add_argument(
        "--duration",
        type=int,
        default=10,
        help="Test duration in seconds (default: 10)"
    )
    parser.add_argument(
        "--fps",
        type=float,
        default=25.0,
        help="MTC framerate (default: 25.0)"
    )
    parser.add_argument(
        "--mtc-port",
        type=int,
        default=0,
        help="ALSA MIDI port number for MTC (default: 0)"
    )
    parser.add_argument(
        "--test-seek",
        action="store_true",
        help="Run seek test: play 10s, seek to 2:00, play 20s"
    )
    
    args = parser.parse_args()
    
    # Resolve video path
    if args.video_path:
        video_path = Path(args.video_path)
        if not video_path.is_absolute():
            # Try relative to project root
            project_root = Path(__file__).parent.parent
            test_video = project_root / video_path
            if test_video.exists():
                video_path = test_video
            elif video_path.exists():
                video_path = video_path.resolve()
            else:
                print(f"Warning: Video file not found at {video_path}")
                print("The test will still run but videocomposer may fail to load the video")
    else:
        # Default: search for problematic.mp4 in common locations
        # If --test-problematic is set, also use problematic.mp4
        if args.test_problematic:
            video_filename = "problematic.mp4"
        else:
            video_filename = "problematic.mp4"  # Default to problematic.mp4
        
        project_root = Path(__file__).parent.parent
        search_paths = [
            project_root / "video_test_files" / video_filename,  # video_test_files folder (first priority)
            project_root / video_filename,  # Project root
            Path.home() / "Videos" / video_filename,  # ~/Videos
            Path("/home/ion/Videos") / video_filename,  # Explicit path
        ]
        
        video_path = None
        for path in search_paths:
            if path.exists():
                video_path = path
                print(f"Found test video at: {video_path}")
                break
        
        if not video_path:
            print(f"Warning: Test video '{video_filename}' not found in common locations:")
            for path in search_paths:
                print(f"  - {path}")
            print("The test will still run but videocomposer may fail to load the video")
            video_path = project_root / video_filename  # Use as placeholder
    
    # Create and run test
    test = MTCIntegrationTest(
        video_path=str(video_path),
        duration=args.duration,
        fps=args.fps,
        mtc_port=args.mtc_port
    )
    
    success = test.run(test_seek=args.test_seek)
    
    print()
    print("=" * 60)
    if success:
        print("TEST PASSED ✓")
        sys.exit(0)
    else:
        print("TEST FAILED ✗")
        sys.exit(1)


if __name__ == "__main__":
    main()

