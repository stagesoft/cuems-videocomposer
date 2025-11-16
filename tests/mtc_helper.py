#!/usr/bin/env python3
"""
Shared MTC (MIDI Time Code) helper module for test scripts.

This module provides a common interface for starting and managing MTC timecode
in test scripts, avoiding code duplication.
"""

import sys
import os
from pathlib import Path
from typing import Optional

# Add libmtcmaster python path
libmtcmaster_python = Path(__file__).parent.parent.parent / "libmtcmaster" / "python"
libmtcmaster_lib = Path(__file__).parent.parent.parent / "libmtcmaster" / "libmtcmaster.so"
sys.path.insert(0, str(libmtcmaster_python))

# Import mtcsender and patch it to use the correct library path
original_cwd = os.getcwd()
MTC_AVAILABLE = False
MtcSender = None

try:
    os.chdir(libmtcmaster_python)
    import mtcsender
    # Patch the library path to use absolute path
    original_init = mtcsender.MtcSender.__init__
    def patched_init(self, fps=25, port=0, portname="SLMTCPort"):
        import ctypes
        # Use absolute path to library
        if not libmtcmaster_lib.exists():
            raise FileNotFoundError(f"libmtcmaster.so not found at {libmtcmaster_lib}")
        self.mtc_lib = ctypes.CDLL(str(libmtcmaster_lib))
        self.mtc_lib.MTCSender_create.restype = ctypes.c_void_p
        self.mtcproc = self.mtc_lib.MTCSender_create()
        self.port = port
        self.char_portname = portname.encode('utf-8')
        self.mtc_lib.MTCSender_openPort.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p]
        self.mtc_lib.MTCSender_openPort(self.mtcproc, self.port, self.char_portname)
        self.fps = fps
    mtcsender.MtcSender.__init__ = patched_init
    MtcSender = mtcsender.MtcSender
    MTC_AVAILABLE = True
except (ImportError, FileNotFoundError) as e:
    MTC_AVAILABLE = False
    MtcSender = None
finally:
    os.chdir(original_cwd)


class MTCHelper:
    """Helper class for managing MTC timecode in tests."""
    
    def __init__(self, fps: float = 25.0, port: int = 0, portname: str = "TestPort"):
        """
        Initialize MTC helper.
        
        Args:
            fps: MTC framerate (default: 25.0)
            port: ALSA MIDI port number (default: 0)
            portname: MIDI port name (default: "TestPort")
        """
        self.fps = fps
        self.port = port
        self.portname = portname
        self.mtc_sender: Optional[MtcSender] = None
        self.available = MTC_AVAILABLE
    
    def is_available(self) -> bool:
        """Check if MTC is available."""
        return self.available
    
    def setup(self) -> bool:
        """
        Setup MTC timecode generation (but don't start playing).
        
        Returns:
            True if MTC was set up successfully, False otherwise
        """
        if not self.available:
            return False
        
        try:
            self.mtc_sender = MtcSender(fps=self.fps, port=self.port, portname=self.portname)
            print(f"DEBUG: MTC sender created (fps={self.fps}, port={self.port})")
            self.mtc_sender.settime_frames(0)
            print("DEBUG: MTC time set to frame 0 in setup()")
            # Don't call play() here - let start() handle it to avoid double-play toggle
            return True
        except Exception as e:
            print(f"WARNING: Failed to setup MTC: {e}")
            return False
    
    def start(self, start_frame: int = 0) -> bool:
        """
        Start MTC playback from a specific frame.
        
        Args:
            start_frame: Frame number to start from (default: 0)
        
        Returns:
            True if started successfully, False otherwise
        """
        if not self.mtc_sender:
            if not self.setup():
                return False
        
        try:
            self.mtc_sender.settime_frames(start_frame)
            print(f"DEBUG: MTC time set to frame {start_frame}")
            self.mtc_sender.play()
            print("DEBUG: MTC play() called - MTC should now be rolling")
            return True
        except Exception as e:
            print(f"WARNING: Failed to start MTC playback: {e}")
            return False
    
    def seek(self, hours: int = 0, minutes: int = 0, seconds: int = 0, frames: int = 0) -> bool:
        """
        Seek MTC to a specific time position using full frame messages.
        
        Args:
            hours: Hours
            minutes: Minutes
            seconds: Seconds
            frames: Frames
        
        Returns:
            True if seek was successful, False otherwise
        """
        if not self.mtc_sender:
            return False
        
        try:
            # Calculate total frames
            total_frames = (hours * 3600 + minutes * 60 + seconds) * int(self.fps) + frames
            self.mtc_sender.settime_frames(total_frames)
            return True
        except Exception as e:
            print(f"WARNING: Failed to seek MTC: {e}")
            return False
    
    def stop(self) -> bool:
        """
        Stop MTC playback.
        
        Returns:
            True if stopped successfully, False otherwise
        """
        if not self.mtc_sender:
            return False
        
        try:
            self.mtc_sender.stop()
            return True
        except Exception as e:
            print(f"WARNING: Failed to stop MTC: {e}")
            return False
    
    def cleanup(self):
        """Cleanup MTC resources."""
        if self.mtc_sender:
            try:
                self.stop()
            except:
                pass
            self.mtc_sender = None
    
    def __enter__(self):
        """Context manager entry."""
        self.setup()
        return self
    
    def __exit__(self, exc_type, exc_val, exc_tb):
        """Context manager exit."""
        self.cleanup()


def create_mtc_helper(fps: float = 25.0, port: int = 0, portname: str = "TestPort") -> Optional[MTCHelper]:
    """
    Create an MTC helper instance.
    
    Args:
        fps: MTC framerate (default: 25.0)
        port: ALSA MIDI port number (default: 0)
        portname: MIDI port name (default: "TestPort")
    
    Returns:
        MTCHelper instance if MTC is available, None otherwise
    """
    if not MTC_AVAILABLE:
        return None
    return MTCHelper(fps=fps, port=port, portname=portname)

