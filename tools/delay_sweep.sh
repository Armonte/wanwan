#!/usr/bin/env bash
# Auto input-delay A/B sweep under in-process link impairment.
#
# For each link profile (one-way delay / jitter / loss, applied per peer by the
# hook's FM2K_NET_* impairment layer) it runs spec_selftest twice per arm:
#   auto -- the computed delay (FM2K_LOCAL_DELAY unset, FM2K_DELAY_MODE=$MODE)
#   old  -- FM2K_LOCAL_DELAY pinned to what the 0.2.84 formula picks for that
#           link: ceil(one_way/10) clamped to [2,15] (10 ms frames, 100 Hz)
# then tools/delay_sweep_report.py summarizes brake / rollback / desync per arm.
#
# Runs against the DEPLOYED launcher (go.sh), like every spec_selftest stage.
# One sweep root under tools/.delay_sweep/<id>; only the newest $KEEP sweeps
# are kept.
#
# Env: FRAMES(2500) RUNS(2) MODE(0) KEEP(2) PROFILES("25:5:0 50:10:0 ...")
#      ARMS("auto old")
set -u
cd "$(dirname "$0")/.."

FRAMES="${FRAMES:-2500}"
RUNS="${RUNS:-2}"
MODE="${MODE:-0}"
KEEP="${KEEP:-2}"
ARMS="${ARMS:-auto old}"
# one_way_ms:jitter_ms:loss
PROFILES="${PROFILES:-25:5:0 50:10:0 100:20:0 150:20:0 115:15:0.20}"

ROOT="tools/.delay_sweep"
ID="$(date +%Y%m%d_%H%M%S)_$$"
OUT="$ROOT/$ID"
mkdir -p "$OUT"

# Prune older sweeps (literal root, never a bare variable path).
ls -1d tools/.delay_sweep/*/ 2>/dev/null | sort | head -n -"$KEEP" | while read -r d; do
    rm -rf -- "./$d"
done

old_delay() {  # 0.2.84 formula for a one-way latency in ms
    local ow=$1 d=$(( ($1 + 9) / 10 ))
    (( d < 2 )) && d=2
    (( d > 15 )) && d=15
    echo "$d"
}

echo "[sweep] out=$OUT frames=$FRAMES runs=$RUNS mode=$MODE profiles=[$PROFILES] arms=[$ARMS]"
for prof in $PROFILES; do
    IFS=: read -r ow jit loss <<<"$prof"
    for arm in $ARMS; do
        for r in $(seq 1 "$RUNS"); do
            tag="ow${ow}_j${jit}_l${loss}_${arm}_r${r}"
            dir="$(pwd)/$OUT/$tag"
            mkdir -p "$dir"
            local_delay=""
            [ "$arm" = "old" ] && local_delay="$(old_delay "$ow")"
            echo "[sweep] $tag (local_delay=${local_delay:-auto})"
            env FM2K_TEST_OUT_DIR="$dir" \
                FM2K_TEST_BACKGROUND="${FM2K_TEST_BACKGROUND:-1}" \
                FM2K_NET_DELAY_MS="$ow" FM2K_NET_JITTER_MS="$jit" \
                FM2K_NET_LOSS="$loss" FM2K_NET_SEED="$r" \
                FM2K_DELAY_MODE="$MODE" \
                ${local_delay:+FM2K_LOCAL_DELAY=$local_delay} \
                WSLENV="FM2K_NET_DELAY_MS:FM2K_NET_JITTER_MS:FM2K_NET_LOSS:FM2K_NET_SEED:FM2K_DELAY_MODE:FM2K_LOCAL_DELAY:FM2K_TEST_BACKGROUND" \
                timeout 1200 python3 -u tools/spec_selftest.py \
                    --frames "$FRAMES" --spectators battle1 \
                    > "$dir/harness.log" 2>&1
            echo "  rc=$?"
        done
    done
done

python3 tools/delay_sweep_report.py "$OUT" | tee "$OUT/report.txt"
echo "[sweep] DONE $OUT"
