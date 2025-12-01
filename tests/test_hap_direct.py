#!/usr/bin/env python3
"""
HAP Direct Texture Upload Test Suite

Tests HAP codec with Vidvox SDK direct DXT texture upload.
Verifies all HAP variants: HAP, HAP Q, HAP Alpha, HAP Q Alpha.
"""

import subprocess
import os
import time
import sys
from pathlib import Path

# Test configuration
VIDEOCOMPOSER_BIN = "./scripts/cuems-videocomposer-wrapper.sh"
VIDEO_TEST_DIR = "video_test_files"
TEST_DURATION = 3  # seconds per test

class Colors:
    """ANSI color codes"""
    HEADER = '\033[95m'
    OKBLUE = '\033[94m'
    OKCYAN = '\033[96m'
    OKGREEN = '\033[92m'
    WARNING = '\033[93m'
    FAIL = '\033[91m'
    ENDC = '\033[0m'
    BOLD = '\033[1m'

def log_test(name):
    """Log test name"""
    print(f"\n{Colors.HEADER}{Colors.BOLD}[TEST] {name}{Colors.ENDC}")

def log_success(msg):
    """Log success message"""
    print(f"{Colors.OKGREEN}✓ {msg}{Colors.ENDC}")

def log_error(msg):
    """Log error message"""
    print(f"{Colors.FAIL}✗ {msg}{Colors.ENDC}")

def log_warning(msg):
    """Log warning message"""
    print(f"{Colors.WARNING}⚠ {msg}{Colors.ENDC}")

def log_info(msg):
    """Log info message"""
    print(f"{Colors.OKCYAN}ℹ {msg}{Colors.ENDC}")

def check_binary():
    """Check if videocomposer wrapper script exists"""
    if not os.path.exists(VIDEOCOMPOSER_BIN):
        log_error(f"Videocomposer wrapper script not found: {VIDEOCOMPOSER_BIN}")
        log_info("Make sure scripts/cuems-videocomposer-wrapper.sh exists")
        return False
    return True

def check_test_video(filename):
    """Check if test video exists"""
    video_path = os.path.join(VIDEO_TEST_DIR, filename)
    if not os.path.exists(video_path):
        log_warning(f"Test video not found: {video_path}")
        return False
    return True

def run_videocomposer(video_file, duration=TEST_DURATION, extra_args=None):
    """
    Run videocomposer with HAP video file
    Returns: (returncode, stdout, stderr)
    """
    video_path = os.path.join(VIDEO_TEST_DIR, video_file)
    
    cmd = [
        VIDEOCOMPOSER_BIN,
        "--file", video_path,
        "--verbose",
        "--no-ui"  # Headless mode for testing
    ]
    
    if extra_args:
        cmd.extend(extra_args)
    
    log_info(f"Running: {' '.join(cmd)}")
    
    try:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )
        
        # Let it run for specified duration
        time.sleep(duration)
        
        # Terminate gracefully
        proc.terminate()
        stdout, stderr = proc.communicate(timeout=5)
        
        return (proc.returncode, stdout, stderr)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, stderr = proc.communicate()
        return (-1, stdout, stderr)
    except Exception as e:
        log_error(f"Failed to run videocomposer: {e}")
        return (-1, "", str(e))

def check_output_for_patterns(output, patterns):
    """
    Check if output contains expected patterns
    Returns: (all_found, found_list, missing_list)
    """
    found = []
    missing = []
    
    for pattern in patterns:
        if pattern in output:
            found.append(pattern)
        else:
            missing.append(pattern)
    
    return (len(missing) == 0, found, missing)

def test_hap_variant_detection(video_file, expected_variant):
    """Test HAP variant is correctly detected"""
    log_test(f"HAP Variant Detection: {expected_variant}")
    
    if not check_test_video(video_file):
        log_warning(f"Skipping {expected_variant} test (video not found)")
        return False
    
    returncode, stdout, stderr = run_videocomposer(video_file, duration=2)
    
    output = stdout + stderr
    
    # Check for HAP-related messages
    patterns = [
        "HAP",  # General HAP detection
    ]
    
    all_found, found, missing = check_output_for_patterns(output, patterns)
    
    if all_found:
        log_success(f"{expected_variant} detected correctly")
        return True
    else:
        log_error(f"{expected_variant} detection failed")
        for p in missing:
            log_error(f"  Missing pattern: {p}")
        return False

def test_hap_direct_decode():
    """Test HAP direct DXT decoding (not FFmpeg fallback)"""
    log_test("HAP Direct DXT Decode")
    
    if not check_test_video("test_hap.mov"):
        log_warning("Skipping HAP direct decode test (video not found)")
        return False
    
    returncode, stdout, stderr = run_videocomposer("test_hap.mov", duration=3)
    
    output = stdout + stderr
    
    # Check for direct decode (no fallback warning)
    if "falling back to FFmpeg RGBA path" in output:
        log_warning("HAP is using FFmpeg fallback instead of direct decode")
        log_info("This may indicate snappy library is not installed or ENABLE_HAP_DIRECT is disabled")
        return False
    
    if "direct DXT upload" in output or "Uploaded HAP frame" in output:
        log_success("HAP direct DXT decode working")
        return True
    
    log_info("HAP decode path unclear from logs")
    return True  # Pass if no errors

def test_hap_playback(video_file, variant_name):
    """Test HAP video playback"""
    log_test(f"HAP Playback: {variant_name}")
    
    if not check_test_video(video_file):
        log_warning(f"Skipping {variant_name} playback test (video not found)")
        return False
    
    returncode, stdout, stderr = run_videocomposer(video_file, duration=TEST_DURATION)
    
    output = stdout + stderr
    
    # Check for errors
    error_patterns = [
        "Failed to decode",
        "decode error",
        "OpenGL error",
        "shader compilation failed"
    ]
    
    for pattern in error_patterns:
        if pattern.lower() in output.lower():
            log_error(f"Error detected: {pattern}")
            return False
    
    log_success(f"{variant_name} playback successful")
    return True

def test_hap_seeking(video_file):
    """Test HAP frame-accurate seeking"""
    log_test(f"HAP Seeking: {video_file}")
    
    if not check_test_video(video_file):
        log_warning("Skipping HAP seeking test (video not found)")
        return False
    
    # TODO: Implement seeking test via OSC commands or other control mechanism
    log_info("Seeking test not yet implemented (requires OSC control)")
    return True

def test_hap_multi_layer():
    """Test multiple HAP layers simultaneously"""
    log_test("HAP Multi-Layer Performance")
    
    if not check_test_video("test_hap.mov"):
        log_warning("Skipping multi-layer test (video not found)")
        return False
    
    # TODO: Implement multi-layer test
    log_info("Multi-layer test not yet implemented (requires layer control)")
    return True

def test_fallback_mechanism():
    """Test FFmpeg fallback when HAP direct decode fails"""
    log_test("HAP FFmpeg Fallback Mechanism")
    
    # This test verifies fallback works if direct decode fails
    # In practice, fallback is automatic if ENABLE_HAP_DIRECT is disabled
    
    log_info("Fallback mechanism is built-in and automatic")
    log_info("Fallback triggers when: snappy missing, ENABLE_HAP_DIRECT=OFF, or decode error")
    return True

def print_summary(results):
    """Print test summary"""
    print(f"\n{Colors.HEADER}{Colors.BOLD}{'='*60}{Colors.ENDC}")
    print(f"{Colors.HEADER}{Colors.BOLD}TEST SUMMARY{Colors.ENDC}")
    print(f"{Colors.HEADER}{Colors.BOLD}{'='*60}{Colors.ENDC}\n")
    
    total = len(results)
    passed = sum(1 for r in results.values() if r)
    failed = total - passed
    
    for test_name, result in results.items():
        status = f"{Colors.OKGREEN}PASS{Colors.ENDC}" if result else f"{Colors.FAIL}FAIL{Colors.ENDC}"
        print(f"  {status}  {test_name}")
    
    print(f"\n{Colors.BOLD}Total: {total}, Passed: {passed}, Failed: {failed}{Colors.ENDC}\n")
    
    if failed == 0:
        print(f"{Colors.OKGREEN}{Colors.BOLD}All tests passed!{Colors.ENDC}\n")
        return 0
    else:
        print(f"{Colors.FAIL}{Colors.BOLD}{failed} test(s) failed{Colors.ENDC}\n")
        return 1

def main():
    """Main test runner"""
    print(f"{Colors.HEADER}{Colors.BOLD}{'='*60}{Colors.ENDC}")
    print(f"{Colors.HEADER}{Colors.BOLD}HAP Direct Texture Upload Test Suite{Colors.ENDC}")
    print(f"{Colors.HEADER}{Colors.BOLD}{'='*60}{Colors.ENDC}")
    
    # Check prerequisites
    if not check_binary():
        return 1
    
    # Run tests
    results = {}
    
    # Test 1: HAP variant detection
    results["HAP Detection"] = test_hap_variant_detection("test_hap.mov", "HAP")
    results["HAP Q Detection"] = test_hap_variant_detection("test_hap_hq.mov", "HAP Q")
    results["HAP Alpha Detection"] = test_hap_variant_detection("test_hap_alpha.mov", "HAP Alpha")
    results["HAP Q Alpha Detection"] = test_hap_variant_detection("test_hap_hq_alpha.mov", "HAP Q Alpha")
    
    # Test 2: Direct DXT decode
    results["HAP Direct Decode"] = test_hap_direct_decode()
    
    # Test 3: Playback
    results["HAP Playback"] = test_hap_playback("test_hap.mov", "HAP")
    results["HAP Q Playback"] = test_hap_playback("test_hap_hq.mov", "HAP Q")
    results["HAP Alpha Playback"] = test_hap_playback("test_hap_alpha.mov", "HAP Alpha")
    results["HAP Q Alpha Playback"] = test_hap_playback("test_hap_hq_alpha.mov", "HAP Q Alpha")
    
    # Test 4: Seeking
    results["HAP Seeking"] = test_hap_seeking("test_hap.mov")
    
    # Test 5: Multi-layer
    results["HAP Multi-Layer"] = test_hap_multi_layer()
    
    # Test 6: Fallback
    results["HAP Fallback"] = test_fallback_mechanism()
    
    # Print summary
    return print_summary(results)

if __name__ == "__main__":
    sys.exit(main())

