#!/usr/bin/env bash
# Spectator-under-loss sweep: the documented repro for the battle-mid-join
# class, run across N seeds and CLASSIFIED, because the failures here are not
# all the same thing and counting them together hides which one moved.
#
# THE PROFILE IS THE ONE THAT FOUND THE BUG, not a guess:
#   FM2K_NET_DELAY_MS=150 FM2K_NET_JITTER_MS=40 FM2K_NET_LOSS=0.20
#   --frames 600 --spectators css,battle1 --assert-spectator-live
# Seeds 1 and 6 failed on the original build; 2/3/4/5/7 passed. The impairment
# is seeded, so a given seed is deterministic and a fix can be A/B'd per seed.
#
# HISTORY, so a number here means something (memory: battle-mid-join):
#   baseline (fix#1 only) 12/40 = 10 desync + 2 freeze
#   fix#2 unbounded hold   7/40 =  2 desync + 5 freeze   (traded desync for freeze)
#   fix#2 bounded + fix#3  3/40 =  1 desync + 2 freeze   <- committed ff387b8
# The 1 residual desync was the FROM-SCRATCH early-CSS joiner, which Phase 4c
# pool-sync should since have removed (every bound subscriber now gets the
# host's battle-entry snapshot). The 2 freezes are a JOIN-HANDSHAKE transport
# failure under loss -- `total=0`, never received a single input -- and are a
# different domain from the desync.
#
# So the expectation for a current build is: 0 desync, and the freezes are what
# is left to chase. A run that shows desyncs again is a REGRESSION, not the old
# bug.
#
# MACHINE TOKEN: launches games. Do not run alongside run_all_tests.
set -u
cd "$(dirname "$0")/.."

SEEDS="${SEEDS:-1 2 3 4 5 6 7 8}"
FRAMES="${FRAMES:-600}"
OUT="${OUT:-tools/.spec_runs/lossweep}"
ARCHIVE="${ARCHIVE:-docs/dev/evidence/lossweep}"
mkdir -p "$OUT" "$ARCHIVE" || { echo "cannot create $OUT"; exit 2; }

pass=0; desync=0; freeze=0; other=0
for s in $SEEDS; do
  log="$OUT/seed_$s.log"
  rm -rf "$OUT/$s"
  FM2K_TEST_OUT_DIR="$(pwd)/$OUT/$s" \
  FM2K_NET_SEED="$s" FM2K_NET_DELAY_MS=150 FM2K_NET_JITTER_MS=40 FM2K_NET_LOSS=0.20 \
    timeout 900 python3 -u tools/spec_selftest.py --frames "$FRAMES" \
      --spectators css,battle1 --assert-spectator-live > "$log" 2>&1
  rc=$?

  if [ ! -s "$log" ]; then
    echo "seed=$s ABORT -- no log written; the harness never ran"; exit 2
  fi

  # Classify. FREEZE is checked FIRST: a frozen viewer also trips the liveness
  # assertion, so a naive "did it fail" count reports it as a desync and the
  # two get conflated -- which is exactly how the 12/40 -> 7/40 "improvement"
  # above hid five NEW freezes.
  if grep -qE "connect never established|join handshake failed" "$log"; then
    echo "seed=$s FREEZE (join handshake never completed)"; freeze=$((freeze+1))
    cp -r "$OUT/$s" "$ARCHIVE/seed_${s}_freeze" 2>/dev/null
  elif grep -qE "DESYNC|divergen|MISMATCH|not bit-exact" "$log"; then
    echo "seed=$s DESYNC"; desync=$((desync+1))
    grep -hE "DESYNC|divergen|first divergence" "$log" | head -2
    cp -r "$OUT/$s" "$ARCHIVE/seed_${s}_desync" 2>/dev/null
  elif grep -qE "PASS|OVERALL PASS" "$log"; then
    echo "seed=$s pass"; pass=$((pass+1))
  else
    echo "seed=$s OTHER"; other=$((other+1))
    tail -3 "$log"
    cp -r "$OUT/$s" "$ARCHIVE/seed_${s}_other" 2>/dev/null
  fi
done

n=$(echo $SEEDS | wc -w)
echo "sweep-complete: $n seeds -- pass=$pass desync=$desync freeze=$freeze other=$other"
echo "  desync here is a REGRESSION (Phase 4c should have removed the last one)."
echo "  freeze is the open join-handshake-under-loss item, a transport problem."
