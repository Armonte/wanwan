#!/usr/bin/env bash
# run_all_tests.sh — the single pre-release regression gate for FM2K rollback.
#
# One entrypoint that runs the netcode/determinism suite and returns ONE
# green/red verdict (non-zero exit on any failure), so a release can be gated
# on `tools/run_all_tests.sh` instead of a human remembering which scripts to
# run. Chains the existing gates; does not re-implement them.
#
# Stages (fast -> slow):
#   1. determinism   ci_determinism_gate.sh  — savestate serializes COMPLETELY
#                    under forced rollback (catch-up + 1:1 replay), the
#                    frame-764/OBJ_SLOT_COUNT regression class.
#   2. netplay+spec  spec_selftest --assert-spectator-live UNDER LOSS — a real
#                    2-player match + a spectator, asserting host/peer/spectator
#                    are bit-exact (CHECKSUM) + input-identical (CINPUT) and the
#                    spectator reached the live edge. Covers desync AND spectate.
#   3. multigame     multigame_determinism_sweep.sh (FULL only) — byte-identical
#                    engine determinism across the real FM2K game library.
#
# Usage:
#   tools/run_all_tests.sh                 # stages 1-2 (the pre-cut default)
#   FULL=1 tools/run_all_tests.sh          # + stage 3 (the multigame sweep)
#   LOSS=0.20 SPEC_RUNS=6 tools/run_all_tests.sh
#
# Env: FRAMES(1500) LOSS(0.15) SPEC_RUNS(4) FM2K_CHECK_DISTANCE(10) FULL(0)
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FRAMES="${FRAMES:-1500}"; LOSS="${LOSS:-0.15}"; SPEC_RUNS="${SPEC_RUNS:-4}"
CD="${FM2K_CHECK_DISTANCE:-10}"; FULL="${FULL:-0}"
OUT="$ROOT/logs/run_all_tests"; rm -rf "$OUT"; mkdir -p "$OUT"

kill_games() { for p in WonderfulWorld_ver_0946 WonderfulRvl FM2K_RollbackLauncher fm2k FM2K; do
    taskkill.exe /F /IM "${p}.exe" >/dev/null 2>&1; done; }

pass=(); fail=()
stage() {  # stage <name> <logfile> -- runs $CMD, verdict by EXIT CODE
    local name="$1" log="$2"
    echo "======================================================================"
    echo "[run_all] STAGE: $name"
    echo "======================================================================"
    kill_games; sleep 0.6
    ( eval "$CMD" ) > "$log" 2>&1; local rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "[run_all] $name: PASS"; pass+=("$name")
    else
        echo "[run_all] $name: FAIL (rc=$rc) — tail:"; tail -8 "$log" | sed 's/^/    /'
        fail+=("$name")
    fi
}

echo "[run_all] FRAMES=$FRAMES LOSS=$LOSS SPEC_RUNS=$SPEC_RUNS CHECK_DISTANCE=$CD FULL=$FULL"

# Stage 1 — determinism (uses the gate's own exit code).
CMD="FRAMES=$FRAMES FM2K_CHECK_DISTANCE=$CD bash '$ROOT/tools/ci_determinism_gate.sh'"
stage "determinism" "$OUT/1_determinism.log"

# Stage 2 — netplay + spectator under loss (bit-exact + live). Runs SPEC_RUNS
# times; ALL must be OVERALL PASS. spec_selftest owns the desync + liveness asserts.
s2_ok=1
for i in $(seq 1 "$SPEC_RUNS"); do
    kill_games; sleep 0.6
    FM2K_SPEC_RC=1 FM2K_NET_LOSS="$LOSS" FM2K_NET_DELAY_MS=80 FM2K_NET_SEED=$((40+i)) \
      timeout 220 python3 "$ROOT/tools/spec_selftest.py" --frames 1200 \
      --spectators css --assert-spectator-live --keep > "$OUT/2_netplay_run${i}.log" 2>&1
    if grep -qE "OVERALL PASS" "$OUT/2_netplay_run${i}.log"; then
        echo "[run_all]   netplay+spec run $i/$SPEC_RUNS: PASS"
    else
        echo "[run_all]   netplay+spec run $i/$SPEC_RUNS: FAIL"; s2_ok=0
        grep -E "OVERALL FAIL|desync" "$OUT/2_netplay_run${i}.log" | tail -2 | sed 's/^/      /'
    fi
done
[ "$s2_ok" = 1 ] && { echo "[run_all] netplay+spectator: PASS ($SPEC_RUNS/$SPEC_RUNS)"; pass+=("netplay+spectator"); } \
                 || { echo "[run_all] netplay+spectator: FAIL"; fail+=("netplay+spectator"); }

# Stage 3 — multigame determinism (opt-in, slow).
if [ "$FULL" = 1 ]; then
    CMD="bash '$ROOT/tools/multigame_determinism_sweep.sh'"
    stage "multigame" "$OUT/3_multigame.log"
else
    echo "[run_all] multigame: SKIPPED (set FULL=1 to run the library sweep)"
fi

kill_games
echo "======================================================================"
echo "[run_all] SUMMARY   pass=${#pass[@]}  fail=${#fail[@]}   logs: $OUT"
for s in "${pass[@]:-}"; do [ -n "$s" ] && echo "   PASS  $s"; done
for s in "${fail[@]:-}"; do [ -n "$s" ] && echo "   FAIL  $s"; done
if [ "${#fail[@]}" -eq 0 ]; then echo "[run_all] ALL GREEN — safe to cut."; exit 0
else echo "[run_all] RED — DO NOT cut until fixed."; exit 1; fi
