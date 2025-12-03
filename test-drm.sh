#!/bin/bash
#
# DRM testing script - stops X, runs test, logs everything, restarts X
# Usage: sudo ./test-drm.sh [videocomposer args]
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_FILE="/tmp/cuems-drm-test-$(date +%Y%m%d-%H%M%S).log"
VIDEOCOMPOSER="$SCRIPT_DIR/build/cuems-videocomposer"

# NDI library setup
NDI_LIB_PATH="/opt/NDI SDK for Linux/lib/x86_64-linux-gnu"
if [ -d "$NDI_LIB_PATH" ]; then
    export LD_LIBRARY_PATH="$NDI_LIB_PATH:$LD_LIBRARY_PATH"
fi

# Detect display manager
DM=""
if systemctl is-active --quiet gdm; then
    DM="gdm"
elif systemctl is-active --quiet gdm3; then
    DM="gdm3"
elif systemctl is-active --quiet lightdm; then
    DM="lightdm"
elif systemctl is-active --quiet sddm; then
    DM="sddm"
fi

echo "=== CUeMS DRM Test ===" | tee "$LOG_FILE"
echo "Log file: $LOG_FILE" | tee -a "$LOG_FILE"
echo "Display manager: ${DM:-none detected}" | tee -a "$LOG_FILE"
echo "Args: $@" | tee -a "$LOG_FILE"
echo "" | tee -a "$LOG_FILE"

if [ ! -f "$VIDEOCOMPOSER" ]; then
    echo "ERROR: $VIDEOCOMPOSER not found" | tee -a "$LOG_FILE"
    exit 1
fi

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "ERROR: Must run as root (sudo)" | tee -a "$LOG_FILE"
    exit 1
fi

# Stop display manager
if [ -n "$DM" ]; then
    echo "Stopping $DM..." | tee -a "$LOG_FILE"
    systemctl stop "$DM"
    sleep 2
fi

# Run the test
echo "Running videocomposer..." | tee -a "$LOG_FILE"
echo "----------------------------------------" | tee -a "$LOG_FILE"

# Run with timeout (30 seconds default, or until Ctrl+C)
timeout --signal=INT 30s "$VIDEOCOMPOSER" "$@" 2>&1 | tee -a "$LOG_FILE" || true

echo "----------------------------------------" | tee -a "$LOG_FILE"
echo "Test completed." | tee -a "$LOG_FILE"

# Restart display manager
if [ -n "$DM" ]; then
    echo "Restarting $DM..." | tee -a "$LOG_FILE"
    systemctl start "$DM"
fi

echo ""
echo "=== Test finished ==="
echo "Full log saved to: $LOG_FILE"
echo ""
echo "View log with: cat $LOG_FILE"

