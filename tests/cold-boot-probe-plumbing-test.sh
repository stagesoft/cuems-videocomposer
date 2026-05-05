#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Stagelab Coop SCCL
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileContributor: Ion Reguera <ion@stagelab.coop>
#
# Plumbing test for the cold-boot verify -> retry -> fatal -> systemd-restart
# chain. Runs cuems-videocomposer in the foreground as the `cuems` user with
# CUEMS_VC_FORCE_VERIFY_FAIL=<N> set, so the verifier returns false on the
# next N calls without needing the actual i915 cold-boot race to fire.
#
# Usage:
#   sudo systemctl stop cuems-videocomposer.service
#   tests/cold-boot-probe-plumbing-test.sh 3   # retry-success path (3 outputs)
#   tests/cold-boot-probe-plumbing-test.sh 6   # fatal-exit path  (3 outputs)
#   sudo systemctl start cuems-videocomposer.service
#
# With N=3:  expect three "CUEMS_VC_FORCE_VERIFY_FAIL — forcing first-frame
#            verify failure" lines and three "cold-boot retry succeeded"
#            lines, then normal operation. Press Ctrl-C to stop.
# With N=6:  expect three forced first-frame failures + three forced retry
#            failures + three "marking fatal" lines + the run loop exits.
#            The script will report the process's non-zero exit code.

set -u

if [ $# -lt 1 ]; then
    echo "Usage: $0 <N>" >&2
    echo "  N=3 → tests retry-success path on a 3-output config" >&2
    echo "  N=6 → tests fatal-exit path on a 3-output config" >&2
    exit 64
fi
N="$1"

if ! [[ "${N}" =~ ^[0-9]+$ ]]; then
    echo "ERROR: N must be a non-negative integer (got '${N}')" >&2
    exit 64
fi

if systemctl is-active --quiet cuems-videocomposer.service; then
    cat <<EOF >&2
ERROR: cuems-videocomposer.service is currently active.
Stop it first so this manual run can claim the seat / DRM master:

    sudo systemctl stop cuems-videocomposer.service

(restart it after the test with: sudo systemctl start cuems-videocomposer.service)
EOF
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CAPTURE_DIR="${SCRIPT_DIR}/cold-boot-captures"
mkdir -p "${CAPTURE_DIR}"
TS="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="${CAPTURE_DIR}/${TS}_plumbing_N${N}.txt"

echo "==> Running /usr/bin/cuems-videocomposer as cuems with CUEMS_VC_FORCE_VERIFY_FAIL=${N}"
echo "==> Output: ${OUT}"
echo "==> (Ctrl-C to stop. With N=6 the process exits non-zero on its own.)"
echo

# Pull the production env file so the binary sees the same OPERATOR_FLAGS /
# OUTPUT_LATENCY_FLAG it would under systemd, plus our debug knob.
ENV_PROD="/etc/cuems/videocomposer-flags.env"
[ -r "${ENV_PROD}" ] && source "${ENV_PROD}" || true
: "${OPERATOR_FLAGS:=}"
: "${OUTPUT_LATENCY_FLAG:=}"

# Need DRM access — the cuems user owns the seat. Use sudo -u cuems but
# preserve the env var explicitly (sudo strips most env by default).
sudo -E -u cuems env \
    CUEMS_VC_FORCE_VERIFY_FAIL="${N}" \
    OPERATOR_FLAGS="${OPERATOR_FLAGS}" \
    OUTPUT_LATENCY_FLAG="${OUTPUT_LATENCY_FLAG}" \
    /usr/bin/cuems-videocomposer ${OPERATOR_FLAGS} ${OUTPUT_LATENCY_FLAG} \
    2>&1 | tee "${OUT}"
EXIT=${PIPESTATUS[0]}

echo
echo "==> Process exited with code ${EXIT}"
echo

# Quick triage — surface the markers
echo "==> Verifier / retry / fatal markers:"
grep -E "FORCE_VERIFY_FAIL|verifyCrtcMode|cold-boot retry|fatalModeset|fatal error" "${OUT}" | tail -40 || true
echo
echo "==> Full output saved to: ${OUT}"

# Verdict heuristic for N=6 (fatal): expect exit code 1 + 'marking fatal' lines
if [ "${N}" = "6" ]; then
    echo
    if [ "${EXIT}" -eq 1 ] && grep -q "marking fatal for systemd restart" "${OUT}"; then
        echo "==> N=6 PASS: process exited 1 and 'marking fatal' was logged."
    else
        echo "==> N=6 UNEXPECTED: exit=${EXIT}, missing 'marking fatal' marker."
    fi
fi
