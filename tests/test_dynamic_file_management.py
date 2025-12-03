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
    # Window bounds (assume typical window size, adjust if needed)
    WINDOW_WIDTH = 1280
    WINDOW_HEIGHT = 720
    # Assume typical video size for bounds calculation (will be conservative)
    ASSUMED_VIDEO_WIDTH = 1920
    ASSUMED_VIDEO_HEIGHT = 1080
    
    def __init__(self, videocomposer_bin, video_file1=None, video_file2=None, video_file3=None, osc_port=7770, fps=25.0, mtc_port=0, enable_loop=False):
        self.videocomposer_bin = Path(videocomposer_bin)
        # Store as strings to support NDI sources (not file paths)
        self.video_file1 = video_file1
        self.video_file2 = video_file2
        self.video_file3 = video_file3
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
    
    def clamp_position(self, x, y, scale):
        """Clamp position to keep scaled layer within window bounds."""
        # Calculate scaled layer size
        scaled_width = self.ASSUMED_VIDEO_WIDTH * scale
        scaled_height = self.ASSUMED_VIDEO_HEIGHT * scale
        
        # Clamp x: ensure layer doesn't go outside left or right
        x_min = 0
        x_max = self.WINDOW_WIDTH - scaled_width
        x = max(x_min, min(x, x_max))
        
        # Clamp y: ensure layer doesn't go outside top or bottom
        y_min = 0
        y_max = self.WINDOW_HEIGHT - scaled_height
        y = max(y_min, min(y, y_max))
        
        return x, y
        
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
            # Don't force DISPLAY - let the application use the environment's DISPLAY
            # or fall back to DRM/KMS/headless mode if no display server is available
            
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
    
    def test_layer_controls(self, cue_id, layer_num=1, second=0):
        """Test per-layer image controls - initial setup with time-based variations."""
        print(f"\n=== Test: Image controls for layer {layer_num} (second {second}) ===")
        print(f"Cue ID: {cue_id}")
        
        # Different controls for each layer
        # Use moderate scales and centered positions to keep layers visible
        if layer_num == 1:
            # Layer 1: Left side, good size
            position_x = 50
            position_y = 100
            opacity = 0.9
            scale = 0.4  # 40% scale - visible but not too big
            rotation = 0.0
            zorder = 1
            blendmode = 0  # NORMAL
            
            tests = [
                ("/videocomposer/layer/{}/position", position_x, position_y),
                ("/videocomposer/layer/{}/opacity", opacity),
                ("/videocomposer/layer/{}/scale", scale, scale),
                ("/videocomposer/layer/{}/rotation", rotation),
                ("/videocomposer/layer/{}/visible", 1),
                ("/videocomposer/layer/{}/zorder", zorder),
                ("/videocomposer/layer/{}/blendmode", blendmode),
            ]
        elif layer_num == 2:
            # Layer 2: Right side, good size
            position_x = 400
            position_y = 100
            opacity = 0.9
            scale = 0.4  # 40% scale - visible but not too big
            rotation = 0.0
            zorder = 2
            blendmode = 0  # NORMAL
            
            tests = [
                ("/videocomposer/layer/{}/position", position_x, position_y),
                ("/videocomposer/layer/{}/opacity", opacity),
                ("/videocomposer/layer/{}/scale", scale, scale),
                ("/videocomposer/layer/{}/rotation", rotation),
                ("/videocomposer/layer/{}/visible", 1),
                ("/videocomposer/layer/{}/zorder", zorder),
                ("/videocomposer/layer/{}/blendmode", blendmode),
            ]
        else:  # layer_num == 3
            # Layer 3: Center-bottom, good size
            position_x = 200
            position_y = 350
            opacity = 0.9
            scale = 0.4  # 40% scale - visible but not too big
            rotation = 0.0
            zorder = 3
            blendmode = 0  # NORMAL
            
            tests = [
                ("/videocomposer/layer/{}/position", position_x, position_y),
                ("/videocomposer/layer/{}/opacity", opacity),
                ("/videocomposer/layer/{}/scale", scale, scale),
                ("/videocomposer/layer/{}/rotation", rotation),
                ("/videocomposer/layer/{}/visible", 1),
                ("/videocomposer/layer/{}/zorder", zorder),
                ("/videocomposer/layer/{}/blendmode", blendmode),
            ]
        
        for path_template, *args in tests:
            path = path_template.format(cue_id)
            self.send_osc(path, *args, verbose=True)
            time.sleep(0.05)  # Faster updates for smoother transitions
        
        return True
    
    def test_layer_controls_repeat(self, cue_id_1, cue_id_2, cue_id_3=None, duration=6):
        """Send initial image controls setup and wait for specified duration."""
        print(f"\n--- Step 6: Initial Image Controls Setup ---")
        print("Setting initial layer properties...")
        
        # Send initial values once (second=0)
        self.test_layer_controls(cue_id_1, layer_num=1, second=0)
        time.sleep(0.1)
        self.test_layer_controls(cue_id_2, layer_num=2, second=0)
        time.sleep(0.1)
        if cue_id_3:
            self.test_layer_controls(cue_id_3, layer_num=3, second=0)
            time.sleep(0.1)
        
        print(f"Initial values set. Waiting {duration} seconds...")
        time.sleep(duration)
        print(f"Completed {duration} seconds wait")
        return True
    
    def progressive_image_adjustments(self, cue_id_1, cue_id_2, cue_id_3=None, duration=30):
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
            'x2': None, 'y2': None, 'opacity2': None, 'scale2': None, 'rotation2': None,
            'x3': None, 'y3': None, 'opacity3': None, 'scale3': None, 'rotation3': None
        }
        
        # Don't alternate layers - update all every frame for maximum smoothness
        frame_counter = 0
        
        while time.time() - start_time < duration:
            elapsed = time.time() - start_time
            progress = elapsed / duration
            
            # Use sine/cosine for smooth oscillations with different frequencies
            # Use slower frequencies for smoother, less jarring motion
            # Multiple frequencies create more interesting patterns
            sin_val = math.sin(progress * 1.5 * math.pi)  # Slower for smoother motion
            cos_val = math.cos(progress * 1.5 * math.pi)
            sin_val2 = math.sin(progress * 2.0 * math.pi)  # Different frequency for variety
            cos_val2 = math.cos(progress * 1.2 * math.pi)
            sin_val3 = math.sin(progress * 0.8 * math.pi)  # Very slow for smooth opacity
            cos_val3 = math.cos(progress * 1.8 * math.pi)
            
            # Update both layers every frame for maximum smoothness
            # Use very small thresholds to allow smooth interpolation
            
            # Layer 1 adjustments
            # Scale: smooth oscillation between 0.35 and 0.45 (small range for stability)
            scale1 = 0.35 + 0.1 * (0.5 + 0.5 * sin_val2)  # Scale: 0.35-0.45
            
            # Position: move in a smooth circular pattern (small radius)
            radius = 50  # Small radius to stay visible
            x1 = 150 + radius * math.cos(progress * math.pi)
            y1 = 150 + radius * math.sin(progress * math.pi)
            # Send position as FLOATS for sub-pixel smoothness
            self.send_osc(f"/videocomposer/layer/{cue_id_1}/position", float(x1), float(y1), verbose=False)
            prev_values['x1'] = x1
            prev_values['y1'] = y1
            
            # Opacity: smooth pulse between 0.7 and 1.0 (stay visible)
            opacity1 = 0.7 + 0.3 * (0.5 + 0.5 * sin_val3)
            self.send_osc(f"/videocomposer/layer/{cue_id_1}/opacity", opacity1, verbose=False)
            prev_values['opacity1'] = opacity1
            
            # Send scale
            self.send_osc(f"/videocomposer/layer/{cue_id_1}/scale", scale1, scale1, verbose=False)
            prev_values['scale1'] = scale1
            
            # Rotation: continuous smooth rotation (slower rotation for smoother motion)
            rotation1 = 15.0 + progress * 180.0  # Half rotation speed for smoother motion
            # Send EVERY frame unconditionally for maximum smoothness
            self.send_osc(f"/videocomposer/layer/{cue_id_1}/rotation", rotation1, verbose=False)
            prev_values['rotation1'] = rotation1
            
            # Layer 2 adjustments
            # Scale: smooth oscillation (between 0.35 and 0.45 - same as layer 1)
            scale2 = 0.35 + 0.1 * (0.5 + 0.5 * -sin_val2)  # Scale: 0.35-0.45
            
            # Position: move in opposite circular pattern (stay in visible area)
            radius2 = 50  # Small radius to stay visible
            x2 = 500 + radius2 * -math.cos(progress * 0.8 * math.pi)
            y2 = 150 + radius2 * -math.sin(progress * 0.8 * math.pi)
            # Send position as FLOATS for sub-pixel smoothness
            self.send_osc(f"/videocomposer/layer/{cue_id_2}/position", float(x2), float(y2), verbose=False)
            prev_values['x2'] = x2
            prev_values['y2'] = y2
            
            # Opacity: smooth opposite pulse (between 0.7 and 1.0 - stay visible)
            opacity2 = 0.7 + 0.3 * (0.5 + 0.5 * -sin_val3)
            self.send_osc(f"/videocomposer/layer/{cue_id_2}/opacity", opacity2, verbose=False)
            prev_values['opacity2'] = opacity2
            
            # Send scale
            self.send_osc(f"/videocomposer/layer/{cue_id_2}/scale", scale2, scale2, verbose=False)
            prev_values['scale2'] = scale2
            
            # Rotation: smooth opposite rotation (slower for smoother motion)
            rotation2 = -10.0 - progress * 180.0  # Half rotation speed for smoother motion
            # Send EVERY frame unconditionally for maximum smoothness
            self.send_osc(f"/videocomposer/layer/{cue_id_2}/rotation", rotation2, verbose=False)
            prev_values['rotation2'] = rotation2
            
            # Layer 3 adjustments (if provided)
            if cue_id_3:
                # Scale: smooth oscillation (between 0.35 and 0.45 - same range as others)
                scale3 = 0.35 + 0.1 * (0.5 + 0.5 * math.cos(progress * 2.5 * math.pi))  # Scale: 0.35-0.45
                
                # Position: figure-8 pattern for interesting motion (stay in visible area)
                radius3 = 40  # Small radius
                x3 = 300 + radius3 * math.sin(progress * 2 * math.pi)
                y3 = 350 + radius3 * math.sin(progress * 4 * math.pi) * 0.5
                self.send_osc(f"/videocomposer/layer/{cue_id_3}/position", float(x3), float(y3), verbose=False)
                prev_values['x3'] = x3
                prev_values['y3'] = y3
                
                # Opacity: smooth wave (between 0.7 and 1.0 - stay visible)
                opacity3 = 0.7 + 0.3 * (0.5 + 0.5 * math.sin(progress * 1.5 * math.pi))
                self.send_osc(f"/videocomposer/layer/{cue_id_3}/opacity", opacity3, verbose=False)
                prev_values['opacity3'] = opacity3
                
                # Send scale
                self.send_osc(f"/videocomposer/layer/{cue_id_3}/scale", scale3, scale3, verbose=False)
                prev_values['scale3'] = scale3
                
                # Rotation: continuous rotation (slower speed)
                rotation3 = 20.0 + progress * 120.0  # Slower rotation
                self.send_osc(f"/videocomposer/layer/{cue_id_3}/rotation", rotation3, verbose=False)
                prev_values['rotation3'] = rotation3
            
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
    
    def reset_and_distribute_layers(self, cue_id_1, cue_id_2, cue_id_3=None, duration=10):
        """Progressively reset rotation, set scale to 50%, and distribute layers in window."""
        print(f"\n=== Reset and Distribute Layers ({duration} seconds) ===")
        print("Resetting rotation to 0, setting scale to 50%, and distributing layers...")
        
        # Get initial state (from progressive adjustments - we'll interpolate from current)
        # Target state: rotation=0, scale=0.4, positions distributed
        target_scale = 0.4
        
        # Calculate distributed positions for layers (keep in visible area)
        if cue_id_3:
            # 3 layers: arrange in a row
            target_positions = [
                (50, 150),       # Layer 1: left
                (350, 150),      # Layer 2: center
                (200, 350),      # Layer 3: bottom-center
            ]
        else:
            # 2 layers: side by side
            target_positions = [
                (100, 150),      # Layer 1: left
                (450, 150),      # Layer 2: right
            ]
        
        start_time = time.time()
        interval = 0.016  # ~60 FPS
        frame_counter = 0
        last_log_time = start_time
        log_interval = 2.0
        
        # Store initial values (we'll interpolate from these)
        initial_values = {
            'rotation1': 15.0, 'scale1': 0.4, 'x1': 150, 'y1': 150,
            'rotation2': -10.0, 'scale2': 0.4, 'x2': 500, 'y2': 150,
        }
        if cue_id_3:
            initial_values.update({
                'rotation3': 20.0, 'scale3': 0.4, 'x3': 300, 'y3': 350,
            })
        
        while time.time() - start_time < duration:
            elapsed = time.time() - start_time
            progress = min(elapsed / duration, 1.0)  # 0.0 to 1.0
            
            # Smooth interpolation using ease-in-out curve
            ease_progress = progress * progress * (3.0 - 2.0 * progress)  # Smoothstep
            
            # Layer 1
            rotation1 = initial_values['rotation1'] * (1.0 - ease_progress)  # Interpolate to 0
            scale1 = initial_values['scale1'] + (target_scale - initial_values['scale1']) * ease_progress
            x1 = initial_values['x1'] + (target_positions[0][0] - initial_values['x1']) * ease_progress
            y1 = initial_values['y1'] + (target_positions[0][1] - initial_values['y1']) * ease_progress
            x1, y1 = self.clamp_position(x1, y1, scale1)
            
            self.send_osc(f"/videocomposer/layer/{cue_id_1}/rotation", rotation1, verbose=False)
            self.send_osc(f"/videocomposer/layer/{cue_id_1}/scale", scale1, scale1, verbose=False)
            self.send_osc(f"/videocomposer/layer/{cue_id_1}/position", float(x1), float(y1), verbose=False)
            
            # Layer 2
            rotation2 = initial_values['rotation2'] * (1.0 - ease_progress)  # Interpolate to 0
            scale2 = initial_values['scale2'] + (target_scale - initial_values['scale2']) * ease_progress
            x2 = initial_values['x2'] + (target_positions[1][0] - initial_values['x2']) * ease_progress
            y2 = initial_values['y2'] + (target_positions[1][1] - initial_values['y2']) * ease_progress
            x2, y2 = self.clamp_position(x2, y2, scale2)
            
            self.send_osc(f"/videocomposer/layer/{cue_id_2}/rotation", rotation2, verbose=False)
            self.send_osc(f"/videocomposer/layer/{cue_id_2}/scale", scale2, scale2, verbose=False)
            self.send_osc(f"/videocomposer/layer/{cue_id_2}/position", float(x2), float(y2), verbose=False)
            
            # Layer 3
            if cue_id_3:
                rotation3 = initial_values['rotation3'] * (1.0 - ease_progress)  # Interpolate to 0
                scale3 = initial_values['scale3'] + (target_scale - initial_values['scale3']) * ease_progress
                x3 = initial_values['x3'] + (target_positions[2][0] - initial_values['x3']) * ease_progress
                y3 = initial_values['y3'] + (target_positions[2][1] - initial_values['y3']) * ease_progress
                x3, y3 = self.clamp_position(x3, y3, scale3)
                
                self.send_osc(f"/videocomposer/layer/{cue_id_3}/rotation", rotation3, verbose=False)
                self.send_osc(f"/videocomposer/layer/{cue_id_3}/scale", scale3, scale3, verbose=False)
                self.send_osc(f"/videocomposer/layer/{cue_id_3}/position", float(x3), float(y3), verbose=False)
            
            frame_counter += 1
            
            # Log progress
            current_time = time.time()
            if current_time - last_log_time >= log_interval:
                print(f"  {elapsed:.1f}s / {duration}s - Progress: {progress*100:.1f}%")
                last_log_time = current_time
            
            # Sleep for smooth timing
            elapsed_loop = time.time() - start_time
            next_frame_time = frame_counter * interval
            sleep_time = next_frame_time - elapsed_loop
            if sleep_time > 0:
                time.sleep(sleep_time)
        
        print("Reset and distribute completed!")
        return True
    
    def corner_deformation_phase(self, cue_id_1, cue_id_2, cue_id_3=None, duration=15):
        """Smoothly animate corner deformation for all layers."""
        print(f"\n=== Corner Deformation Phase ({duration} seconds) ===")
        print("Animating corner deformation smoothly...")
        
        start_time = time.time()
        interval = 0.016  # ~60 FPS
        frame_counter = 0
        last_log_time = start_time
        log_interval = 2.0
        
        # Assume layer size for corner calculations (normalized 0-1 coordinates)
        # Corners are: corner1 (top-left), corner2 (top-right), corner3 (bottom-right), corner4 (bottom-left)
        # Format: x1, y1, x2, y2, x3, y3, x4, y4
        
        while time.time() - start_time < duration:
            elapsed = time.time() - start_time
            progress = elapsed / duration
            
            # Use sine waves for smooth corner movement
            sin_val = math.sin(progress * 2 * math.pi)
            cos_val = math.cos(progress * 2 * math.pi)
            sin_val2 = math.sin(progress * 3 * math.pi)
            cos_val2 = math.cos(progress * 1.5 * math.pi)
            
            # Layer 1: Wave distortion
            # Corners in normalized coordinates (0.0-1.0)
            corner1_x = 0.0 + 0.1 * sin_val  # Top-left
            corner1_y = 0.0 + 0.1 * cos_val
            corner2_x = 1.0 + 0.1 * -sin_val  # Top-right
            corner2_y = 0.0 + 0.1 * cos_val2
            corner3_x = 1.0 + 0.1 * sin_val2  # Bottom-right
            corner3_y = 1.0 + 0.1 * -cos_val
            corner4_x = 0.0 + 0.1 * -sin_val2  # Bottom-left
            corner4_y = 1.0 + 0.1 * cos_val2
            
            self.send_osc(f"/videocomposer/layer/{cue_id_1}/corners",
                         corner1_x, corner1_y, corner2_x, corner2_y,
                         corner3_x, corner3_y, corner4_x, corner4_y, verbose=False)
            
            # Layer 2: Different pattern
            corner1_x = 0.0 + 0.15 * cos_val
            corner1_y = 0.0 + 0.15 * sin_val2
            corner2_x = 1.0 + 0.15 * -cos_val2
            corner2_y = 0.0 + 0.15 * -sin_val
            corner3_x = 1.0 + 0.15 * sin_val
            corner3_y = 1.0 + 0.15 * cos_val
            corner4_x = 0.0 + 0.15 * -sin_val2
            corner4_y = 1.0 + 0.15 * -cos_val2
            
            self.send_osc(f"/videocomposer/layer/{cue_id_2}/corners",
                         corner1_x, corner1_y, corner2_x, corner2_y,
                         corner3_x, corner3_y, corner4_x, corner4_y, verbose=False)
            
            # Layer 3: If provided
            if cue_id_3:
                corner1_x = 0.0 + 0.12 * sin_val2
                corner1_y = 0.0 + 0.12 * -cos_val
                corner2_x = 1.0 + 0.12 * cos_val2
                corner2_y = 0.0 + 0.12 * sin_val
                corner3_x = 1.0 + 0.12 * -sin_val
                corner3_y = 1.0 + 0.12 * cos_val2
                corner4_x = 0.0 + 0.12 * -cos_val
                corner4_y = 1.0 + 0.12 * -sin_val2
                
                self.send_osc(f"/videocomposer/layer/{cue_id_3}/corners",
                             corner1_x, corner1_y, corner2_x, corner2_y,
                             corner3_x, corner3_y, corner4_x, corner4_y, verbose=False)
            
            frame_counter += 1
            
            # Log progress
            current_time = time.time()
            if current_time - last_log_time >= log_interval:
                print(f"  {elapsed:.1f}s / {duration}s - Progress: {progress*100:.1f}%")
                last_log_time = current_time
            
            # Sleep for smooth timing
            elapsed_loop = time.time() - start_time
            next_frame_time = frame_counter * interval
            sleep_time = next_frame_time - elapsed_loop
            if sleep_time > 0:
                time.sleep(sleep_time)
        
        print("Corner deformation phase completed!")
        return True
    
    def master_controls_phase(self, duration=10):
        """Animate master/composite transforms that affect the entire output.
        
        Master controls apply to the composite of all layers (before OSD):
        - Position offset (normalized coordinates, -1 to 1)
        - Scale (1.0 = 100%)
        - Rotation (degrees)
        - Opacity (0.0 to 1.0)
        """
        print(f"\n=== Master Controls Phase ({duration} seconds) ===")
        print("Animating master/composite transforms (position, scale, rotation, opacity)...")
        print("These transforms apply to ALL layers combined before OSD.")
        
        start_time = time.time()
        interval = 0.016  # ~60 FPS
        frame_counter = 0
        last_log_time = start_time
        log_interval = 2.0
        
        while time.time() - start_time < duration:
            elapsed = time.time() - start_time
            progress = elapsed / duration
            
            # Use sine waves for smooth animation
            sin_val = math.sin(progress * 2 * math.pi)
            cos_val = math.cos(progress * 2 * math.pi)
            sin_val2 = math.sin(progress * 1.5 * math.pi)
            
            # Master position offset (normalized coordinates -1 to 1)
            # Increased range for more noticeable movement (similar to layer phases)
            pos_x = 0.25 * sin_val  # -0.25 to 0.25
            pos_y = 0.2 * cos_val   # -0.2 to 0.2
            self.send_osc("/videocomposer/master/position", pos_x, pos_y, verbose=False)
            
            # Master scale (more noticeable zoom in/out)
            scale = 0.8 + 0.4 * (0.5 + 0.5 * sin_val2)  # 0.8 to 1.2
            self.send_osc("/videocomposer/master/scale", scale, scale, verbose=False)
            
            # Master rotation (more noticeable swing)
            rotation = 15.0 * sin_val  # -15 to 15 degrees
            self.send_osc("/videocomposer/master/rotation", rotation, verbose=False)
            
            # Master opacity (more noticeable fade)
            opacity = 0.6 + 0.4 * (0.5 + 0.5 * cos_val)  # 0.6 to 1.0
            self.send_osc("/videocomposer/master/opacity", opacity, verbose=False)
            
            # Log once per second
            if frame_counter % 60 == 0:
                print(f"  pos=({pos_x:.3f}, {pos_y:.3f}), scale={scale:.3f}, "
                      f"rotation={rotation:.1f}°, opacity={opacity:.2f}")
            
            frame_counter += 1
            
            # Log progress
            current_time = time.time()
            if current_time - last_log_time >= log_interval:
                print(f"  {elapsed:.1f}s / {duration}s - Progress: {progress*100:.1f}%")
                last_log_time = current_time
            
            # Sleep for smooth timing
            elapsed_loop = time.time() - start_time
            next_frame_time = frame_counter * interval
            sleep_time = next_frame_time - elapsed_loop
            if sleep_time > 0:
                time.sleep(sleep_time)
        
        # Reset master transforms at end
        print("Resetting master transforms...")
        self.send_osc("/videocomposer/master/reset")
        
        print("Master controls phase completed!")
        return True
    
    def layer_color_correction_phase(self, cue_id_1, cue_id_2, cue_id_3=None, duration=10):
        """Animate per-layer color correction controls.
        
        Color corrections apply to individual layers:
        - Brightness (-1.0 to 1.0)
        - Contrast (0.0 to 2.0)
        - Saturation (0.0 to 2.0)
        - Hue (-180 to 180 degrees)
        - Gamma (0.1 to 3.0)
        """
        print(f"\n=== Layer Color Correction Phase ({duration} seconds) ===")
        print("Animating per-layer color corrections...")
        print("Each layer gets independent color adjustments.")
        
        start_time = time.time()
        interval = 0.016  # ~60 FPS
        frame_counter = 0
        last_log_time = start_time
        log_interval = 2.0
        
        while time.time() - start_time < duration:
            elapsed = time.time() - start_time
            progress = elapsed / duration
            
            # Use varied sine waves with different frequencies for extreme variations
            # Multiple frequencies create complex, varied patterns
            sin_val = math.sin(progress * 2.0 * math.pi)
            cos_val = math.cos(progress * 2.0 * math.pi)
            sin_val2 = math.sin(progress * 1.5 * math.pi)
            cos_val2 = math.cos(progress * 1.8 * math.pi)
            sin_val3 = math.sin(progress * 1.2 * math.pi)
            cos_val3 = math.cos(progress * 2.2 * math.pi)
            sin_val4 = math.sin(progress * 0.8 * math.pi)
            cos_val4 = math.cos(progress * 2.5 * math.pi)
            sin_val5 = math.sin(progress * 3.0 * math.pi)
            cos_val5 = math.cos(progress * 0.6 * math.pi)
            
            # Layer 1 color corrections - EXTREME variations
            # Brightness: -0.8 to 0.8 (was -0.3 to 0.3)
            brightness1 = 0.8 * sin_val
            # Contrast: 0.3 to 1.7 (was 0.7 to 1.3) - very dramatic
            contrast1 = 1.0 + 0.7 * cos_val2
            # Saturation: 0.0 to 2.0 (full range, was 0.5 to 1.5)
            saturation1 = 1.0 + 1.0 * (0.5 + 0.5 * sin_val3)  # 0.0 to 2.0 (grayscale to super saturated)
            # Hue: -180 to 180 (full range, was -60 to 60)
            hue1 = 180.0 * math.sin(progress * 1.5 * math.pi)  # Full color wheel rotation
            # Gamma: 0.3 to 2.5 (was 0.8 to 1.2) - very dramatic
            gamma1 = 1.4 + 1.1 * cos_val4
            
            self.send_osc(f"/videocomposer/layer/{cue_id_1}/brightness", brightness1, verbose=False)
            self.send_osc(f"/videocomposer/layer/{cue_id_1}/contrast", contrast1, verbose=False)
            self.send_osc(f"/videocomposer/layer/{cue_id_1}/saturation", saturation1, verbose=False)
            self.send_osc(f"/videocomposer/layer/{cue_id_1}/hue", hue1, verbose=False)
            self.send_osc(f"/videocomposer/layer/{cue_id_1}/gamma", gamma1, verbose=False)
            
            # Layer 2 color corrections - DIFFERENT extreme pattern
            # Brightness: -0.8 to 0.8 with different phase
            brightness2 = 0.8 * cos_val3
            # Contrast: 0.3 to 1.7 with different frequency
            contrast2 = 1.0 + 0.7 * sin_val5
            # Saturation: 0.0 to 2.0 with different pattern
            saturation2 = 1.0 + 1.0 * (0.5 + 0.5 * cos_val5)  # 0.0 to 2.0 (opposite phase from layer 1)
            # Hue: -180 to 180 with different speed
            hue2 = 180.0 * math.cos(progress * 1.8 * math.pi)  # Different frequency
            # Gamma: 0.3 to 2.5 with different pattern
            gamma2 = 1.4 + 1.1 * sin_val4
            
            self.send_osc(f"/videocomposer/layer/{cue_id_2}/brightness", brightness2, verbose=False)
            self.send_osc(f"/videocomposer/layer/{cue_id_2}/contrast", contrast2, verbose=False)
            self.send_osc(f"/videocomposer/layer/{cue_id_2}/saturation", saturation2, verbose=False)
            self.send_osc(f"/videocomposer/layer/{cue_id_2}/hue", hue2, verbose=False)
            self.send_osc(f"/videocomposer/layer/{cue_id_2}/gamma", gamma2, verbose=False)
            
            # Layer 3 color corrections - THIRD unique extreme pattern
            if cue_id_3:
                # Brightness: -0.8 to 0.8 with combined waves
                brightness3 = 0.8 * (0.5 * sin_val2 + 0.5 * cos_val4)
                # Contrast: 0.3 to 1.7 with complex pattern
                contrast3 = 1.0 + 0.7 * (0.6 * sin_val3 + 0.4 * cos_val)
                # Saturation: 0.0 to 2.0 with unique pattern
                sat_wave = 0.7 * sin_val4 + 0.3 * cos_val2
                saturation3 = 1.0 + 1.0 * (0.5 + 0.5 * sat_wave)  # 0.0 to 2.0
                # Hue: -180 to 180 with different speed
                hue3 = 180.0 * math.sin(progress * 2.2 * math.pi)  # Fast rotation
                # Gamma: 0.3 to 2.5 with complex pattern
                gamma_wave = 0.5 * cos_val3 + 0.5 * sin_val5
                gamma3 = 1.4 + 1.1 * gamma_wave
                
                self.send_osc(f"/videocomposer/layer/{cue_id_3}/brightness", brightness3, verbose=False)
                self.send_osc(f"/videocomposer/layer/{cue_id_3}/contrast", contrast3, verbose=False)
                self.send_osc(f"/videocomposer/layer/{cue_id_3}/saturation", saturation3, verbose=False)
                self.send_osc(f"/videocomposer/layer/{cue_id_3}/hue", hue3, verbose=False)
                self.send_osc(f"/videocomposer/layer/{cue_id_3}/gamma", gamma3, verbose=False)
            
            # Log once per second
            if frame_counter % 60 == 0:
                print(f"  Layer 1: brightness={brightness1:.2f}, contrast={contrast1:.2f}, "
                      f"saturation={saturation1:.2f}, hue={hue1:.1f}°, gamma={gamma1:.2f}")
                print(f"  Layer 2: brightness={brightness2:.2f}, contrast={contrast2:.2f}, "
                      f"saturation={saturation2:.2f}, hue={hue2:.1f}°, gamma={gamma2:.2f}")
                if cue_id_3:
                    print(f"  Layer 3: brightness={brightness3:.2f}, contrast={contrast3:.2f}, "
                          f"saturation={saturation3:.2f}, hue={hue3:.1f}°, gamma={gamma3:.2f}")
            
            frame_counter += 1
            
            # Log progress
            current_time = time.time()
            if current_time - last_log_time >= log_interval:
                print(f"  {elapsed:.1f}s / {duration}s - Progress: {progress*100:.1f}%")
                last_log_time = current_time
            
            # Sleep for smooth timing
            elapsed_loop = time.time() - start_time
            next_frame_time = frame_counter * interval
            sleep_time = next_frame_time - elapsed_loop
            if sleep_time > 0:
                time.sleep(sleep_time)
        
        # Reset layer color corrections at end
        print("Resetting layer color corrections...")
        self.send_osc(f"/videocomposer/layer/{cue_id_1}/color/reset")
        self.send_osc(f"/videocomposer/layer/{cue_id_2}/color/reset")
        if cue_id_3:
            self.send_osc(f"/videocomposer/layer/{cue_id_3}/color/reset")
        
        print("Layer color correction phase completed!")
        return True
    
    def master_color_correction_phase(self, duration=10):
        """Animate master/composite color correction controls.
        
        Master color corrections apply to the composite of all layers (before OSD):
        - Brightness (-1.0 to 1.0)
        - Contrast (0.0 to 2.0)
        - Saturation (0.0 to 2.0)
        - Hue (-180 to 180 degrees)
        - Gamma (0.1 to 3.0)
        """
        print(f"\n=== Master Color Correction Phase ({duration} seconds) ===")
        print("Animating master/composite color corrections...")
        print("These color adjustments apply to ALL layers combined before OSD.")
        
        start_time = time.time()
        interval = 0.016  # ~60 FPS
        frame_counter = 0
        last_log_time = start_time
        log_interval = 2.0
        
        while time.time() - start_time < duration:
            elapsed = time.time() - start_time
            progress = elapsed / duration
            
            # Use varied sine waves with multiple frequencies for extreme, complex variations
            sin_val = math.sin(progress * 2.0 * math.pi)
            cos_val = math.cos(progress * 2.0 * math.pi)
            sin_val2 = math.sin(progress * 1.5 * math.pi)
            cos_val2 = math.cos(progress * 1.8 * math.pi)
            sin_val3 = math.sin(progress * 0.7 * math.pi)
            cos_val3 = math.cos(progress * 2.3 * math.pi)
            sin_val4 = math.sin(progress * 3.2 * math.pi)
            
            # Master color correction - EXTREME variations
            # Brightness: -0.8 to 0.8 (was -0.3 to 0.3) - full range
            brightness = 0.8 * sin_val
            self.send_osc("/videocomposer/master/brightness", brightness, verbose=False)
            
            # Contrast: 0.3 to 1.7 (was 0.7 to 1.3) - very dramatic
            contrast = 1.0 + 0.7 * cos_val2
            self.send_osc("/videocomposer/master/contrast", contrast, verbose=False)
            
            # Saturation: 0.0 to 2.0 (was 0.5 to 1.5) - full range including grayscale
            saturation = 1.0 + 1.0 * (0.5 + 0.5 * sin_val3)  # 0.0 to 2.0 (grayscale to super saturated)
            self.send_osc("/videocomposer/master/saturation", saturation, verbose=False)
            
            # Hue: -180 to 180 (was -60 to 60) - full color wheel rotation
            hue = 180.0 * math.sin(progress * 1.5 * math.pi)  # Complete color cycle
            self.send_osc("/videocomposer/master/hue", hue, verbose=False)
            
            # Gamma: 0.3 to 2.5 (was 0.8 to 1.2) - very dramatic range
            gamma = 1.4 + 1.1 * cos_val3  # From very dark (0.3) to very bright (2.5)
            self.send_osc("/videocomposer/master/gamma", gamma, verbose=False)
            
            # Log once per second
            if frame_counter % 60 == 0:
                print(f"  brightness={brightness:.2f}, contrast={contrast:.2f}, "
                      f"saturation={saturation:.2f}, hue={hue:.1f}°, gamma={gamma:.2f}")
            
            frame_counter += 1
            
            # Log progress
            current_time = time.time()
            if current_time - last_log_time >= log_interval:
                print(f"  {elapsed:.1f}s / {duration}s - Progress: {progress*100:.1f}%")
                last_log_time = current_time
            
            # Sleep for smooth timing
            elapsed_loop = time.time() - start_time
            next_frame_time = frame_counter * interval
            sleep_time = next_frame_time - elapsed_loop
            if sleep_time > 0:
                time.sleep(sleep_time)
        
        # Reset master color corrections at end
        print("Resetting master color corrections...")
        self.send_osc("/videocomposer/master/color/reset")
        
        print("Master color correction phase completed!")
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
        cue_id_3 = "770e8400-e29b-41d4-a716-446655440002"
        
        # Step 3: Load video files (up to 3 layers)
        print("\n--- Step 3: Load Video Files (up to 3 layers) ---")
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
            print("Using first video file/source for second layer")
            self.video_file2 = self.video_file1
        elif not self.is_ndi_source(self.video_file2):
            if not Path(self.video_file2).exists():
                print(f"WARNING: Second video file not found: {self.video_file2}")
                print("Using first video file/source for second layer")
                self.video_file2 = self.video_file1
        
        # Check if third video source is provided and valid
        use_layer_3 = False
        if self.video_file3:
            if self.is_ndi_source(self.video_file3):
                use_layer_3 = True
            elif Path(self.video_file3).exists():
                use_layer_3 = True
            else:
                print(f"WARNING: Third video file not found: {self.video_file3}")
                print("Skipping layer 3")
        
        # Load first layer
        print(f"Loading layer 1: {self.video_file1}")
        self.test_layer_load(cue_id_1, self.video_file1)
        time.sleep(2)
        
        # Load second layer
        print(f"Loading layer 2: {self.video_file2}")
        self.test_layer_load(cue_id_2, self.video_file2)
        time.sleep(2)
        
        # Load third layer only if explicitly provided
        if use_layer_3:
            print(f"Loading layer 3: {self.video_file3}")
            self.test_layer_load(cue_id_3, self.video_file3)
            time.sleep(2)
        else:
            cue_id_3 = None  # Mark as not used
            print("Running with 2 layers only")
        
        # Step 4: Set mtcfollow on all layers (if not default enabled)
        print("\n--- Step 4: Set MTC Follow on All Layers ---")
        # Check if mtcfollow is default enabled - if not, enable it explicitly
        # (According to implementation, it should default to enabled, but we'll set it anyway)
        self.send_osc(f"/videocomposer/layer/{cue_id_1}/mtcfollow", 1)
        time.sleep(0.2)
        self.send_osc(f"/videocomposer/layer/{cue_id_2}/mtcfollow", 1)
        time.sleep(0.2)
        if cue_id_3:
            self.send_osc(f"/videocomposer/layer/{cue_id_3}/mtcfollow", 1)
            time.sleep(0.2)
        print("MTC follow enabled on all layers")
        
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
        
        # Step 6: Initial image controls setup (repeating for 6 seconds)
        self.test_layer_controls_repeat(cue_id_1, cue_id_2, cue_id_3, duration=6)
        
        # Step 6.5: Enable looping if requested
        if self.enable_loop:
            print("\n--- Step 6.5: Enable Looping on All Layers ---")
            # Enable infinite loop on all layers
            self.send_osc(f"/videocomposer/layer/{cue_id_1}/loop", 1, -1)  # Enable, infinite
            time.sleep(0.1)
            self.send_osc(f"/videocomposer/layer/{cue_id_2}/loop", 1, -1)  # Enable, infinite
            time.sleep(0.1)
            if cue_id_3:
                self.send_osc(f"/videocomposer/layer/{cue_id_3}/loop", 1, -1)  # Enable, infinite
                time.sleep(0.1)
            print("Looping enabled on all layers")
        
        # Step 7: Progressive image adjustments over 30 seconds
        print("\n--- Step 7: Progressive Image Adjustments (30 seconds) ---")
        print("Making continuous smooth adjustments to all image controls...")
        self.progressive_image_adjustments(cue_id_1, cue_id_2, cue_id_3, duration=30)
        
        # Step 8: Reset and distribute layers
        print("\n--- Step 8: Reset and Distribute Layers (10 seconds) ---")
        self.reset_and_distribute_layers(cue_id_1, cue_id_2, cue_id_3, duration=10)
        
        # Step 9: Corner deformation phase
        print("\n--- Step 9: Corner Deformation Phase (15 seconds) ---")
        self.corner_deformation_phase(cue_id_1, cue_id_2, cue_id_3, duration=15)
        
        # Step 10: Master controls phase
        print("\n--- Step 10: Master Controls Phase (10 seconds) ---")
        self.master_controls_phase(duration=10)
        
        # Step 11: Layer color correction phase
        print("\n--- Step 11: Layer Color Correction Phase (10 seconds) ---")
        self.layer_color_correction_phase(cue_id_1, cue_id_2, cue_id_3, duration=10)
        
        # Step 12: Master color correction phase
        print("\n--- Step 12: Master Color Correction Phase (10 seconds) ---")
        self.master_color_correction_phase(duration=10)
        
        print("\n" + "=" * 60)
        print("All tests completed!")
        print("=" * 60)
        print("\nVideocomposer is running with:")
        print(f"  - Layer 1: {cue_id_1}")
        print(f"  - Layer 2: {cue_id_2}")
        if cue_id_3:
            print(f"  - Layer 3: {cue_id_3}")
        print("  - All layers following MTC timecode")
        print("  - OSD enabled: timecode and frame number display")
        if self.enable_loop:
            print("  - Looping enabled on all layers")
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
    parser.add_argument("--videocomposer", default="scripts/cuems-videocomposer-wrapper.sh",
                       help="Path to videocomposer wrapper script")
    parser.add_argument("--video1", required=True,
                       help="Path to first test video file or NDI source (e.g., 'ndi://Source Name')")
    parser.add_argument("--video2", 
                       help="Path to second test video file or NDI source (defaults to video1 if not provided)")
    parser.add_argument("--video3", 
                       help="Path to third test video file or NDI source (optional - only 2 layers if not provided)")
    parser.add_argument("--osc-port", type=int, default=7770,
                       help="OSC port (default: 7770)")
    parser.add_argument("--fps", type=float, default=25.0,
                       help="MTC framerate (default: 25.0)")
    parser.add_argument("--mtc-port", type=int, default=0,
                       help="ALSA MIDI port number for MTC (default: 0)")
    parser.add_argument("--loop", action="store_true",
                       help="Enable looping on layers (default: no loop)")
    
    args = parser.parse_args()
    
    # Use video1 for video2 if not provided, but don't auto-create video3
    video2 = args.video2 if args.video2 else args.video1
    video3 = args.video3  # Only set if explicitly provided (None otherwise)
    
    # Use wrapper script
    videocomposer_bin = args.videocomposer
    if not Path(videocomposer_bin).exists():
        # Try wrapper script in scripts directory
        wrapper = Path(__file__).parent.parent / "scripts" / "cuems-videocomposer-wrapper.sh"
        if wrapper.exists():
            videocomposer_bin = str(wrapper)
            print(f"Using wrapper script: {videocomposer_bin}")
        else:
            print(f"ERROR: videocomposer wrapper script not found at {wrapper}")
            sys.exit(1)
    
    test = DynamicFileManagementTest(
        videocomposer_bin,
        args.video1,
        video2,
        video3,
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

