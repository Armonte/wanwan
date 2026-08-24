#!/usr/bin/env bash
# seam_hunt_loop.sh -- repeat the seam gate and ARCHIVE every failing run.
#
# The seam_ring_check violation this exists for is INTERMITTENT (~1 in 4 at
# the gate's 230ms/50j/20% profile), so a single green run proves nothing and
# a single red one is easy to lose. Each iteration gets its own output dir;
# failures are kept, passes are kept too (they are the control arm -- a
# violation that only ever appears in one of the two is the whole signal).
#
# Usage: tools/seam_hunt_loop.sh [iterations] [seed]
# Artifacts: logs/seam_hunt/<seed>/run<N>_{PASS,FAIL}/
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ITERS="${1:-8}"
SEED="${2:-130}"
BASE="$ROOT/logs/seam_hunt/$SEED"
mkdir -p "$BASE"

pass=0; viol=0; nocov=0
for i in $(seq 1 "$ITERS"); do
    tmp="$BASE/.run${i}_tmp"
    rm -rf "$tmp"; mkdir -p "$tmp"
    SEAM_SEEDS="$SEED" SEAM_OUT="$tmp" \
      SEAM_SPEC_LIVE="$ROOT/tools/.spec_runs/seam_hunt_${SEED}_$i" \
      bash "$ROOT/tools/seam_desync_gate.sh" > "$tmp/gate_stdout.log" 2>&1
    rc=$?
    # THREE outcomes, not two. Collapsing them is how a run that never
    # checked the failing peer scored as evidence of health:
    #   VIOLATION -- a real seam_ring_check violation (the thing we hunt)
    #   NOCOV     -- the session never reached battle, or P2 never wrote its
    #                ring. P1 has NEVER violated in any observed run; it is
    #                always P2, so a run missing P2's CSV proves nothing and
    #                must not count as a pass. The gate's own words for the
    #                rounds=0 case: "a harness/deployment failure, not a
    #                green run."
    #   PASS      -- full coverage (battle reached AND both rings present)
    #                with zero violations. Only this is evidence of health.
    rounds=$(grep -o 'rounds=[0-9]*' "$tmp/gate_stdout.log" 2>/dev/null | head -1 | cut -d= -f2)
    rounds="${rounds:-0}"
    p2csv=$(ls "$tmp"/*P2_seamring.csv 2>/dev/null | head -1)
    if [ "$rounds" -ge 2 ] && [ -n "$p2csv" ]; then
        if [ "$rc" -eq 0 ]; then verdict=PASS; pass=$((pass+1))
        else verdict=VIOLATION; viol=$((viol+1)); fi
    else
        verdict=NOCOV; nocov=$((nocov+1))
    fi
    dest="$BASE/run${i}_${verdict}"
    rm -rf "$dest"
    # mv can lose the race if the gate is still flushing; copy-then-remove is
    # slower but never leaves the run unarchived (run9 of the 2026-08-24 batch
    # vanished exactly this way and was silently scored twice).
    cp -r "$tmp" "$dest" 2>/dev/null && rm -rf "$tmp"
    echo "[seam-hunt] run $i/$ITERS -> $verdict (rounds=$rounds p2ring=$([ -n "$p2csv" ] && echo y || echo n)) " \
         "(pass=$pass violation=$viol nocov=$nocov)"
done
echo "[seam-hunt] DONE seed=$SEED pass=$pass violation=$viol nocov=$nocov  artifacts: $BASE"
echo "[seam-hunt] NOTE: only 'pass' and 'violation' are evidence; NOCOV runs measured nothing."
