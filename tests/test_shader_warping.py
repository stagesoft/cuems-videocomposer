#!/usr/bin/env python3
"""
Test shader-based corner deformation and warping quality modes.

This test verifies:
1. Shader-based rendering works without warping
2. Corner deformation works with standard quality
3. High-quality mode works for extreme warps
4. Shaders work with different video formats
"""

import subprocess
import time
import argparse
from pathlib import Path
from pythonosc import udp_client

def send_osc(address, path, *args, port=7700):
    """Send OSC message to videocomposer using pythonosc"""
    try:
        client = udp_client.SimpleUDPClient(address, port)
        # Convert all args to proper types (strings stay strings, numbers stay numbers)
        osc_args = []
        for arg in args:
            if isinstance(arg, (int, float)):
                osc_args.append(arg)
            else:
                osc_args.append(str(arg))
        # Debug: print what we're sending
        print(f"DEBUG OSC: {path} -> {osc_args}")
        client.send_message(path, osc_args)
        return True
    except Exception as e:
        print(f"OSC error: {e}")
        return False

def test_corner_deformation(video_path, duration=30):
    """Test corner deformation with various warp levels"""
    
    print("\n" + "="*70)
    print("SHADER-BASED CORNER DEFORMATION TEST")
    print("="*70)
    
    # Start videocomposer
    print("\n[1/6] Starting videocomposer...")
    script_dir = Path(__file__).parent
    project_root = script_dir.parent
    wrapper_script = project_root / "scripts" / "cuems-videocomposer-wrapper.sh"
    proc = subprocess.Popen(
        [str(wrapper_script), "--osc", "7700"],
        cwd=str(project_root),
        stdout=None,
        stderr=None
    )
    
    time.sleep(2)  # Wait for startup
    
    try:
        # Load video on layer 0
        # OSC format: /videocomposer/layer/load ss (filepath, cueId)
        print(f"\n[2/6] Loading video: {video_path}")
        send_osc("localhost", "/videocomposer/layer/load", str(video_path), "0")
        time.sleep(1)
        
        # Make layer visible and start playback
        # Layer commands use pattern: /videocomposer/layer/<id>/<command>
        # NOTE: Video will only play when MTC timecode is running
        send_osc("localhost", "/videocomposer/layer/0/visible", 1)
        send_osc("localhost", "/videocomposer/layer/0/play")
        
        print("\n[3/6] Testing standard rendering (no warp)...")
        print("  → Video should display normally")
        print("  → Check that shader rendering works")
        time.sleep(5)
        
        # Test 1: Subtle keystone correction (~10° warp)
        print("\n[4/6] Testing subtle warp (keystone correction)...")
        print("  → Applying ~10° perspective correction")
        print("  → Should use standard quality shader")
        
        # Top-left corner: pull inward slightly
        # Layer commands use pattern: /videocomposer/layer/<id>/<command>
        send_osc("localhost", "/videocomposer/layer/0/corner_deform", 
                0.0, -0.1, 0.0,   # Corner 0: (x, y) offset
                0.0, 0.0, -0.1,   # Corner 1
                0.0, 0.0, 0.1,    # Corner 2
                0.0, 0.1, 0.0)    # Corner 3
        send_osc("localhost", "/videocomposer/layer/0/corner_deform_enable", 1)
        
        time.sleep(5)
        
        # Test 2: Moderate warp (~20° warp)
        print("\n[5/6] Testing moderate warp...")
        print("  → Applying ~20° perspective warp")
        
        send_osc("localhost", "/videocomposer/layer/0/corner_deform",
                0.0, -0.2, 0.0,   # More pronounced warp
                0.0, 0.0, -0.2,
                0.0, 0.0, 0.2,
                0.0, 0.2, 0.0)
        
        time.sleep(5)
        
        # Test 3: Extreme warp with high-quality mode
        print("\n[6/6] Testing extreme warp (high-quality mode)...")
        print("  → Applying >30° projection mapping warp")
        print("  → Enabling high-quality anisotropic filtering")
        
        # Enable high-quality mode
        send_osc("localhost", "/videocomposer/layer/0/corner_deform_hq", 1)
        
        # Extreme warp
        send_osc("localhost", "/videocomposer/layer/0/corner_deform",
                0.0, -0.4, 0.0,   # Extreme warp
                0.0, -0.2, -0.4,
                0.0, 0.2, 0.4,
                0.0, 0.4, 0.2)
        
        time.sleep(5)
        
        # Test 4: Trapezoid (projection mapping scenario)
        print("\n[Bonus] Testing trapezoid warp (projector mapping)...")
        print("  → Simulating projection onto angled surface")
        
        send_osc("localhost", "/videocomposer/layer/0/corner_deform",
                0.0, -0.3, -0.2,  # Top narrower than bottom
                0.0, 0.3, -0.2,
                0.0, 0.4, 0.3,
                0.0, -0.4, 0.3)
        
        time.sleep(5)
        
        # Disable warping
        print("\n[Cleanup] Disabling warp, returning to normal...")
        send_osc("localhost", "/videocomposer/layer/0/corner_deform_enable", 0)
        send_osc("localhost", "/videocomposer/layer/0/corner_deform_hq", 0)
        
        time.sleep(3)
        
        print("\n" + "="*70)
        print("TEST COMPLETE")
        print("="*70)
        print("\nVisual inspection checklist:")
        print("  [?] Video rendered correctly without warping")
        print("  [?] Subtle warp applied smoothly")
        print("  [?] Moderate warp maintained quality")
        print("  [?] Extreme warp with HQ mode looked sharp")
        print("  [?] No visible tearing or corruption")
        print("  [?] Smooth transitions between warp levels")
        print("\nIf all items check out, shader warping is working correctly!")
        
    except KeyboardInterrupt:
        print("\n\nTest interrupted by user")
    finally:
        print("\nStopping videocomposer...")
        proc.terminate()
        proc.wait(timeout=5)

def test_multi_layer_warping(video1, video2, duration=30):
    """Test multiple layers with different warp settings"""
    
    print("\n" + "="*70)
    print("MULTI-LAYER WARPING TEST")
    print("="*70)
    
    print("\n[1/4] Starting videocomposer...")
    script_dir = Path(__file__).parent
    project_root = script_dir.parent
    wrapper_script = project_root / "scripts" / "cuems-videocomposer-wrapper.sh"
    proc = subprocess.Popen(
        [str(wrapper_script), "--osc", "7700"],
        cwd=str(project_root),
        stdout=None,
        stderr=None
    )
    
    time.sleep(2)
    
    try:
        # Load two videos
        # OSC format: /videocomposer/layer/load ss (filepath, cueId)
        print(f"\n[2/4] Loading layer 0: {video1}")
        send_osc("localhost", "/videocomposer/layer/load", str(video1), "0")
        time.sleep(1)
        
        print(f"[2/4] Loading layer 1: {video2}")
        send_osc("localhost", "/videocomposer/layer/load", str(video2), "1")
        time.sleep(1)
        
        # Setup layers
        # NOTE: Videos will only play when MTC timecode is running
        send_osc("localhost", "/videocomposer/layer/0/visible", 1)
        send_osc("localhost", "/videocomposer/layer/1/visible", 1)
        send_osc("localhost", "/videocomposer/layer/1/opacity", 0.7)  # Semi-transparent overlay
        send_osc("localhost", "/videocomposer/layer/0/play")
        send_osc("localhost", "/videocomposer/layer/1/play")
        
        print("\n[3/4] Applying different warps to each layer...")
        
        # Layer 0: subtle warp
        print("  → Layer 0: Subtle keystone correction")
        send_osc("localhost", "/videocomposer/layer/0/corner_deform",
                0.0, -0.1, 0.0,
                0.0, 0.1, 0.0,
                0.0, 0.1, 0.1,
                0.0, -0.1, 0.1)
        send_osc("localhost", "/videocomposer/layer/0/corner_deform_enable", 1)
        
        # Layer 1: moderate warp with HQ
        print("  → Layer 1: Moderate warp with high-quality")
        send_osc("localhost", "/videocomposer/layer/1/corner_deform",
                0.0, -0.25, -0.1,
                0.0, 0.25, -0.1,
                0.0, 0.2, 0.2,
                0.0, -0.2, 0.2)
        send_osc("localhost", "/videocomposer/layer/1/corner_deform_enable", 1)
        send_osc("localhost", "/videocomposer/layer/1/corner_deform_hq", 1)
        
        print(f"\n[4/4] Running for {duration} seconds...")
        print("  → Both layers should render warped simultaneously")
        print("  → Check for smooth playback and no corruption")
        
        time.sleep(duration)
        
        print("\n" + "="*70)
        print("MULTI-LAYER TEST COMPLETE")
        print("="*70)
        
    except KeyboardInterrupt:
        print("\n\nTest interrupted by user")
    finally:
        print("\nStopping videocomposer...")
        proc.terminate()
        proc.wait(timeout=5)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Test shader-based corner deformation")
    parser.add_argument("--video", default="video_test_files/test_h264_24fps.mp4",
                       help="Video file to test with")
    parser.add_argument("--video2", default="video_test_files/test_hap.mov",
                       help="Second video for multi-layer test")
    parser.add_argument("--duration", type=int, default=30,
                       help="Test duration in seconds")
    parser.add_argument("--test", choices=["basic", "multi", "both"], default="basic",
                       help="Which test to run")
    
    args = parser.parse_args()
    
    video_path = Path(args.video)
    if not video_path.exists():
        print(f"Error: Video file not found: {video_path}")
        print("Please provide a valid video file path with --video")
        exit(1)
    
    # Convert to absolute path for OSC
    video_path = video_path.resolve()
    
    try:
        if args.test in ["basic", "both"]:
            test_corner_deformation(video_path, args.duration)
            
            if args.test == "both":
                print("\n\nWaiting 3 seconds before multi-layer test...")
                time.sleep(3)
        
        if args.test in ["multi", "both"]:
            video2_path = Path(args.video2)
            if not video2_path.exists():
                print(f"Warning: Second video not found: {video2_path}")
                print("Skipping multi-layer test")
            else:
                video2_path = video2_path.resolve()
                test_multi_layer_warping(video_path, video2_path, args.duration)
    
    except Exception as e:
        print(f"\nTest error: {e}")
        import traceback
        traceback.print_exc()

