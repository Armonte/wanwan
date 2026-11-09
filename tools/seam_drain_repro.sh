#!/usr/bin/env bash
# Match-end drain / rematch-seam repro under a high-latency link.
#
# Runs the multi-match E2E shape (1-round 15 s matches, several match ends per
# run) with in-process impairment and a LOW input delay -- the shape that made
# the old EndBattle drain give up and carry a misprediction into the rematch
# (docs/dev/investigations/2026-11-08_rematch_f1_desync/). Reports, per run:
# GekkoNet DESYNC # count, drain complete/timeout counts, harness verdict.
#
# Env: FM2K_SEAM_EXIT_INPUT_NEUTRAL=0 is the exit-tick-leak negative control.
# Env: RUNS(4) TOTAL(6000) OW(100) JIT(20) LOSS(0) DELAY(5; "auto" = unset)
set -u
cd "$(dirname "$0")/.."
RUNS="${RUNS:-4}"; TOTAL="${TOTAL:-6000}"
OW="${OW:-100}"; JIT="${JIT:-20}"; LOSS="${LOSS:-0}"; DELAY="${DELAY:-5}"
ROOT="tools/.delay_sweep"
OUT="$ROOT/seam_$(date +%Y%m%d_%H%M%S)_$$"
mkdir -p "$OUT"
echo "[seam] out=$OUT runs=$RUNS total=$TOTAL link=${OW}ms/${JIT}ms/loss$LOSS delay=$DELAY"
for i in $(seq 1 "$RUNS"); do
    dir="$(pwd)/$OUT/run$i"; mkdir -p "$dir"
    local_delay=""; [ "$DELAY" != "auto" ] && local_delay="$DELAY"
    env FM2K_TEST_OUT_DIR="$dir" FM2K_TEST_BACKGROUND="${FM2K_TEST_BACKGROUND:-1}" \
        FM2K_NET_DELAY_MS="$OW" FM2K_NET_JITTER_MS="$JIT" FM2K_NET_LOSS="$LOSS" \
        FM2K_NET_SEED=$((90 + i)) \
        ${local_delay:+FM2K_LOCAL_DELAY=$local_delay} \
        WSLENV="FM2K_NET_DELAY_MS:FM2K_NET_JITTER_MS:FM2K_NET_LOSS:FM2K_NET_SEED:FM2K_LOCAL_DELAY:FM2K_TEST_BACKGROUND:FM2K_SEAM_EXIT_INPUT_NEUTRAL" \
        timeout 900 python3 -u tools/spec_selftest.py \
            --rounds 1 --round-time 15 --total-frames "$TOTAL" \
            --spectators battle1 --keep > "$dir/harness.log" 2>&1
    ds=$(cat "$dir"/live_FM2K_P*_Debug.log 2>/dev/null | grep -c "DESYNC #")
    ok=$(cat "$dir"/live_FM2K_P*_Debug.log 2>/dev/null | grep -c "EndBattle drain complete")
    to=$(cat "$dir"/live_FM2K_P*_Debug.log 2>/dev/null | grep -c "EndBattle drain timed out")
    oob=$(cat "$dir"/live_FM2K_P*_Debug.log 2>/dev/null | grep -c "ENDSEAM-OOB")
    verdict=$(grep -oE "OVERALL (PASS|FAIL)" "$dir/harness.log" | tail -1)
    echo "[seam] run$i desync_lines=$ds drain_complete=$ok drain_timeout=$to oob=$oob ${verdict:-no-verdict}"
done
echo "[seam] DONE $OUT"
