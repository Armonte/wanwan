#!/usr/bin/env bash
# Run the hub path repeatedly until GekkoNet reports a player-vs-player desync,
# then stop and point at the evidence.
#
# The desync this hunts is a PURE RNG divergence: one extra gameplay game_rand
# draw on one peer with hp/round still identical (see
# docs/dev/desync_campaign_state.md). Attribution comes from the desync ring,
# which both peers dump on desync -- read the pair with
#
#     tools/desync_ring_diff.py --pair <P1 csv> <P2 csv>
#
# and look at which site_* column differs across the diverging frame.
#
# Hit rate is UNSTABLE (0 in 12, then 1 in 2, then 0 in 12). Budget accordingly.
# Do NOT judge by .fm2krep index diffs -- meaningless under rollback.
set -u
cd "$(dirname "$0")/.."

RUNS="${RUNS:-12}"
LOGDIR="${LOGDIR:-/mnt/c/games/2dfm/wanwan/logs}"
OUTBASE="${OUTBASE:-tools/.spec_runs/hunt}"

# Clear stale rings so the ones found afterwards are unambiguously from this
# hunt. They are timestamped, but an operator reading `ls` should not have to
# work that out.
rm -f "$LOGDIR"/FM2K_P*_desyncring_*.csv

for i in $(seq 1 "$RUNS"); do
  out="$OUTBASE/$i"
  rm -rf "$out"
  FM2K_TEST_OUT_DIR="$(pwd)/$out" FM2K_CK_REGIONS=1 \
    timeout 900 python3 -u tools/hub_spectate_e2e.py > "$OUTBASE/run_$i.log" 2>&1
  if grep -qE "DESYNC #" "$OUTBASE/run_$i.log"; then
    echo "run=$i DESYNC -- evidence in $out"
    grep -E "DESYNC #" "$OUTBASE/run_$i.log" | head -2
    ls -la "$LOGDIR"/FM2K_P*_desyncring_*.csv 2>/dev/null
    echo "desync-hunt-caught"
    exit 0
  elif grep -q "hubspec: PASS" "$OUTBASE/run_$i.log"; then
    echo "run=$i pass"
  else
    echo "run=$i other-fail"
    grep -E "hubspec: FAIL" -A2 "$OUTBASE/run_$i.log" | head -3
  fi
done
echo "desync-hunt-complete (no desync in $RUNS runs)"
