#!/usr/bin/env bash
# spec_stress_capture.sh — run a spectator scenario N times and ARCHIVE every
# run's PER-INSTANCE logs (host P1, player P2, spectator S1) + the harness stdout
# into a distinct per-run directory. Built because the plain harness clobbers
# live_FM2K_*.log each run, so intermittent failures (the ~1-in-4 RC spectator
# wedge under 20% loss) were un-diagnosable after the fact. Now every run is
# preserved, pass or fail, so you can diff a wedged run against a healthy one.
#
#   tools/spec_stress_capture.sh                       # defaults: 20% loss, seed 42, 12 runs
#   RUNS=8 LOSS=0.20 SEED=42 DELAY=80 FRAMES=1200 tools/spec_stress_capture.sh
#   TAG=myrun tools/spec_stress_capture.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RC="${RC:-1}"; LOSS="${LOSS:-0.20}"; SEED="${SEED:-42}"; DELAY="${DELAY:-80}"
FRAMES="${FRAMES:-1200}"; RUNS="${RUNS:-12}"
TAG="${TAG:-spec_stress_rc${RC}_loss${LOSS}_seed${SEED}}"
OUT="$ROOT/logs/gate_runs/$TAG"
LIVE="$ROOT/tools/.spec_selftest"
GAMEDIR="/mnt/c/games/2dfm/wanwan"
rm -rf "$OUT"; mkdir -p "$OUT"
SUMMARY="$OUT/summary.tsv"
echo -e "run\treached\tstall_frame\tgap\tchecksum_frames\tverdict\tlogdir" > "$SUMMARY"
echo "[capture] TAG=$TAG RC=$RC loss=$LOSS seed=$SEED delay=$DELAY frames=$FRAMES runs=$RUNS -> $OUT"

fails=0
for i in $(seq 1 "$RUNS"); do
    rd="$(printf '%s/run_%02d' "$OUT" "$i")"; mkdir -p "$rd"
    rm -f "$GAMEDIR"/FM2K_P*_desync_f*.log 2>/dev/null
    # Zombie hygiene: a run that hard-wedges (hung P1/P2 barrier from a
    # spectator reconnect storm, or a spectator that never self-exits)
    # leaves game/launcher processes holding the fixed ports (7000/7002/
    # 7100/7102). The NEXT run's peers then can't bind/sync -> a false
    # cascade of wedges that reads as a netcode bug but is pure port
    # contention. Kill any survivors before each run so every run starts
    # from a clean slate and failures are attributable to THIS run.
    for p in WonderfulWorld_ver_0946 WonderfulRvl FM2K_RollbackLauncher fm2k FM2K; do
        taskkill.exe /F /IM "${p}.exe" >/dev/null 2>&1
    done
    sleep 0.5
    out="$(FM2K_SPEC_RC="$RC" FM2K_NET_LOSS="$LOSS" FM2K_NET_DELAY_MS="$DELAY" FM2K_NET_SEED="$SEED" \
        timeout 220 python3 "$ROOT/tools/spec_selftest.py" --frames "$FRAMES" \
        --assert-spectator-live --keep 2>&1)"
    echo "$out" > "$rd/harness.log"
    # ARCHIVE every instance's live debug log for THIS run.
    for f in P1 P2 S1; do
        cp "$LIVE/live_FM2K_${f}_Debug.log" "$rd/${f}.log" 2>/dev/null
    done
    cp "$GAMEDIR"/FM2K_P*_desync_f*.log "$rd/" 2>/dev/null
    line="$(echo "$out" | grep -iE 'LIVE-EDGE S1' | tail -1)"
    reached="$(echo "$line" | grep -oiE 'reached_live=[A-Za-z]+' | cut -d= -f2)"
    sf="$(echo "$out" | grep -oiE 'stall_frame=[0-9]+' | tail -1 | grep -oE '[0-9]+')"
    gap="$(echo "$line" | grep -oiE 'gap=[0-9-]+' | cut -d= -f2)"
    cks="$(echo "$out" | grep -iE 'CHECKSUM S1' | grep -oE '[0-9]+ frames FULL-STATE' | grep -oE '^[0-9]+' | head -1)"
    verdict="$(echo "$out" | grep -oiE 'OVERALL (PASS|FAIL)' | tail -1)"
    echo -e "${i}\t${reached:-?}\t${sf:-?}\t${gap:-?}\t${cks:-0}\t${verdict:-?}\t$rd" >> "$SUMMARY"
    tag="OK"; [ "$reached" != "True" ] && { tag="WEDGE"; fails=$((fails+1)); }
    echo "[capture] run $i: $tag reached=${reached:-?} stall_frame=${sf:-?} gap=${gap:-?} checksum=${cks:-0} -> $rd"
done
echo "========================================"
column -t "$SUMMARY"
echo "[capture] $fails/$RUNS wedged. Per-instance logs preserved under $OUT/run_NN/"
