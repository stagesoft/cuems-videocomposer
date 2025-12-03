#!/usr/bin/env python3
"""
Simple multi-layer test without continuous OSC updates.
Tests if basic two-layer playback works without corruption.
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

# Import shared MTC helper
try:
    from mtc_helper import MTCHelper, MTC_AVAILABLE
except ImportError:
    print("WARNING: mtc_helper not found. MTC testing will be skipped.")
    MTC_AVAILABLE = False
    MTCHelper = None


def main():
    parser = argparse.ArgumentParser(description="Simple multi-layer test")
    parser.add_argument("--videocomposer", default="scripts/cuems-videocomposer-wrapper.sh",
                       help="Path to videocomposer wrapper script")
    parser.add_argument("--video1", required=True,
                       help="Path to first test video file")
    parser.add_argument("--video2", 
                       help="Path to second test video file (defaults to video1 if not provided)")
    parser.add_argument("--osc-port", type=int, default=7770,
                       help="OSC port (default: 7770)")
    parser.add_argument("--duration", type=int, default=60,
                       help="Test duration in seconds (default: 60)")
    parser.add_argument("--fps", type=float, default=25.0,
                       help="MTC framerate (default: 25.0)")
    
    args = parser.parse_args()
    
    videocomposer_bin = Path(args.videocomposer)
    if not videocomposer_bin.exists():
        print(f"ERROR: videocomposer not found at {videocomposer_bin}")
        sys.exit(1)
    
    video1 = Path(args.video1)
    video2 = Path(args.video2) if args.video2 else video1
    
    if not video1.exists():
        print(f"ERROR: Video file not found: {video1}")
        sys.exit(1)
    
    # Setup MTC
    mtc_helper = None
    if MTC_AVAILABLE:
        mtc_helper = MTCHelper(fps=args.fps, port=0, portname="SimpleTest")
        if mtc_helper.setup():
            print(f"MTC sender created (fps={args.fps})")
            mtc_helper.start(start_frame=0)
            print("MTC playback started")
        else:
            print("WARNING: Failed to create MTC sender")
            mtc_helper = None
    
    time.sleep(1)
    
    # Start videocomposer
    cmd = [
        str(videocomposer_bin),
        "--osc", str(args.osc_port),
        "--midi", "-1",
        "--verbose"
    ]
    
    env = os.environ.copy()
    # Don't force DISPLAY - let the application use the environment's DISPLAY
    # or fall back to DRM/KMS/headless mode if no display server is available
    
    print(f"Starting videocomposer: {' '.join(cmd)}")
    process = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        env=env
    )
    
    print(f"Started videocomposer (PID: {process.pid})")
    time.sleep(3)  # Wait for initialization
    
    # Connect OSC
    osc_client = pythonosc.udp_client.SimpleUDPClient('127.0.0.1', args.osc_port)
    print(f"Connected to OSC server on port {args.osc_port}")
    
    # Test cue IDs
    cue_id_1 = "test-layer-1"
    cue_id_2 = "test-layer-2"
    
    # Load two layers
    print(f"\nLoading layer 1: {video1}")
    osc_client.send_message("/videocomposer/layer/load", [str(video1), cue_id_1])
    time.sleep(2)
    
    print(f"Loading layer 2: {video2}")
    osc_client.send_message("/videocomposer/layer/load", [str(video2), cue_id_2])
    time.sleep(2)
    
    # Set mtcfollow on both layers
    print("Setting MTC follow on both layers")
    osc_client.send_message(f"/videocomposer/layer/{cue_id_1}/mtcfollow", [1])
    osc_client.send_message(f"/videocomposer/layer/{cue_id_2}/mtcfollow", [1])
    time.sleep(0.5)
    
    # Set different positions so we can see both layers
    print("Setting layer positions")
    osc_client.send_message(f"/videocomposer/layer/{cue_id_1}/position", [100, 100])
    osc_client.send_message(f"/videocomposer/layer/{cue_id_1}/scale", [0.5, 0.5])
    osc_client.send_message(f"/videocomposer/layer/{cue_id_1}/zorder", [1])
    
    osc_client.send_message(f"/videocomposer/layer/{cue_id_2}/position", [-100, -100])
    osc_client.send_message(f"/videocomposer/layer/{cue_id_2}/scale", [0.5, 0.5])
    osc_client.send_message(f"/videocomposer/layer/{cue_id_2}/zorder", [2])
    time.sleep(0.5)
    
    print(f"\n=== Running for {args.duration} seconds with NO continuous OSC updates ===")
    print("Watch the videocomposer window for any corruption...")
    print("Press Ctrl+C to stop early.\n")
    
    try:
        start_time = time.time()
        while time.time() - start_time < args.duration:
            elapsed = time.time() - start_time
            expected_frame = int(elapsed * args.fps)
            print(f"  Time: {elapsed:.1f}s, Expected frame: ~{expected_frame}", end='\r')
            time.sleep(1)
        print(f"\n\nTest completed after {args.duration} seconds!")
    except KeyboardInterrupt:
        print("\n\nInterrupted by user")
    finally:
        print("Stopping...")
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
        
        if mtc_helper:
            mtc_helper.stop()
            mtc_helper.cleanup()
        
        print("Done.")


if __name__ == "__main__":
    main()

