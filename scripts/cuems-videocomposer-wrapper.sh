#!/bin/bash
#
# Alternative wrapper script location
# This can be installed to /usr/local/bin/ for system-wide use
#

# Source the main wrapper script if it exists
if [ -f "$(dirname "$0")/../cuems-videocomposer.sh" ]; then
    exec "$(dirname "$0")/../cuems-videocomposer.sh" "$@"
else
    # Fallback: try to find it
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    if [ -f "$SCRIPT_DIR/cuems-videocomposer.sh" ]; then
        exec "$SCRIPT_DIR/cuems-videocomposer.sh" "$@"
    else
        echo "ERROR: cuems-videocomposer.sh not found" >&2
        exit 1
    fi
fi

