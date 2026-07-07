#!/usr/bin/env bash
# rc_loss_matrix.sh — sweep the ReliableChannel spectator path across loss levels
# and seeds, recording the live-edge verdict per cell. FM2K_SPEC_RC toggles RC vs
# the TCP baseline. Sequential (each cell launches 3 game instances).
#
#   tools/rc_loss_matrix.sh            # RC path (FM2K_SPEC_RC=1)
#   RC=0 tools/rc_loss_matrix.sh       # TCP baseline
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/logs/gate_runs/rc_loss_matrix.tsv"
RC="${RC:-1}"
LOSSES="${LOSSES:-0.12 0.15 0.20}"
SEEDS="${SEEDS:-42 99}"
DELAY="${DELAY:-80}"
FRAMES="${FRAMES:-1200}"

echo -e "rc\tloss\tseed\treached_live\tgap\tspec_max\thost_final\tchecksum_frames\tverdict" > "$OUT"
for loss in $LOSSES; do
  for seed in $SEEDS; do
    log="$ROOT/logs/gate_runs/rc_m_${RC}_${loss}_${seed}.log"
    echo "[matrix] rc=$RC loss=$loss seed=$seed ..."
    FM2K_SPEC_RC="$RC" FM2K_NET_LOSS="$loss" FM2K_NET_DELAY_MS="$DELAY" FM2K_NET_SEED="$seed" \
      python3 "$ROOT/tools/spec_selftest.py" --frames "$FRAMES" --assert-spectator-live > "$log" 2>&1
    rc_exit=$?
    line=$(grep -iE "LIVE-EDGE S1" "$log" 2>/dev/null | tail -1)
    reached=$(echo "$line" | grep -oiE "spectator_reached_live=[A-Za-z]+" | cut -d= -f2)
    gap=$(echo "$line" | grep -oiE "gap=[0-9-]+" | cut -d= -f2)
    spec_max=$(echo "$line" | grep -oiE "spec_max_frame=[0-9]+" | cut -d= -f2)
    host_final=$(echo "$line" | grep -oiE "host_final_frame=[0-9]+" | cut -d= -f2)
    cks=$(grep -iE "CHECKSUM S1 seg0" "$log" 2>/dev/null | grep -oiE "[0-9]+ frames FULL-STATE IDENTICAL" | grep -oE "^[0-9]+")
    verdict=$(grep -iE "OVERALL (PASS|FAIL)" "$log" 2>/dev/null | grep -oiE "OVERALL (PASS|FAIL)" | tail -1)
    echo -e "${RC}\t${loss}\t${seed}\t${reached:-?}\t${gap:-?}\t${spec_max:-?}\t${host_final:-?}\t${cks:-0}\t${verdict:-?}" >> "$OUT"
    echo "  -> reached=${reached:-?} gap=${gap:-?} checksum_frames=${cks:-0} ${verdict:-?}"
  done
done
echo "[matrix] done -> $OUT"
column -t "$OUT"
