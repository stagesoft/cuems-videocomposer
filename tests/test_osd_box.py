#!/usr/bin/env python3
"""
Test script specifically for OSD box toggle functionality.
Tests that box background is black when enabled, transparent when disabled.
"""

import subprocess
import time
import socket
import sys
from pathlib import Path

def send_osc_command(port: int, path: str, value: int):
    """Send OSC command with integer argument."""
    try:
        path_bytes = path.encode('utf-8') + b'\0'
        path_padded = path_bytes + b'\0' * ((4 - len(path_bytes) % 4) % 4)
        type_tag = b',i\0'
        type_tag_padded = type_tag + b'\0' * ((4 - len(type_tag) % 4) % 4)
        value_bytes = value.to_bytes(4, byteorder='big')
        osc_msg = path_padded + type_tag_padded + value_bytes
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.sendto(osc_msg, ('127.0.0.1', port))
        sock.close()
    except Exception as e:
        print(f"    Warning: Failed to send OSC command: {e}")

def send_osc_string_command(port: int, path: str, string_value: str):
    """Send OSC command with string argument."""
    try:
        path_bytes = path.encode('utf-8') + b'\0'
        path_padded = path_bytes + b'\0' * ((4 - len(path_bytes) % 4) % 4)
        type_tag = b',s\0'
        type_tag_padded = type_tag + b'\0' * ((4 - len(type_tag) % 4) % 4)
        string_bytes = string_value.encode('utf-8') + b'\0'
        string_padded = string_bytes + b'\0' * ((4 - len(string_bytes) % 4) % 4)
        osc_msg = path_padded + type_tag_padded + string_padded
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.sendto(osc_msg, ('127.0.0.1', port))
        sock.close()
    except Exception as e:
        print(f"    Warning: Failed to send OSC string command: {e}")

def test_box_toggle(video_path: Path, videocomposer_bin: Path):
    """Test box toggle functionality."""
    osc_port = 7000
    
    cmd = [
        str(videocomposer_bin),
        "-v",
        "--osc", str(osc_port),
        str(video_path)
    ]
    
    print("=" * 60)
    print("Testing OSD Box Toggle")
    print("=" * 60)
    
    process = None
    try:
        print(f"  Starting videocomposer with video: {video_path.name}")
        process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1
        )
        
        # Wait for startup
        time.sleep(2)
        
        # Enable all OSD elements
        print("\n  Enabling OSD elements:")
        send_osc_string_command(osc_port, "/videocomposer/osd/smpte", "89")
        send_osc_command(osc_port, "/videocomposer/osd/frame", 95)
        send_osc_string_command(osc_port, "/videocomposer/osd/text", "BOX TEST")
        time.sleep(1)
        
        print("\n  Test 1: Box ON (should show BLACK backgrounds)")
        send_osc_command(osc_port, "/videocomposer/osd/box", 1)
        print("    Waiting 5 seconds - verify all OSD elements have BLACK boxes")
        time.sleep(5)
        
        print("\n  Test 2: Box OFF (should show TRANSPARENT backgrounds)")
        send_osc_command(osc_port, "/videocomposer/osd/box", 0)
        print("    Waiting 5 seconds - verify all OSD elements have TRANSPARENT backgrounds")
        time.sleep(5)
        
        print("\n  Test 3: Box ON again (should show BLACK backgrounds)")
        send_osc_command(osc_port, "/videocomposer/osd/box", 1)
        print("    Waiting 5 seconds - verify all OSD elements have BLACK boxes again")
        time.sleep(5)
        
        print("\n  Test 4: Box OFF again (should show TRANSPARENT backgrounds)")
        send_osc_command(osc_port, "/videocomposer/osd/box", 0)
        print("    Waiting 5 seconds - verify transparent backgrounds")
        time.sleep(5)
        
        print("\n  Box toggle test completed!")
        print("  Expected behavior:")
        print("    - Box ON: Black background behind all OSD text")
        print("    - Box OFF: Transparent background (text only, no box)")
        print("    - Should NOT show white background at any time")
        
    except KeyboardInterrupt:
        print("\n  Test interrupted by user")
    finally:
        if process:
            print("\n  Terminating videocomposer...")
            send_osc_command(osc_port, "/videocomposer/quit", 0)
            time.sleep(1)
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()

if __name__ == "__main__":
    script_dir = Path(__file__).parent
    project_root = script_dir.parent
    
    videocomposer_bin = project_root / "build" / "cuems-videocomposer"
    if not videocomposer_bin.exists():
        videocomposer_bin = project_root / "cuems-videocomposer"
    
    video_path = project_root / "video_test_files" / "test_playback_patterns.mov"
    
    if not videocomposer_bin.exists():
        print(f"Error: videocomposer binary not found at {videocomposer_bin}")
        sys.exit(1)
    
    if not video_path.exists():
        print(f"Error: Test video not found at {video_path}")
        sys.exit(1)
    
    test_box_toggle(video_path, videocomposer_bin)

