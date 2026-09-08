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
# 600 was the July default and is NO LONGER ENOUGH: under 0.20 loss the
# battle-join spectator can still be dialing when the host hits the frame cap
# and auto-terminates, and every retry then goes to a dead process. That
# produced 5 of 6 "join handshake freezes" in the 2026-09-07 sweep, all of them
# test artifacts. HOST-GONE below catches the residue if it recurs.
FRAMES="${FRAMES:-2400}"
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

  # Classify BY THE HARNESS'S OWN VERDICT. It already separates the two
  # failure kinds explicitly, and every attempt to infer them from failure
  # WORDS has been wrong -- three times:
  #   1. /DESYNC|divergen|MISMATCH/ matched this advisory:
  #        "crc= 2/599 mismatches ... [advisory -- gekko owns this term;
  #         judge by DESYNC #]"
  #      i.e. the sentence telling you not to judge by those words.
  #   2. "DESYNC #" still matched it, because that advisory ENDS with
  #      "DESYNC #]". The real line is `DESYNC #1 f=194: local=...`.
  #   3. "OVERALL FAIL" matched LIVENESS failures, which the harness states
  #      are "NOT an engine desync ... the correctness gates ran and said: no
  #      desync detected".
  # A liveness freeze also produces OVERALL FAIL, so DESYNC must be tested
  # with the digit or an actual gate FAIL, and freeze tested on its own text.
  # HOST-GONE FIRST. A spectator dialing a host that has already
  # auto-terminated reports "connect never established / host unreachable",
  # which is literally true and looks exactly like a join-handshake failure.
  # Distinguish by TIMESTAMP: if the host's last log line precedes the
  # spectator's first dial, the harness ran out of frames -- it is not a
  # transport bug, and it must never again be counted as one.
  hostlog="$OUT/$s/live_FM2K_P1_Debug.log"
  host_end=$(tail -1 "$hostlog" 2>/dev/null | grep -oE "^\[[0-9:.]+\]" | tr -d "[]")
  spec_dial=$(grep -hoE "^\[[0-9:.]+\].*(connecting|reconnecting) to root" \
                "$OUT/$s"/live_FM2K_S*_Debug.log 2>/dev/null \
              | grep -oE "^\[[0-9:.]+\]" | tr -d "[]" | sort | head -1)
  if grep -qhE "connect never established" "$OUT/$s"/live_FM2K_S*_Debug.log 2>/dev/null \
     && [ -n "$host_end" ] && [ -n "$spec_dial" ] \
     && [ "$spec_dial" \> "$host_end" ]; then
    echo "seed=$s HOST-GONE -- host auto-terminated at $host_end, spectator first dialed at $spec_dial (RAISE FRAMES, not a bug)"
    other=$((other+1))
    cp -r "$OUT/$s" "$ARCHIVE/seed_${s}_hostgone" 2>/dev/null
  elif grep -qE "DESYNC #[0-9]" "$log" || grep -qE "\-> FAIL" "$log"; then
    echo "seed=$s DESYNC"; desync=$((desync+1))
    grep -hE "DESYNC #[0-9]|\-> FAIL" "$log" | head -2
    cp -r "$OUT/$s" "$ARCHIVE/seed_${s}_desync" 2>/dev/null
  elif grep -qE "assert-spectator-live: FAILED|covered only 0 frames|OVERALL FAIL: --assert-spectator-live|connect never established|join handshake failed" "$log"; then
    echo "seed=$s FREEZE (spectator joined then received nothing)"; freeze=$((freeze+1))
    grep -hoE "GATE S[0-9] \([a-z0-9]+\): checked [0-9]+" "$log" | tr '\n' ' '; echo
    cp -r "$OUT/$s" "$ARCHIVE/seed_${s}_freeze" 2>/dev/null
  elif grep -q "OVERALL PASS" "$log"; then
    echo "seed=$s pass"; pass=$((pass+1))
  else
    echo "seed=$s OTHER (no verdict line -- run did not complete)"; other=$((other+1))
    tail -3 "$log"
    cp -r "$OUT/$s" "$ARCHIVE/seed_${s}_other" 2>/dev/null
  fi
done

n=$(echo $SEEDS | wc -w)
echo "sweep-complete: $n seeds -- pass=$pass desync=$desync freeze=$freeze other=$other"
echo "  desync here is a REGRESSION (Phase 4c should have removed the last one)."
echo "  freeze is the open join-handshake-under-loss item, a transport problem."
