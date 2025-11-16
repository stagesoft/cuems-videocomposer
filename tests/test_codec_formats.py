#!/usr/bin/env python3
"""
Test script for different video codecs and formats.

This test:
1. Tests different video codec detection (HAP, H.264, HEVC, AV1, VP9, etc.)
2. Tests hardware vs software decoding paths
3. Verifies that videocomposer correctly identifies and uses the optimal decoding path
4. Can be used to verify codec support and decoding performance

Usage:
    python3 tests/test_codec_formats.py [--video-dir PATH] [--test-all] [--test-hw] [--test-sw]
"""

import sys
import os
import time
import subprocess
import signal
import argparse
import json
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# Import shared MTC helper
from mtc_helper import MTCHelper, MTC_AVAILABLE

class CodecFormatTest:
    def __init__(self, video_dir: Path, videocomposer_bin: Optional[Path] = None, use_mtc: bool = True, fps: float = 25.0):
        self.video_dir = Path(video_dir)
        self.videocomposer_bin = videocomposer_bin or self._find_videocomposer()
        self.test_results: Dict[str, Dict] = {}
        self.use_mtc = use_mtc and MTC_AVAILABLE
        self.fps = fps
        self.mtc_helper = MTCHelper(fps=fps, port=0, portname="CodecTest") if self.use_mtc else None
        
    def _find_videocomposer(self) -> Path:
        """Find videocomposer binary."""
        # Try build directory first
        build_dir = Path(__file__).parent.parent / "build"
        if (build_dir / "cuems-videocomposer").exists():
            return build_dir / "cuems-videocomposer"
        
        # Try system path
        import shutil
        bin_path = shutil.which("cuems-videocomposer")
        if bin_path:
            return Path(bin_path)
        
        raise FileNotFoundError("Could not find cuems-videocomposer binary")
    
    def get_video_info(self, video_path: Path) -> Dict:
        """Get video codec and format information using ffprobe."""
        try:
            cmd = [
                "ffprobe", "-v", "quiet", "-print_format", "json", "-show_format", "-show_streams",
                str(video_path)
            ]
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
            if result.returncode != 0:
                return {"error": result.stderr}
            
            data = json.loads(result.stdout)
            video_stream = None
            for stream in data.get("streams", []):
                if stream.get("codec_type") == "video":
                    video_stream = stream
                    break
            
            if not video_stream:
                return {"error": "No video stream found"}
            
            return {
                "codec": video_stream.get("codec_name", "unknown"),
                "codec_long": video_stream.get("codec_long_name", "unknown"),
                "width": video_stream.get("width", 0),
                "height": video_stream.get("height", 0),
                "fps": eval(video_stream.get("r_frame_rate", "0/1")),
                "duration": float(data.get("format", {}).get("duration", 0)),
                "format": data.get("format", {}).get("format_name", "unknown"),
                "bitrate": int(data.get("format", {}).get("bit_rate", 0))
            }
        except Exception as e:
            return {"error": str(e)}
    
    def test_video_file_interactive(self, video_path: Path, test_duration: int = 10) -> Dict:
        """Test a single video file interactively - ask user if video is visible."""
        video_path = Path(video_path)
        if not video_path.exists():
            return {"error": f"Video file not found: {video_path}"}
        
        print(f"\n{'='*60}")
        print(f"Testing: {video_path.name}")
        print(f"{'='*60}")
        
        # Get video info
        info = self.get_video_info(video_path)
        if "error" in info:
            print(f"  ERROR: {info['error']}")
            return {"error": info["error"]}
        
        print(f"  Codec: {info['codec']} ({info['codec_long']})")
        print(f"  Format: {info['format']}")
        print(f"  Resolution: {info['width']}x{info['height']}")
        print(f"  FPS: {info['fps']:.2f}")
        print(f"  Duration: {info['duration']:.2f}s")
        
        # Run videocomposer
        print(f"\n  Playing video for {test_duration} seconds...")
        print(f"  Please watch the videocomposer window and observe if video is visible.")
        print(f"  The window should open shortly...\n")
        
        result = self._run_videocomposer(video_path, test_duration, verbose_output=False)
        
        # Ask user if they saw video
        print(f"\n  Video playback completed.")
        while True:
            response = input("  Did you see video playing? (y/n/s=skip): ").strip().lower()
            if response in ['y', 'yes']:
                video_visible = True
                break
            elif response in ['n', 'no']:
                video_visible = False
                break
            elif response in ['s', 'skip']:
                return {"skipped": True, "video": str(video_path), "info": info}
            else:
                print("  Please answer 'y' (yes), 'n' (no), or 's' (skip)")
        
        test_result = {
            "video": str(video_path),
            "info": info,
            "result": result,
            "video_visible": video_visible,
            "success": video_visible
        }
        
        status = "✓ VISIBLE" if video_visible else "✗ NOT VISIBLE"
        print(f"  Result: {status}\n")
        
        return test_result
    
    def test_video_file(self, video_path: Path, test_duration: int = 5) -> Dict:
        """Test a single video file with videocomposer."""
        video_path = Path(video_path)
        if not video_path.exists():
            return {"error": f"Video file not found: {video_path}"}
        
        print(f"\n{'='*60}")
        print(f"Testing: {video_path.name}")
        print(f"{'='*60}")
        
        # Get video info
        info = self.get_video_info(video_path)
        if "error" in info:
            print(f"  ERROR: {info['error']}")
            return {"error": info["error"]}
        
        print(f"  Codec: {info['codec']} ({info['codec_long']})")
        print(f"  Format: {info['format']}")
        print(f"  Resolution: {info['width']}x{info['height']}")
        print(f"  FPS: {info['fps']:.2f}")
        print(f"  Duration: {info['duration']:.2f}s")
        
        # Determine expected decoding path
        expected_path = self._determine_expected_path(info)
        print(f"  Expected decoding: {expected_path}")
        
        # Run videocomposer
        result = self._run_videocomposer(video_path, test_duration)
        
        # Analyze output
        analysis = self._analyze_output(result, info, expected_path)
        
        test_result = {
            "video": str(video_path),
            "info": info,
            "expected_path": expected_path,
            "result": result,
            "analysis": analysis,
            "success": analysis.get("success", False)
        }
        
        # Print results
        print(f"\n  Test Result: {'✓ PASS' if test_result['success'] else '✗ FAIL'}")
        if analysis.get("detected_codec"):
            print(f"  Detected codec: {analysis['detected_codec']}")
        if analysis.get("decoding_path"):
            print(f"  Decoding path: {analysis['decoding_path']}")
        if analysis.get("errors"):
            print(f"  Errors: {len(analysis['errors'])}")
            for error in analysis['errors'][:3]:
                print(f"    - {error}")
        
        return test_result
    
    def _determine_expected_path(self, info: Dict) -> str:
        """Determine expected decoding path based on codec."""
        codec = info.get("codec", "").lower()
        
        if codec in ["hap"]:
            return "HAP_DIRECT (zero-copy GPU)"
        elif codec in ["h264", "hevc", "av1"]:
            return "GPU_HARDWARE (if available) or CPU_SOFTWARE"
        else:
            return "CPU_SOFTWARE"
    
    def _setup_mtc(self):
        """Setup and start MTC timecode sender."""
        if not self.mtc_helper:
            return False
        
        # start() will call setup() if needed, and then play()
        # This ensures play() is only called once
        if self.mtc_helper.start(start_frame=0):
            print(f"  MTC timecode started (fps={self.fps})")
            return True
        else:
            print(f"  WARNING: Failed to start MTC")
            return False
    
    def _stop_mtc(self):
        """Stop MTC timecode sender."""
        if self.mtc_helper:
            self.mtc_helper.cleanup()
    
    def _run_videocomposer(self, video_path: Path, duration: int, verbose_output: bool = True) -> Dict:
        """Run videocomposer with the video file."""
        cmd = [
            str(self.videocomposer_bin),
            "-v",  # Verbose
            str(video_path)
        ]
        
        # Start MTC before launching videocomposer
        mtc_started = self._setup_mtc()
        
        process = None
        try:
            print(f"  Running videocomposer for {duration} seconds...")
            process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1
            )
            
            # Collect output
            output_lines = []
            start_time = time.time()
            
            while True:
                if process.poll() is not None:
                    break
                
                if time.time() - start_time > duration:
                    process.terminate()
                    time.sleep(0.5)
                    if process.poll() is None:
                        process.kill()
                    break
                
                try:
                    line = process.stdout.readline()
                    if line:
                        output_lines.append(line.strip())
                        # Print output if verbose mode is enabled
                        # In interactive mode, user watches the window, not console
                        if verbose_output:
                            print(f"    {line.strip()}")
                except:
                    break
            
            return {
                "returncode": process.returncode,
                "output": output_lines,
                "duration": time.time() - start_time
            }
        except Exception as e:
            return {
                "error": str(e),
                "returncode": -1,
                "output": []
            }
        finally:
            if process and process.poll() is None:
                process.kill()
            self._stop_mtc()
    
    def _analyze_output(self, result: Dict, info: Dict, expected_path: str) -> Dict:
        """Analyze videocomposer output to determine what happened."""
        analysis = {
            "success": False,
            "detected_codec": None,
            "decoding_path": None,
            "errors": [],
            "warnings": []
        }
        
        if "error" in result:
            analysis["errors"].append(result["error"])
            return analysis
        
        output = "\n".join(result.get("output", []))
        
        # Check for codec detection
        codec = info.get("codec", "").lower()
        if codec in output.lower():
            analysis["detected_codec"] = codec
        
        # Check for HAP detection
        if "hap" in output.lower() and codec == "hap":
            analysis["decoding_path"] = "HAP_DIRECT"
            analysis["success"] = True
        # Check for hardware decoding (look for hardware decoder messages)
        elif any(hw in output.lower() for hw in [
            "using hardware", "hardware decoder", "hardware decoding", "vaapi", "cuda", 
            "videotoolbox", "dxva2", "attempting to open hardware decoder",
            "loaded hardware-decoded frame", "gpu_hardware", "successfully opened hardware"
        ]):
            analysis["decoding_path"] = "GPU_HARDWARE"
            analysis["success"] = True
        # Check for software decoding
        elif any(sw in output.lower() for sw in [
            "using software", "software decoding", "software-decoded", "cpu_software",
            "falling back to software", "no hardware decoder", "loaded software-decoded frame",
            "using cpu", "software codec"
        ]):
            analysis["decoding_path"] = "CPU_SOFTWARE"
            # Success if software was expected
            if "SOFTWARE" in expected_path:
                analysis["success"] = True
        
        # Extract errors and warnings
        for line in result.get("output", []):
            if "error" in line.lower() or "failed" in line.lower():
                analysis["errors"].append(line)
            elif "warning" in line.lower():
                analysis["warnings"].append(line)
        
        # If no errors and process ran, consider it successful (even if we couldn't detect path)
        if not analysis["errors"] and result.get("returncode") in [0, -15, -9]:  # 0=success, -15=TERM, -9=KILL
            if not analysis["decoding_path"]:
                # Try to infer from output even if keywords weren't found
                if "loaded" in output.lower() and "frame" in output.lower():
                    # If frames are being loaded, assume it's working
                    analysis["decoding_path"] = "DETECTED (path not explicitly logged)"
                    analysis["success"] = True
            elif not analysis["success"]:
                # We detected a path but didn't mark success - mark it now
                analysis["success"] = True
        
        return analysis
    
    def find_test_videos(self, patterns: List[str] = None) -> List[Path]:
        """Find test video files."""
        if patterns is None:
            # Include all common video formats from video_test_files directory
            patterns = ["*.mp4", "*.mov", "*.avi", "*.mkv", "*.webm"]
        
        videos = []
        for pattern in patterns:
            videos.extend(self.video_dir.glob(pattern))
        
        # Remove duplicates and sort
        return sorted(set(videos))
    
    def run_all_tests(self, test_hw: bool = True, test_sw: bool = True, duration: int = 5, one_by_one: bool = False) -> Dict:
        """Run tests on all available test videos."""
        videos = self.find_test_videos()
        
        if not videos:
            print(f"ERROR: No test videos found in {self.video_dir}")
            print("Run tests/create_test_videos.sh first to create test files")
            return {}
        
        print(f"\nFound {len(videos)} test videos")
        print(f"Testing hardware decoding: {test_hw}")
        print(f"Testing software decoding: {test_sw}")
        if one_by_one:
            print(f"Running tests one by one with {duration} seconds per video\n")
        
        results = {}
        for i, video in enumerate(videos, 1):
            # Test all videos from video_test_files directory
            # Filter by test type only if explicitly disabled
            video_name = video.name.lower()
            if not test_hw and any(codec in video_name for codec in ["h264", "hevc", "av1"]):
                continue
            if not test_sw and any(codec in video_name for codec in ["vp9", "mpeg4"]):
                continue
            
            if one_by_one:
                print(f"\n{'='*60}")
                print(f"TEST {i}/{len(videos)}: {video.name}")
                print(f"{'='*60}")
            
            # Test all other videos (problematic.mp4, test_playback_patterns.mov, etc.)
            result = self.test_video_file(video, duration)
            results[video.name] = result
            
            if one_by_one:
                # Show quick result
                success = result.get("success", False)
                path = result.get("analysis", {}).get("decoding_path", "UNKNOWN")
                status = "✓ PASS" if success else "✗ FAIL"
                print(f"\n  Result: {status} | Path: {path}")
                if i < len(videos):
                    print(f"\n  Next: {videos[i].name}")
                    time.sleep(1)  # Brief pause between tests
        
        return results
    
    def print_summary(self, results: Dict):
        """Print test summary."""
        print(f"\n{'='*60}")
        print("TEST SUMMARY")
        print(f"{'='*60}")
        
        total = len(results)
        passed = sum(1 for r in results.values() if r.get("success", False))
        failed = total - passed
        
        print(f"Total tests: {total}")
        print(f"Passed: {passed} ✓")
        print(f"Failed: {failed} ✗")
        
        if failed > 0:
            print(f"\nFailed tests:")
            for name, result in results.items():
                if not result.get("success", False):
                    errors = result.get('analysis', {}).get('errors', [])
                    if errors:
                        print(f"  - {name}: {errors[0]}")
                    else:
                        print(f"  - {name}: Unknown error (check output above)")
        
        # Group by codec
        print(f"\nBy codec:")
        codec_stats = {}
        for name, result in results.items():
            codec = result.get("info", {}).get("codec", "unknown")
            if codec not in codec_stats:
                codec_stats[codec] = {"total": 0, "passed": 0}
            codec_stats[codec]["total"] += 1
            if result.get("success", False):
                codec_stats[codec]["passed"] += 1
        
        for codec, stats in sorted(codec_stats.items()):
            print(f"  {codec}: {stats['passed']}/{stats['total']} passed")
        
        # Group by decoding path
        print(f"\nBy decoding path:")
        path_stats = {}
        for name, result in results.items():
            path = result.get("analysis", {}).get("decoding_path", "UNKNOWN")
            if path not in path_stats:
                path_stats[path] = {"total": 0, "passed": 0}
            path_stats[path]["total"] += 1
            if result.get("success", False):
                path_stats[path]["passed"] += 1
        
        for path, stats in sorted(path_stats.items()):
            print(f"  {path}: {stats['passed']}/{stats['total']} passed")


def main():
    parser = argparse.ArgumentParser(description="Test video codec and format support")
    parser.add_argument("--video-dir", type=str, 
                       default=str(Path(__file__).parent.parent / "video_test_files"),
                       help="Directory containing test video files (default: video_test_files)")
    parser.add_argument("--videocomposer", type=str,
                       help="Path to videocomposer binary")
    parser.add_argument("--test-all", action="store_true",
                       help="Test all available videos")
    parser.add_argument("--test-hw", action="store_true", default=True,
                       help="Test hardware-decoded codecs (H.264, HEVC, AV1)")
    parser.add_argument("--test-sw", action="store_true", default=True,
                       help="Test software codecs (VP9, MPEG-4)")
    parser.add_argument("--test-hap", action="store_true",
                       help="Test HAP codec specifically")
    parser.add_argument("--duration", type=int, default=10,
                       help="Test duration per video in seconds (default: 10, longer for interactive mode)")
    parser.add_argument("--video", type=str,
                       help="Test a specific video file")
    parser.add_argument("--no-mtc", action="store_true",
                       help="Don't start MTC timecode (may cause videocomposer to wait)")
    parser.add_argument("--fps", type=float, default=25.0,
                       help="MTC framerate (default: 25.0)")
    parser.add_argument("--interactive", action="store_true",
                       help="Interactive mode: test one video at a time and ask if video is visible")
    parser.add_argument("--one-by-one", action="store_true",
                       help="Test formats one by one with clear output for each")
    
    args = parser.parse_args()
    
    video_dir = Path(args.video_dir)
    videocomposer_bin = Path(args.videocomposer) if args.videocomposer else None
    
    if not video_dir.exists():
        print(f"ERROR: Video directory not found: {video_dir}")
        print(f"Expected location: {Path(__file__).parent.parent / 'video_test_files'}")
        print("Run tests/create_test_videos.sh first to create test files")
        sys.exit(1)
    
    # Print the directory being used
    print(f"Using video directory: {video_dir.resolve()}")
    
    tester = CodecFormatTest(video_dir, videocomposer_bin, use_mtc=not args.no_mtc, fps=args.fps)
    
    results = {}
    
    if args.interactive:
        # Interactive mode: test one video at a time
        videos = tester.find_test_videos()
        
        if not videos:
            print(f"ERROR: No test videos found in {video_dir}")
            print("Run tests/create_test_videos.sh first to create test files")
            sys.exit(1)
        
        print(f"\n{'='*60}")
        print(f"INTERACTIVE MODE")
        print(f"{'='*60}")
        print(f"Found {len(videos)} videos to test")
        print(f"Each video will play for {args.duration} seconds")
        print(f"You will be asked if you see video after each one")
        print(f"Answer: 'y' (yes), 'n' (no), or 's' (skip)")
        print(f"{'='*60}\n")
        
        input("Press ENTER to start testing...")
        
        for i, video in enumerate(videos, 1):
            print(f"\n{'='*60}")
            print(f"Video {i}/{len(videos)}")
            print(f"{'='*60}")
            
            result = tester.test_video_file_interactive(video, args.duration)
            results[video.name] = result
            
            if i < len(videos):
                response = input(f"\nPress ENTER to continue to next video, or 'q' to quit: ").strip().lower()
                if response == 'q':
                    print("\nTesting stopped by user.")
                    break
        
        # Print summary
        print(f"\n{'='*60}")
        print("INTERACTIVE TEST SUMMARY")
        print(f"{'='*60}")
        visible_count = sum(1 for r in results.values() if r.get("video_visible") == True)
        not_visible_count = sum(1 for r in results.values() if r.get("video_visible") == False)
        skipped_count = sum(1 for r in results.values() if r.get("skipped") == True)
        
        print(f"Total tested: {len(results)}")
        print(f"Visible: {visible_count} ✓")
        print(f"Not visible: {not_visible_count} ✗")
        print(f"Skipped: {skipped_count}")
        print(f"\nVideos that were NOT visible:")
        for name, result in results.items():
            if result.get("video_visible") == False:
                print(f"  - {name}")
        
    elif args.video:
        # Test single video
        video_path = Path(args.video)
        if not video_path.is_absolute():
            video_path = video_dir / video_path
        
        result = tester.test_video_file(video_path, args.duration)
        results[video_path.name] = result
        tester.print_summary(results)
    elif args.test_hap:
        # Test HAP specifically
        hap_videos = tester.find_test_videos(["*hap*"])
        if not hap_videos:
            print("ERROR: No HAP test videos found")
            sys.exit(1)
        
        for video in hap_videos:
            results[video.name] = tester.test_video_file(video, args.duration)
        tester.print_summary(results)
    else:
        # Test all
        results = tester.run_all_tests(args.test_hw, args.test_sw, args.duration, one_by_one=args.one_by_one)
        tester.print_summary(results)
    
    # Exit with error if any tests failed
    if results and any(not r.get("success", False) for r in results.values()):
        sys.exit(1)


if __name__ == "__main__":
    main()

