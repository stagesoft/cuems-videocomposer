#!/usr/bin/env python3
"""
NDI Input Testing Script

Tests NDI input functionality in cuems-videocomposer:
1. Discovers NDI sources
2. Connects to an NDI source
3. Verifies video frames are received
4. Monitors statistics and performance

Usage:
    python3 tests/test_ndi_input.py [options]
"""

import sys
import os
import time
import subprocess
import signal
import argparse
import re
from pathlib import Path
from typing import Optional, List, Dict

# Import shared MTC helper if available
try:
    from mtc_helper import MTCHelper, MTC_AVAILABLE
except ImportError:
    MTC_AVAILABLE = False
    MTCHelper = None

class NDITest:
    def __init__(self, videocomposer_bin: Optional[Path] = None, 
                 source: Optional[str] = None,
                 duration: int = 30,
                 verbose: bool = False,
                 use_mtc: bool = False,
                 use_osc: bool = False):
        self.videocomposer_bin = videocomposer_bin or self._find_videocomposer()
        self.source = source
        self.duration = duration
        self.verbose = verbose
        self.use_mtc = use_mtc and MTC_AVAILABLE
        self.use_osc = use_osc
        self.process = None
        self.mtc_helper = None
        self.stats = {
            'frames_captured': 0,
            'frames_dropped': 0,
            'capture_errors': 0,
            'connection_time': None,
            'first_frame_time': None,
        }
        
        # Set up signal handler
        signal.signal(signal.SIGINT, self._signal_handler)
    
    def _find_videocomposer(self) -> Path:
        """Find videocomposer executable."""
        possible_paths = [
            Path(__file__).parent.parent / "build" / "cuems-videocomposer",
            Path("/usr/bin/cuems-videocomposer"),
            Path("/usr/local/bin/cuems-videocomposer"),
        ]
        
        for path in possible_paths:
            if path.exists() and os.access(path, os.X_OK):
                return path
        
        raise FileNotFoundError(
            "videocomposer not found. Build the application first.\n"
            f"  Checked: {[str(p) for p in possible_paths]}"
        )
    
    def _signal_handler(self, signum, frame):
        """Handle Ctrl-C gracefully."""
        print("\n\nInterrupted by user. Cleaning up...")
        self.cleanup()
        sys.exit(1)
    
    def discover_sources(self) -> List[str]:
        """Discover available NDI sources using videocomposer."""
        print("Discovering NDI sources...")
        
        # Use test_ndi_discovery.py functionality
        try:
            from test_ndi_discovery import discover_ndi_sources
            sources = discover_ndi_sources(timeout_seconds=5)
            return sources
        except ImportError:
            # Fallback: call videocomposer directly
            try:
                import subprocess
                result = subprocess.run(
                    [str(self.videocomposer_bin), "--discover-ndi", "5"],
                    capture_output=True,
                    text=True,
                    timeout=10
                )
                
                sources = []
                for line in result.stdout.split('\n'):
                    line = line.strip()
                    if line and line[0].isdigit() and '. ' in line:
                        source = line.split('. ', 1)[1]
                        sources.append(source)
                
                return sources
            except Exception as e:
                print(f"ERROR: Failed to discover sources: {e}")
                return []
    
    def run(self) -> bool:
        """Run the NDI input test."""
        if not self.source:
            print("ERROR: No NDI source specified.")
            print("Use --source 'SOURCE-NAME' or --auto to use first available source")
            return False
        
        print("=" * 60)
        print("NDI Input Test")
        print("=" * 60)
        print(f"Source: {self.source}")
        print(f"Duration: {self.duration} seconds")
        print(f"Videocomposer: {self.videocomposer_bin}")
        print()
        
        # Start MTC if requested
        if self.use_mtc:
            print("Starting MTC timecode...")
            self.mtc_helper = MTCHelper(fps=25.0, port=0, portname="NDITest")
            self.mtc_helper.start()
            time.sleep(0.5)
        
        # Build command
        cmd = [str(self.videocomposer_bin)]
        
        # Add NDI source
        ndi_source = self.source
        if not ndi_source.startswith("ndi://"):
            ndi_source = f"ndi://{ndi_source}"
        
        cmd.extend(["--layer", "1", "--file", ndi_source])
        
        if self.verbose:
            cmd.append("--verbose")
        
        print(f"Starting videocomposer: {' '.join(cmd)}")
        print()
        
        # Start process
        try:
            self.process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1
            )
            
            start_time = time.time()
            self.stats['connection_time'] = start_time
            
            # Monitor output
            print("Monitoring videocomposer output...")
            print("-" * 60)
            
            for line in self.process.stdout:
                if self.verbose:
                    print(line.rstrip())
                
                # Parse NDI-specific messages
                if "NDI:" in line or "ndi" in line.lower():
                    print(f"  {line.rstrip()}")
                    
                    # Extract statistics
                    if "frames" in line.lower() and "dropped" in line.lower():
                        # Parse: "NDI: 1000 frames, 2 dropped, avg capture: 33.3ms"
                        match = re.search(r'(\d+)\s+frames', line)
                        if match:
                            self.stats['frames_captured'] = int(match.group(1))
                        
                        match = re.search(r'(\d+)\s+dropped', line)
                        if match:
                            self.stats['frames_dropped'] = int(match.group(1))
                    
                    if "connected" in line.lower():
                        self.stats['connection_time'] = time.time()
                        print(f"  ✓ Connected to NDI source")
                    
                    if "format:" in line.lower() or "resolution" in line.lower():
                        print(f"  ✓ Format detected")
                        if not self.stats['first_frame_time']:
                            self.stats['first_frame_time'] = time.time()
                
                # Check for errors
                if "error" in line.lower() or "failed" in line.lower():
                    if "ndi" in line.lower():
                        print(f"  ✗ ERROR: {line.rstrip()}")
                        self.stats['capture_errors'] += 1
                
                # Check if process died
                if self.process.poll() is not None:
                    break
                
                # Check timeout
                if time.time() - start_time >= self.duration:
                    print(f"\nTest duration ({self.duration}s) reached.")
                    break
            
            # Wait for process to finish
            return_code = self.process.wait(timeout=5)
            
            if return_code != 0:
                print(f"\n✗ videocomposer exited with code {return_code}")
                return False
            
            print("\n" + "=" * 60)
            print("Test Results")
            print("=" * 60)
            self._print_stats()
            
            return True
            
        except subprocess.TimeoutExpired:
            print("\n✗ Process did not terminate in time")
            return False
        except Exception as e:
            print(f"\n✗ Error: {e}")
            return False
        finally:
            self.cleanup()
    
    def _print_stats(self):
        """Print test statistics."""
        print(f"Connection time: {self.stats['connection_time']:.2f}s")
        if self.stats['first_frame_time']:
            latency = self.stats['first_frame_time'] - self.stats['connection_time']
            print(f"First frame latency: {latency*1000:.1f}ms")
        print(f"Frames captured: {self.stats['frames_captured']}")
        print(f"Frames dropped: {self.stats['frames_dropped']}")
        print(f"Capture errors: {self.stats['capture_errors']}")
        
        if self.stats['frames_captured'] > 0:
            drop_rate = (self.stats['frames_dropped'] / self.stats['frames_captured']) * 100
            print(f"Drop rate: {drop_rate:.2f}%")
        
        # Overall assessment
        print()
        if self.stats['frames_captured'] > 0 and self.stats['capture_errors'] == 0:
            print("✓ Test PASSED: NDI input working correctly")
        elif self.stats['frames_captured'] > 0:
            print("⚠ Test PARTIAL: Frames received but with errors")
        else:
            print("✗ Test FAILED: No frames received")
    
    def cleanup(self):
        """Clean up resources."""
        if self.process:
            try:
                self.process.terminate()
                self.process.wait(timeout=2)
            except:
                try:
                    self.process.kill()
                except:
                    pass
        
        if self.mtc_helper:
            self.mtc_helper.stop()

def main():
    parser = argparse.ArgumentParser(
        description="Test NDI input functionality",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Discover sources
  python3 tests/test_ndi_input.py --discover

  # Test with specific source
  python3 tests/test_ndi_input.py --source "DESKTOP-ABC (NDI Test Patterns)" --duration 30

  # Test with auto-discovery (uses first available)
  python3 tests/test_ndi_input.py --auto --duration 30

  # Verbose output for debugging
  python3 tests/test_ndi_input.py --source "YOUR-SOURCE" --verbose

  # Test with MTC sync
  python3 tests/test_ndi_input.py --source "YOUR-SOURCE" --mtc
        """
    )
    
    parser.add_argument(
        '--source',
        type=str,
        help='NDI source name (e.g., "DESKTOP-ABC (NDI Test Patterns)")'
    )
    
    parser.add_argument(
        '--auto',
        action='store_true',
        help='Automatically use first available NDI source'
    )
    
    parser.add_argument(
        '--discover',
        action='store_true',
        help='Only discover and list NDI sources, then exit'
    )
    
    parser.add_argument(
        '--duration',
        type=int,
        default=30,
        help='Test duration in seconds (default: 30)'
    )
    
    parser.add_argument(
        '--verbose',
        action='store_true',
        help='Verbose output (show all videocomposer logs)'
    )
    
    parser.add_argument(
        '--mtc',
        action='store_true',
        help='Enable MTC timecode sync'
    )
    
    parser.add_argument(
        '--osc',
        action='store_true',
        help='Enable OSC control testing'
    )
    
    parser.add_argument(
        '--videocomposer',
        type=str,
        help='Path to videocomposer executable (default: auto-detect)'
    )
    
    args = parser.parse_args()
    
    # Handle discover mode
    if args.discover:
        test = NDITest()
        sources = test.discover_sources()
        if sources:
            print(f"\nFound {len(sources)} source(s):")
            for source in sources:
                print(f"  - {source}")
        else:
            print("\nNo sources found or discovery not implemented.")
            print("Use NDI Test Patterns or check your NDI source application.")
        return 0
    
    # Determine source
    source = args.source
    if args.auto:
        test = NDITest()
        sources = test.discover_sources()
        if sources:
            source = sources[0]
            print(f"Auto-selected source: {source}")
        else:
            print("ERROR: No sources found for auto-selection")
            return 1
    
    if not source:
        print("ERROR: Must specify --source, --auto, or --discover")
        parser.print_help()
        return 1
    
    # Run test
    videocomposer_bin = Path(args.videocomposer) if args.videocomposer else None
    test = NDITest(
        videocomposer_bin=videocomposer_bin,
        source=source,
        duration=args.duration,
        verbose=args.verbose,
        use_mtc=args.mtc,
        use_osc=args.osc
    )
    
    success = test.run()
    return 0 if success else 1

if __name__ == "__main__":
    sys.exit(main())

