#!/usr/bin/env python3
"""
Test script for OSD options in cuems-videocomposer.
Tests all OSD modes: SMPTE, FRAME, TEXT, BOX, MSG
"""

import subprocess
import time
import socket
import sys
from pathlib import Path

def send_osc_command(port: int, path: str, value: int):
    """Send OSC command with integer argument."""
    try:
        # Path
        path_bytes = path.encode('utf-8') + b'\0'
        path_padded = path_bytes + b'\0' * ((4 - len(path_bytes) % 4) % 4)
        
        # Type tag (',i' for integer)
        type_tag = b',i\0'
        type_tag_padded = type_tag + b'\0' * ((4 - len(type_tag) % 4) % 4)
        
        # Integer value (4 bytes, big-endian)
        value_bytes = value.to_bytes(4, byteorder='big')
        
        # Combine message
        osc_msg = path_padded + type_tag_padded + value_bytes
        
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.sendto(osc_msg, ('127.0.0.1', port))
        sock.close()
    except Exception as e:
        print(f"    Warning: Failed to send OSC command: {e}")

def send_osc_string_command(port: int, path: str, string_value: str):
    """Send OSC command with string argument."""
    try:
        # Path
        path_bytes = path.encode('utf-8') + b'\0'
        path_padded = path_bytes + b'\0' * ((4 - len(path_bytes) % 4) % 4)
        
        # Type tag (',s' for string)
        type_tag = b',s\0'
        type_tag_padded = type_tag + b'\0' * ((4 - len(type_tag) % 4) % 4)
        
        # String value (null-terminated, padded to 4-byte boundary)
        string_bytes = string_value.encode('utf-8') + b'\0'
        string_padded = string_bytes + b'\0' * ((4 - len(string_bytes) % 4) % 4)
        
        # Combine message
        osc_msg = path_padded + type_tag_padded + string_padded
        
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.sendto(osc_msg, ('127.0.0.1', port))
        sock.close()
    except Exception as e:
        print(f"    Warning: Failed to send OSC string command: {e}")

def test_osd_options(video_path: Path, videocomposer_bin: Path):
    """Test all OSD options."""
    osc_port = 7000
    
    cmd = [
        str(videocomposer_bin),
        "-v",
        "--osc", str(osc_port),
        str(video_path)
    ]
    
    print("=" * 60)
    print("Testing OSD Options")
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
        
        print("\n  Test 1: SMPTE timecode at 89% (should have black box by default)")
        send_osc_string_command(osc_port, "/videocomposer/osd/smpte", "89")
        time.sleep(3)
        
        print("\n  Test 2: Frame number at 95%")
        send_osc_command(osc_port, "/videocomposer/osd/frame", 95)
        time.sleep(3)
        
        print("\n  Test 3: Custom text 'TEST TEXT' at center (direct OSC)")
        send_osc_string_command(osc_port, "/videocomposer/osd/text", "TEST TEXT")
        time.sleep(3)
        
        print("\n  Test 4: Toggle BOX off (transparent background)")
        send_osc_command(osc_port, "/videocomposer/osd/box", 0)
        time.sleep(3)
        
        print("\n  Test 5: Toggle BOX on (black background)")
        send_osc_command(osc_port, "/videocomposer/osd/box", 1)
        time.sleep(3)
        
        print("\n  Test 6: Disable SMPTE, keep FRAME")
        send_osc_string_command(osc_port, "/videocomposer/cmd", "osd smpte -1")
        time.sleep(3)
        
        print("\n  Test 7: Disable FRAME, enable SMPTE")
        send_osc_string_command(osc_port, "/videocomposer/cmd", "osd frame -1")
        send_osc_string_command(osc_port, "/videocomposer/osd/smpte", "89")
        time.sleep(3)
        
        print("\n  Test 8: Clear text")
        send_osc_string_command(osc_port, "/videocomposer/cmd", "osd notext")
        time.sleep(2)
        
        print("\n  All OSD tests completed!")
        print("  Please verify visually:")
        print("    - SMPTE timecode with black box at 89%")
        print("    - Frame number with black box at 95%")
        print("    - Custom text 'TEST TEXT' at center")
        print("    - BOX toggle (transparent/black)")
        
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
    
    videocomposer_bin = project_root / "scripts" / "cuems-videocomposer-wrapper.sh"
    
    video_path = project_root / "video_test_files" / "test_playback_patterns.mov"
    
    if not videocomposer_bin.exists():
        print(f"Error: videocomposer wrapper script not found at {videocomposer_bin}")
        sys.exit(1)
    
    if not video_path.exists():
        print(f"Error: Test video not found at {video_path}")
        sys.exit(1)
    
    test_osd_options(video_path, videocomposer_bin)

