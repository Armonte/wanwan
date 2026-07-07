#!/usr/bin/env bash
# ci_determinism_gate.sh — standing determinism gate for the rollback savestate.
#
# Proves the savestate serializes COMPLETELY: records a stress-autoplay match
# under forced rollback (FM2K_CHECK_DISTANCE), replays it from the input log,
# and diffs per-frame engine state. Any persistent divergence => non-zero exit
# so CI (or a pre-release check) fails loudly. This is the regression guard for
# the frame-764 / OBJ_SLOT_COUNT class of rollback-completeness bugs.
#
# Runs BOTH modes so a green result means sim determinism AND catch-up
# correctness:
#   - catchup : replay fast-forwards (FM2K_SPECTATOR_ALWAYS_CATCHUP) — the path
#               a live spectator uses to catch up to the live edge.
#   - 1to1    : replay renders every frame at 1:1 (SELFTEST_NO_CATCHUP=1) —
#               isolates pure sim determinism from the catch-up path.
#
# Usage:
#   tools/ci_determinism_gate.sh                 # frames=1500, check_distance=10
#   FRAMES=3000 FM2K_CHECK_DISTANCE=12 tools/ci_determinism_gate.sh
#
# Exit 0 = GREEN (both modes CLEAN). Exit 1 = RED (a divergence survived).
set -uo pipefail

FRAMES="${FRAMES:-1500}"
CD="${FM2K_CHECK_DISTANCE:-10}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail=0

for mode in catchup 1to1; do
    log="$(mktemp)"
    if [ "$mode" = "1to1" ]; then extra_env="SELFTEST_NO_CATCHUP=1"; else extra_env=""; fi
    echo "[gate] mode=$mode  check_distance=$CD  frames=$FRAMES"
    # shellcheck disable=SC2086
    env $extra_env FM2K_CHECK_DISTANCE="$CD" \
        python3 "$ROOT/tools/replay_selftest.py" --frames "$FRAMES" > "$log" 2>&1
    rc=$?
    if [ $rc -eq 0 ] && grep -q "CLEAN: all" "$log"; then
        echo "[gate] mode=$mode: PASS ($(grep -m1 'CLEAN: all' "$log"))"
    else
        echo "[gate] mode=$mode: FAIL (rc=$rc) -----------------------------"
        # surface the divergence line(s) + tail for triage
        grep -nE "DIVERGE|divergen|MISMATCH|first divergence|frame [0-9]+" "$log" | head -8
        tail -15 "$log"
        fail=1
    fi
    rm -f "$log"
done

if [ $fail -eq 0 ]; then
    echo "[gate] ===================================="
    echo "[gate] DETERMINISM GATE: GREEN — savestate serializes completely"
    exit 0
else
    echo "[gate] ===================================="
    echo "[gate] DETERMINISM GATE: RED — a rollback divergence survived"
    echo "[gate] Bisect with: FM2K_CHECK_DISTANCE=$CD python3 tools/replay_selftest.py --frames $FRAMES --keep"
    echo "[gate] then tools/parity_diff.py on the kept .pty pair to localize the field/slot."
    exit 1
fi
