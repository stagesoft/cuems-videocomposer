#!/usr/bin/env python3
"""
NDI Source Discovery Test

Discovers and lists available NDI sources on the network.
This is useful for finding NDI source names to use with videocomposer.

Usage:
    python3 tests/test_ndi_discovery.py [--timeout SECONDS]
"""

import sys
import os
import time
import subprocess
import argparse
from pathlib import Path

# Add parent directory to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))

def discover_ndi_sources(timeout_seconds=5):
    """
    Discover NDI sources using videocomposer's --discover-ndi flag.
    """
    videocomposer_bin = Path(__file__).parent.parent / "build" / "cuems-videocomposer"
    
    # Try system path if build directory doesn't exist
    if not videocomposer_bin.exists():
        import shutil
        bin_path = shutil.which("cuems-videocomposer")
        if bin_path:
            videocomposer_bin = Path(bin_path)
        else:
            print("ERROR: videocomposer not found. Build the application first.")
            print(f"  Expected at: {videocomposer_bin}")
            return []
    
    try:
        # Call videocomposer with --discover-ndi flag
        result = subprocess.run(
            [str(videocomposer_bin), "--discover-ndi", str(timeout_seconds)],
            capture_output=True,
            text=True,
            timeout=timeout_seconds + 5
        )
        
        # Parse output
        sources = []
        in_sources_list = False
        
        for line in result.stdout.split('\n'):
            line = line.strip()
            if not line:
                continue
            
            # Look for numbered list items
            if line[0].isdigit() and '. ' in line:
                # Extract source name (everything after "N. ")
                source = line.split('. ', 1)[1] if '. ' in line else line
                sources.append(source)
                in_sources_list = True
            elif in_sources_list and line.startswith('-'):
                # Alternative format: "- Source Name"
                source = line[1:].strip()
                sources.append(source)
        
        return sources
        
    except subprocess.TimeoutExpired:
        print("ERROR: Discovery timed out")
        return []
    except FileNotFoundError:
        print(f"ERROR: videocomposer not found at {videocomposer_bin}")
        return []
    except Exception as e:
        print(f"ERROR: {e}")
        return []

def main():
    parser = argparse.ArgumentParser(
        description="Discover NDI sources on the network",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Discover sources with default timeout (5 seconds)
  python3 tests/test_ndi_discovery.py

  # Discover with longer timeout
  python3 tests/test_ndi_discovery.py --timeout 10
        """
    )
    
    parser.add_argument(
        '--timeout',
        type=int,
        default=5,
        help='Discovery timeout in seconds (default: 5)'
    )
    
    args = parser.parse_args()
    
    print("NDI Source Discovery")
    print("=" * 50)
    print(f"Timeout: {args.timeout} seconds")
    print("Searching for NDI sources...")
    print()
    
    sources = discover_ndi_sources(args.timeout)
    
    if sources:
        print(f"Found {len(sources)} NDI source(s):")
        print()
        for i, source in enumerate(sources, 1):
            print(f"  {i}. {source}")
        print()
        print("To use with videocomposer:")
        print(f'  ./build/cuems-videocomposer --layer 1 --file "ndi://{sources[0]}"')
    else:
        print("No NDI sources found.")
        print()
        print("Troubleshooting:")
        print("  1. Ensure an NDI source is running (NDI Test Patterns, OBS, etc.)")
        print("  2. Check network connectivity")
        print("  3. Verify firewall allows NDI (UDP ports 5960-5969)")
        print("  4. Try increasing timeout: --timeout 10")
    
    return 0 if sources else 1

if __name__ == "__main__":
    sys.exit(main())

