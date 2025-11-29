#!/usr/bin/env python3
"""
Test script for dynamic video file management system.

Tests the new OSC commands for loading/unloading files, looping, and per-layer controls.
1. Starts MTC timecode
2. Loads two video files
3. Sets mtcfollow on both layers
4. Modifies both layers via OSC image controls
"""

import subprocess
import time
import sys
import os
import math
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


class DynamicFileManagementTest:
    def __init__(self, videocomposer_bin, video_file1=None, video_file2=None, osc_port=7770, fps=25.0, mtc_port=0, enable_loop=False):
        self.videocomposer_bin = Path(videocomposer_bin)
        # Store as strings to support NDI sources (not file paths)
        self.video_file1 = video_file1
        self.video_file2 = video_file2
        self.osc_port = osc_port
        self.fps = fps
        self.mtc_port = mtc_port
        self.enable_loop = enable_loop
        self.videocomposer_process = None
        self.osc_client = None
        self.mtc_helper = None
        self.monitoring_thread = None
        self.stop_monitoring = threading.Event()
        if MTC_AVAILABLE:
            self.mtc_helper = MTCHelper(fps=fps, port=mtc_port, portname="DynamicFileTest")
        
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
            
            # Start monitoring thread
            self.stop_monitoring.clear()
            self.monitoring_thread = threading.Thread(target=self._monitor_system, daemon=True)
            self.monitoring_thread.start()
            print("Started system monitoring thread")
            
            return True
        else:
            print("ERROR: Failed to start MTC")
            return False
    
    def _monitor_system(self):
        """Monitor system in background thread - prints even if main thread hangs."""
        start_time = time.time()
        last_print = start_time
        
        while not self.stop_monitoring.is_set():
            current_time = time.time()
            if current_time - last_print >= 2.0:
                elapsed = current_time - start_time
                expected_frame = int(elapsed * self.fps)
                print(f"[MONITOR] Time: {elapsed:.1f}s, Expected MTC frame: ~{expected_frame}, Thread is alive!")
                last_print = current_time
            time.sleep(0.5)
    
    def stop_mtc(self):
        """Stop MTC playback."""
        # Stop monitoring thread
        if self.monitoring_thread:
            print("Stopping monitoring thread...")
            self.stop_monitoring.set()
            self.monitoring_thread.join(timeout=2)
            print("Monitoring thread stopped")
        
        if self.mtc_helper:
            self.mtc_helper.stop()
            print("MTC playback stopped")
    
    def start_videocomposer(self):
        """Start the videocomposer application."""
        if not self.videocomposer_bin.exists():
            print(f"ERROR: videocomposer not found at {self.videocomposer_bin}")
            return False
        
        cmd = [str(self.videocomposer_bin)]
        
        # Don't add video file - we'll load via OSC
        # Add OSC port
        cmd.extend(["--osc", str(self.osc_port)])
        
        # Add MIDI port for MTC (use -1 for autodetect)
        cmd.extend(["--midi", "-1"])
        
        # Add verbose for debugging
        cmd.append("--verbose")
        
        try:
            env = os.environ.copy()
            if 'DISPLAY' not in env:
                env['DISPLAY'] = ':0'
            
            # Don't capture stdout/stderr to avoid pipe buffer blocking
            # When verbose output fills the pipe buffer, videocomposer can hang
            self.videocomposer_process = subprocess.Popen(
                cmd,
                stdout=None,  # Let output go to terminal
                stderr=None,  # Let errors go to terminal
                text=True,
                bufsize=1,
                env=env
            )
            
            print(f"Started videocomposer (PID: {self.videocomposer_process.pid})")
            print(f"OSC port: {self.osc_port}")
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
            print(f"ERROR: Failed to connect to OSC: {e}")
            return False
    
    def send_osc(self, path, *args, verbose=False):
        """Send OSC message."""
        if not self.osc_client:
            if verbose:
                print("ERROR: OSC client not connected")
            return False
        
        try:
            self.osc_client.send_message(path, args)
            if verbose:
                print(f"Sent: {path} {args}")
            return True
        except Exception as e:
            if verbose:
                print(f"ERROR: Failed to send OSC message: {e}")
            return False
    
    def is_ndi_source(self, source):
        """Check if source is an NDI source or other live source (not a file path)."""
        if not source:
            return False
        source_str = str(source)
        # Check for known live source prefixes
        if (source_str.startswith("ndi://") or 
            source_str.startswith("rtsp://") or 
            source_str.startswith("http://") or 
            source_str.startswith("https://") or
            source_str.startswith("udp://") or
            source_str.startswith("tcp://") or
            source_str.startswith("/dev/video")):
            return True
        # If it's a file that exists, it's definitely a file (not a live source)
        if Path(source_str).exists() and Path(source_str).is_file():
            return False
        # If it doesn't exist as a file, it might be:
        # - An NDI source name (without ndi:// prefix)
        # - A network stream URL
        # - A non-existent file (will be caught by videocomposer)
        # In any case, skip the file existence check and let videocomposer handle it
        return True
    
    def test_layer_load(self, cue_id, video_file):
        """Test /videocomposer/layer/load s s"""
        print(f"\n=== Test: Load layer with file ===")
        print(f"Cue ID: {cue_id}")
        print(f"File: {video_file}")
        
        # Skip file existence check for NDI sources and other live sources
        if not self.is_ndi_source(video_file):
            if not Path(video_file).exists():
                print(f"WARNING: Video file not found: {video_file}")
                return False
        
        return self.send_osc("/videocomposer/layer/load", video_file, cue_id)
    
    def test_layer_file(self, cue_id, video_file):
        """Test /videocomposer/layer/<cueId>/file s"""
        print(f"\n=== Test: Load file into existing layer ===")
        print(f"Cue ID: {cue_id}")
        print(f"File: {video_file}")
        
        # Skip file existence check for NDI sources and other live sources
        if not self.is_ndi_source(video_file):
            if not Path(video_file).exists():
                print(f"WARNING: Video file not found: {video_file}")
                return False
        
        return self.send_osc(f"/videocomposer/layer/{cue_id}/file", video_file)
    
    def test_layer_unload(self, cue_id):
        """Test /videocomposer/layer/unload s"""
        print(f"\n=== Test: Unload layer ===")
        print(f"Cue ID: {cue_id}")
        
        return self.send_osc("/videocomposer/layer/unload", cue_id)
    
    def test_layer_controls(self, cue_id, layer_num=1):
        """Test per-layer image controls - initial setup."""
        print(f"\n=== Test: Initial per-layer image controls for layer {layer_num} ===")
        print(f"Cue ID: {cue_id}")
        
        # Different controls for each layer to see the difference
        if layer_num == 1:
            # Layer 1: Top-left, smaller, rotated
            tests = [
                ("/videocomposer/layer/{}/position", 100, 100),
                ("/videocomposer/layer/{}/opacity", 0.9),
                ("/videocomposer/layer/{}/scale", 0.8, 0.8),
                ("/videocomposer/layer/{}/rotation", 15.0),
                ("/videocomposer/layer/{}/visible", 1),
                ("/videocomposer/layer/{}/zorder", 1),
                ("/videocomposer/layer/{}/blendmode", 0),  # NORMAL
            ]
        else:
            # Layer 2: Bottom-right, larger, different rotation
            tests = [
                ("/videocomposer/layer/{}/position", 400, 300),
                ("/videocomposer/layer/{}/opacity", 0.7),
                ("/videocomposer/layer/{}/scale", 1.2, 1.2),
                ("/videocomposer/layer/{}/rotation", -10.0),
                ("/videocomposer/layer/{}/visible", 1),
                ("/videocomposer/layer/{}/zorder", 2),
                ("/videocomposer/layer/{}/blendmode", 2),  # SCREEN
            ]
        
        for path_template, *args in tests:
            path = path_template.format(cue_id)
            self.send_osc(path, *args, verbose=True)
            time.sleep(0.1)
        
        return True
    
    def progressive_image_adjustments(self, cue_id_1, cue_id_2, duration=30):
        """Progressively adjust image controls over time - continuous and smooth."""
        print(f"\n=== Progressive Image Adjustments ({duration} seconds) ===")
        print("Making continuous smooth adjustments to position, scale, rotation, and opacity...")
        print(f"Update rate: ~60 updates/second for ultra-smooth animation")
        
        start_time = time.time()
        interval = 0.016  # ~60 FPS (16.67ms) for very smooth animation
        last_log_time = start_time
        log_interval = 2.0  # Log progress every 2 seconds
        
        # Track previous values to only send when changed (reduces OSC traffic)
        # But use very small thresholds for smoothness
        prev_values = {
            'x1': None, 'y1': None, 'opacity1': None, 'scale1': None, 'rotation1': None,
            'x2': None, 'y2': None, 'opacity2': None, 'scale2': None, 'rotation2': None
        }
        
        # Don't alternate layers - update both every frame for maximum smoothness
        frame_counter = 0
        
        while time.time() - start_time < duration:
            elapsed = time.time() - start_time
            progress = elapsed / duration
            
            # Use sine/cosine for smooth oscillations with different frequencies
            # Multiple frequencies create more interesting patterns
            sin_val = math.sin(progress * 2 * math.pi)
            cos_val = math.cos(progress * 2 * math.pi)
            sin_val2 = math.sin(progress * 3 * math.pi)  # Different frequency for variety
            cos_val2 = math.cos(progress * 1.5 * math.pi)
            sin_val3 = math.sin(progress * 1.2 * math.pi)  # Additional frequency for opacity
            cos_val3 = math.cos(progress * 2.3 * math.pi)
            
            # Update both layers every frame for maximum smoothness
            # Use very small thresholds to allow smooth interpolation
            
            # Layer 1 adjustments
            # Position: move in a smooth circular pattern (use float for precision)
            radius = 100
            x1 = 200 + radius * cos_val
            y1 = 150 + radius * sin_val
            # Send position every frame for maximum smoothness (OSC uses integers, but frequent updates minimize jumps)
            # Only skip if position hasn't changed at all (same integer value)
            x1_int = int(round(x1))
            y1_int = int(round(y1))
            if prev_values['x1'] is None or x1_int != int(round(prev_values['x1'])) or y1_int != int(round(prev_values['y1'])):
                self.send_osc(f"/videocomposer/layer/{cue_id_1}/position", x1_int, y1_int, verbose=False)
                prev_values['x1'] = x1
                prev_values['y1'] = y1
            
            # Opacity: smooth pulse between 0.4 and 1.0 using sin (very smooth transitions)
            opacity1 = 0.4 + 0.6 * (0.5 + 0.5 * sin_val3)
            # Send every frame for maximum smoothness (very small threshold)
            if prev_values['opacity1'] is None or abs(opacity1 - prev_values['opacity1']) >= 0.0001:
                self.send_osc(f"/videocomposer/layer/{cue_id_1}/opacity", opacity1, verbose=False)
                prev_values['opacity1'] = opacity1
            
            # Scale: smooth oscillation between 0.5 and 1.2 using sin2
            scale1 = 0.5 + 0.7 * (0.5 + 0.5 * sin_val2)
            # Send every frame for maximum smoothness (very small threshold)
            if prev_values['scale1'] is None or abs(scale1 - prev_values['scale1']) >= 0.0001:
                self.send_osc(f"/videocomposer/layer/{cue_id_1}/scale", scale1, scale1, verbose=False)
                prev_values['scale1'] = scale1
            
            # Rotation: continuous smooth rotation (full 360 degrees over duration)
            rotation1 = 15.0 + progress * 360.0
            # Send every frame for maximum smoothness (very small threshold)
            if prev_values['rotation1'] is None or abs(rotation1 - prev_values['rotation1']) >= 0.01:
                self.send_osc(f"/videocomposer/layer/{cue_id_1}/rotation", rotation1, verbose=False)
                prev_values['rotation1'] = rotation1
            
            # Layer 2 adjustments
            # Position: move in opposite circular pattern with different phase
            radius2 = 150
            x2 = 400 + radius2 * -cos_val2
            y2 = 300 + radius2 * -sin_val2
            # Send position every frame for maximum smoothness
            x2_int = int(round(x2))
            y2_int = int(round(y2))
            if prev_values['x2'] is None or x2_int != int(round(prev_values['x2'])) or y2_int != int(round(prev_values['y2'])):
                self.send_osc(f"/videocomposer/layer/{cue_id_2}/position", x2_int, y2_int, verbose=False)
                prev_values['x2'] = x2
                prev_values['y2'] = y2
            
            # Opacity: smooth opposite pulse (between 0.6 and 1.0)
            opacity2 = 0.6 + 0.4 * (0.5 + 0.5 * -sin_val3)
            # Send every frame for maximum smoothness (very small threshold)
            if prev_values['opacity2'] is None or abs(opacity2 - prev_values['opacity2']) >= 0.0001:
                self.send_osc(f"/videocomposer/layer/{cue_id_2}/opacity", opacity2, verbose=False)
                prev_values['opacity2'] = opacity2
            
            # Scale: smooth opposite oscillation (between 0.8 and 1.5)
            scale2 = 0.8 + 0.7 * (0.5 + 0.5 * -sin_val2)
            # Send every frame for maximum smoothness (very small threshold)
            if prev_values['scale2'] is None or abs(scale2 - prev_values['scale2']) >= 0.0001:
                self.send_osc(f"/videocomposer/layer/{cue_id_2}/scale", scale2, scale2, verbose=False)
                prev_values['scale2'] = scale2
            
            # Rotation: smooth opposite rotation
            rotation2 = -10.0 - progress * 360.0
            # Send every frame for maximum smoothness (very small threshold)
            if prev_values['rotation2'] is None or abs(rotation2 - prev_values['rotation2']) >= 0.01:
                self.send_osc(f"/videocomposer/layer/{cue_id_2}/rotation", rotation2, verbose=False)
                prev_values['rotation2'] = rotation2
            
            frame_counter += 1
            
            # Blend mode: skip for now - changing it frequently may cause rendering issues
            # blend_mode = int(progress * 4) % 4
            # if blend_mode != prev_values['blend1']:
            #     self.send_osc(f"/videocomposer/layer/{cue_id_1}/blendmode", blend_mode, verbose=False)
            #     prev_values['blend1'] = blend_mode
            # blend_mode2 = (blend_mode + 2) % 4
            # if blend_mode2 != prev_values['blend2']:
            #     self.send_osc(f"/videocomposer/layer/{cue_id_2}/blendmode", blend_mode2, verbose=False)
            #     prev_values['blend2'] = blend_mode2
            
            # Log progress every 2 seconds (not every frame to avoid spam)
            current_time = time.time()
            if current_time - last_log_time >= log_interval:
                updates_per_sec = frame_counter / elapsed if elapsed > 0 else 0
                print(f"  {elapsed:.1f}s / {duration}s - Progress: {progress*100:.1f}% - Updates: {updates_per_sec:.1f}/s")
                last_log_time = current_time
            
            # Use precise sleep for smooth timing
            elapsed_loop = time.time() - start_time
            next_frame_time = frame_counter * interval
            sleep_time = next_frame_time - elapsed_loop
            if sleep_time > 0:
                time.sleep(sleep_time)
            # If we're behind schedule, don't sleep (catch up)
        
        print("Progressive adjustments completed!")
        return True
    
    def test_looping(self, cue_id):
        """Test looping controls."""
        print(f"\n=== Test: Looping controls ===")
        print(f"Cue ID: {cue_id}")
        
        # Test full file loop
        self.send_osc(f"/videocomposer/layer/{cue_id}/loop", 1, -1)  # Enable, infinite
        time.sleep(0.1)
        
        # Test loop with count
        self.send_osc(f"/videocomposer/layer/{cue_id}/loop", 1, 3)  # Enable, 3 times
        time.sleep(0.1)
        
        # Test region loop
        self.send_osc(f"/videocomposer/layer/{cue_id}/loop/region", 100, 500, -1)  # Frames 100-500, infinite
        time.sleep(0.1)
        
        # Disable region loop
        self.send_osc(f"/videocomposer/layer/{cue_id}/loop/region/disable")
        time.sleep(0.1)
        
        return True
    
    def test_auto_unload(self, cue_id):
        """Test auto-unload."""
        print(f"\n=== Test: Auto-unload ===")
        print(f"Cue ID: {cue_id}")
        
        # Enable auto-unload
        self.send_osc(f"/videocomposer/layer/{cue_id}/autounload", 1)
        time.sleep(0.1)
        
        # Disable auto-unload
        self.send_osc(f"/videocomposer/layer/{cue_id}/autounload", 0)
        time.sleep(0.1)
        
        return True
    
    def test_offset_mtcfollow(self, cue_id):
        """Test offset and MTC follow."""
        print(f"\n=== Test: Offset and MTC follow ===")
        print(f"Cue ID: {cue_id}")
        
        # Test frame offset
        self.send_osc(f"/videocomposer/layer/{cue_id}/offset", 100)
        time.sleep(0.1)
        
        # Test SMPTE offset
        self.send_osc(f"/videocomposer/layer/{cue_id}/offset", "00:00:05:00")
        time.sleep(0.1)
        
        # Test MTC follow
        self.send_osc(f"/videocomposer/layer/{cue_id}/mtcfollow", 1)
        time.sleep(0.1)
        
        self.send_osc(f"/videocomposer/layer/{cue_id}/mtcfollow", 0)
        time.sleep(0.1)
        
        return True
    
    def run_tests(self):
        """Run all tests."""
        print("=" * 60)
        print("Dynamic File Management Test Suite")
        print("=" * 60)
        
        # Step 1: Setup MTC
        print("\n--- Step 1: Setup MTC Timecode ---")
        if MTC_AVAILABLE:
            if not self.setup_mtc():
                print("WARNING: MTC setup failed, continuing without MTC")
            else:
                if not self.start_mtc(start_frame=0):
                    print("WARNING: MTC start failed, continuing without MTC")
        else:
            print("WARNING: MTC not available, continuing without MTC")
        
        time.sleep(1)
        
        # Step 2: Start videocomposer
        print("\n--- Step 2: Start Videocomposer ---")
        if not self.start_videocomposer():
            self.stop_mtc()
            return False
        
        if not self.connect_osc():
            self.stop()
            return False
        
        time.sleep(2)  # Wait for videocomposer to initialize and connect to MTC
        
        # Test cue IDs
        cue_id_1 = "550e8400-e29b-41d4-a716-446655440000"
        cue_id_2 = "660e8400-e29b-41d4-a716-446655440001"
        
        # Step 3: Load two files
        print("\n--- Step 3: Load Two Video Files ---")
        if not self.video_file1:
            print(f"ERROR: First video file/source not provided")
            self.stop()
            return False
        
        # Check file existence only for file paths (not NDI sources)
        if not self.is_ndi_source(self.video_file1):
            if not Path(self.video_file1).exists():
                print(f"ERROR: First video file not found: {self.video_file1}")
                self.stop()
                return False
        
        if not self.video_file2:
            print(f"WARNING: Second video file/source not provided")
            print("Using first video file/source for both layers")
            self.video_file2 = self.video_file1
        elif not self.is_ndi_source(self.video_file2):
            if not Path(self.video_file2).exists():
                print(f"WARNING: Second video file not found: {self.video_file2}")
                print("Using first video file/source for both layers")
                self.video_file2 = self.video_file1
        
        # Load first layer
        print(f"Loading layer 1: {self.video_file1}")
        self.test_layer_load(cue_id_1, self.video_file1)
        time.sleep(2)
        
        # Load second layer
        print(f"Loading layer 2: {self.video_file2}")
        self.test_layer_load(cue_id_2, self.video_file2)
        time.sleep(2)
        
        # Step 4: Set mtcfollow on both layers (if not default enabled)
        print("\n--- Step 4: Set MTC Follow on Both Layers ---")
        # Check if mtcfollow is default enabled - if not, enable it explicitly
        # (According to implementation, it should default to enabled, but we'll set it anyway)
        self.send_osc(f"/videocomposer/layer/{cue_id_1}/mtcfollow", 1)
        time.sleep(0.2)
        self.send_osc(f"/videocomposer/layer/{cue_id_2}/mtcfollow", 1)
        time.sleep(0.2)
        print("MTC follow enabled on both layers")
        
        # Step 5: Enable OSD display
        print("\n--- Step 5: Enable OSD Display ---")
        # Enable timecode display (SMPTE) - send both commands for compatibility
        # /videocomposer/osd/timecode with integer 1 enables SMPTE mode
        self.send_osc("/videocomposer/osd/timecode", 1, verbose=True)
        time.sleep(0.1)
        # /videocomposer/osd/smpte with string sets y position (89 = near top, but not too high)
        self.send_osc("/videocomposer/osd/smpte", "70", verbose=True)
        time.sleep(0.1)
        # Enable frame number display (position at y=90%)
        self.send_osc("/videocomposer/osd/frame", 90, verbose=True)
        time.sleep(0.1)
        # Enable black box background for OSD
        self.send_osc("/videocomposer/osd/box", 1, verbose=True)
        time.sleep(0.1)
        print("OSD enabled: timecode and frame number display")
        
        # Step 6: Initial image controls setup
        print("\n--- Step 6: Initial Image Controls Setup ---")
        self.test_layer_controls(cue_id_1, layer_num=1)
        time.sleep(0.5)
        self.test_layer_controls(cue_id_2, layer_num=2)
        time.sleep(0.5)
        
        # Step 6.5: Enable looping if requested
        if self.enable_loop:
            print("\n--- Step 6.5: Enable Looping on Both Layers ---")
            # Enable infinite loop on both layers
            self.send_osc(f"/videocomposer/layer/{cue_id_1}/loop", 1, -1)  # Enable, infinite
            time.sleep(0.1)
            self.send_osc(f"/videocomposer/layer/{cue_id_2}/loop", 1, -1)  # Enable, infinite
            time.sleep(0.1)
            print("Looping enabled on both layers")
        
        # Step 7: Progressive image adjustments over 30 seconds
        print("\n--- Step 7: Progressive Image Adjustments (30 seconds) ---")
        print("Making continuous smooth adjustments to all image controls...")
        self.progressive_image_adjustments(cue_id_1, cue_id_2, duration=30)
        
        print("\n" + "=" * 60)
        print("All tests completed!")
        print("=" * 60)
        print("\nVideocomposer is running with:")
        print(f"  - Layer 1 (top-left, smaller, rotated): {cue_id_1}")
        print(f"  - Layer 2 (bottom-right, larger, different blend): {cue_id_2}")
        print("  - Both layers following MTC timecode")
        print("  - OSD enabled: timecode and frame number display")
        if self.enable_loop:
            print("  - Looping enabled on both layers")
        print("  - Progressive adjustments completed (30 seconds)")
        print("\nPress Ctrl+C to stop.")
        
        return True
    
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
    parser = argparse.ArgumentParser(description="Test dynamic file management system")
    parser.add_argument("--videocomposer", default="build/cuems-videocomposer",
                       help="Path to videocomposer executable (or use wrapper script)")
    parser.add_argument("--video1", required=True,
                       help="Path to first test video file or NDI source (e.g., 'ndi://Source Name')")
    parser.add_argument("--video2", 
                       help="Path to second test video file or NDI source (defaults to video1 if not provided)")
    parser.add_argument("--osc-port", type=int, default=7770,
                       help="OSC port (default: 7770)")
    parser.add_argument("--fps", type=float, default=25.0,
                       help="MTC framerate (default: 25.0)")
    parser.add_argument("--mtc-port", type=int, default=0,
                       help="ALSA MIDI port number for MTC (default: 0)")
    parser.add_argument("--loop", action="store_true",
                       help="Enable looping on layers (default: no loop)")
    
    args = parser.parse_args()
    
    # Use video1 for video2 if not provided
    video2 = args.video2 if args.video2 else args.video1
    
    # Support wrapper script for NDI library path
    videocomposer_bin = args.videocomposer
    if not Path(videocomposer_bin).exists():
        # Try wrapper script
        wrapper = Path(__file__).parent.parent / "cuems-videocomposer.sh"
        if wrapper.exists():
            videocomposer_bin = str(wrapper)
            print(f"Using wrapper script: {videocomposer_bin}")
    
    test = DynamicFileManagementTest(
        videocomposer_bin,
        args.video1,
        video2,
        args.osc_port,
        args.fps,
        args.mtc_port,
        args.loop
    )
    
    try:
        test.run_tests()
        
        # Keep running until interrupted
        while True:
            time.sleep(1)
            
    except KeyboardInterrupt:
        print("\n\nInterrupted by user")
    finally:
        test.stop()


if __name__ == "__main__":
    main()

