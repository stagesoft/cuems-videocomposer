#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
#
# Per-boot capture script for the cold-boot mode-verification campaign.
# Run once after each cold boot of the test node.
#
# Captures:
#   - journalctl -u cuems-videocomposer for the current boot
#   - /sys/kernel/debug/dri/0/i915_display_info at T+5s after service start
#     (debugfs is root-only — script uses sudo if not run as root)
#   - Operator's PASS/FAIL classification of the panel photo (see runbook)
#
# Appends one row to tests/cold-boot-results.csv with a timestamp, boot id,
# the operator verdict, and where the journal/debugfs captures landed on disk.
#
# Prerequisites:
#   - Binary built with -DCUEMS_PROBE_SPLASH=ON (see tests/COLD_BOOT_PROBE.md)
#   - cuems-videocomposer.service is enabled and started by the system
#   - Operator has just observed and photographed the splash window

set -u  # do not use -e: we want to record FAIL even on tooling errors

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_CSV="${SCRIPT_DIR}/cold-boot-results.csv"
CAPTURE_DIR="${SCRIPT_DIR}/cold-boot-captures"
mkdir -p "${CAPTURE_DIR}"

if [ ! -f "${RESULTS_CSV}" ]; then
    echo "timestamp,boot_id,verdict,journal_path,i915_path,notes" > "${RESULTS_CSV}"
fi

TS="$(date -u +%Y%m%dT%H%M%SZ)"
BOOT_ID="$(journalctl --list-boots -n 1 --no-pager 2>/dev/null | awk 'END {print $2}')"
[ -z "${BOOT_ID}" ] && BOOT_ID="unknown"

JOURNAL_OUT="${CAPTURE_DIR}/${TS}_journal.txt"
I915_OUT="${CAPTURE_DIR}/${TS}_i915_display_info.txt"

echo "==> Capturing journal for cuems-videocomposer this boot..."
journalctl -b 0 -u cuems-videocomposer --no-pager > "${JOURNAL_OUT}" 2>&1
echo "    -> ${JOURNAL_OUT}"

echo "==> Capturing /sys/kernel/debug/dri/0/i915_display_info..."
if [ "$(id -u)" -eq 0 ]; then
    cat /sys/kernel/debug/dri/0/i915_display_info > "${I915_OUT}" 2>&1
else
    sudo cat /sys/kernel/debug/dri/0/i915_display_info > "${I915_OUT}" 2>&1
fi
echo "    -> ${I915_OUT}"

# Quick journal triage — surface the most informative lines
echo
echo "==> Journal triage (verifier + retry markers):"
grep -E "verifyCrtcMode|cold-boot retry|Modeset|drmSetMaster|fatalModeset|Restart" \
     "${JOURNAL_OUT}" | tail -40 || true
echo

# Operator verdict prompt
cat <<EOF
==> PANEL VERDICT
Look at the photograph you just took of the panel during the 10 s splash window.

  PASS = red border at the panel EDGES, all four colored corners
         (cyan TL, magenta TR, yellow BL, green BR) at the panel corners,
         "1920 x 1080" centered on the full panel.

  FAIL = red border at the 1/4 mark (far from the panel edges),
         all four corners clustered in the top-left quadrant of the panel,
         rest of the panel showing fbcon black/garbage.

EOF

read -p "Verdict for this boot [PASS/FAIL/SKIP]: " VERDICT
VERDICT="$(echo "${VERDICT}" | tr '[:lower:]' '[:upper:]' | tr -d ' ')"
case "${VERDICT}" in
    PASS|FAIL|SKIP) ;;
    *) echo "Unrecognized verdict — recording as SKIP"; VERDICT="SKIP" ;;
esac

read -p "Optional note (free text, no commas): " NOTES
NOTES="$(echo "${NOTES}" | tr -d ',')"

echo "${TS},${BOOT_ID},${VERDICT},${JOURNAL_OUT},${I915_OUT},${NOTES}" >> "${RESULTS_CSV}"
echo
echo "==> Recorded in ${RESULTS_CSV}"
echo

# Quick running tally
echo "==> Running tally:"
awk -F, 'NR>1 {tot++; v[$3]++} END {
    for (k in v) printf "    %-6s %d\n", k, v[k]
    printf "    TOTAL  %d\n", tot
}' "${RESULTS_CSV}"
