#!/usr/bin/env bash
# spec_host_gone_test.sh -- verify a spectator handles an UNGRACEFUL host
# vanish (crash / network drop / TerminateProcess, no SPEC_SESSION_END)
# gracefully: it must BACK OFF its reconnect attempts (not storm a dead host
# at a fixed 500ms) and then EXIT CLEANLY with a clear "host disconnected"
# message -- instead of spinning in a reconnect loop forever.
#
# The harness terminates the host at --frames (a hard TerminateProcess, no
# SESSION_END), so the spectator -- live by then -- sees its upstream vanish.
# We drive the spectator's OWN host-gone watchdog to fire BEFORE the harness's
# ~6s post-host cleanup by setting FM2K_SPEC_HOST_GONE_MS below it, so the
# spectator self-exits and we can observe its behavior in its own log.
#
#   tools/spec_host_gone_test.sh           # defaults: 20% loss, 3 runs
#   RUNS=5 LOSS=0.10 tools/spec_host_gone_test.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOSS="${LOSS:-0.20}"; RUNS="${RUNS:-3}"; FRAMES="${FRAMES:-700}"
# THE ONE DELIBERATE PLACE THAT TRUNCATES THE LADDER, and it is justified:
# this stage MEASURES the give-up itself, so it must fire before the harness's
# ~6s post-host cleanup kills the process it is observing. Every OTHER spectator
# harness stopped overriding this on 2026-08-18 (H1) -- 5000 sits below the
# shipped 12000 budget and below RC stall repair (3500) + both starve
# escalations (4000, +4000), so a run taken at 5000 is measuring a different
# product. Do not copy this value anywhere else.
GONE_MS="${GONE_MS:-5000}"      # spectator host-gone watchdog (< harness cleanup)
LIVE="$ROOT/tools/.spec_selftest"
OUT="$ROOT/logs/gate_runs/host_gone"
rm -rf "$OUT"; mkdir -p "$OUT"

pass=0; fail=0
for i in $(seq 1 "$RUNS"); do
    rd="$(printf '%s/run_%02d' "$OUT" "$i")"; mkdir -p "$rd"
    for p in WonderfulWorld_ver_0946 WonderfulRvl FM2K_RollbackLauncher fm2k FM2K; do
        taskkill.exe /F /IM "${p}.exe" >/dev/null 2>&1
    done
    sleep 0.6
    # Host dies at battle frame $FRAMES (mid-match); spectator joins at CSS so
    # it is solidly LIVE before the host vanishes.
    FM2K_SPECTATOR_DEBUG=1 FM2K_SPEC_RC=1 FM2K_NET_LOSS="$LOSS" FM2K_NET_DELAY_MS=80 \
      FM2K_NET_SEED=$((100+i)) FM2K_SPEC_HOST_GONE_MS="$GONE_MS" \
      timeout 200 python3 "$ROOT/tools/spec_selftest.py" --frames "$FRAMES" \
      --spectators css --keep >/dev/null 2>&1
    cp "$LIVE/live_FM2K_S1_Debug.log" "$rd/S1.log" 2>/dev/null
    cp "$LIVE/live_FM2K_P1_Debug.log" "$rd/P1.log" 2>/dev/null

    S1="$rd/S1.log"
    # 1) Spectator got LIVE before the host vanished. A css-phase join is
    #    FULL_SESSION (streams from frame 0, no snapshot), so key off the
    #    admitted-input counter reaching well into battle rather than a
    #    "SNAPSHOT applied" line (which only a mid-battle snapshot join emits).
    peak_total=$(grep -oE "total=[0-9]+" "$S1" 2>/dev/null | grep -oE "[0-9]+" | sort -n | tail -1)
    got_live=$([ "${peak_total:-0}" -ge 100 ] && echo 1 || echo 0)
    # 2) Clean host-gone / stream-end exit present (NOT an infinite spin).
    clean_exit=$(grep -cE "host disconnected .* closing stream|stream ended .* host left" "$S1" 2>/dev/null)
    # 3) Reconnect BACKOFF engaged: intervals escalate past the 500ms base.
    #    "next backoff Nms" -- collect the distinct N values seen.
    backoffs=$(grep -oE "next backoff [0-9]+ms" "$S1" 2>/dev/null | grep -oE "[0-9]+" | sort -n | uniq | tr '\n' ' ')
    max_backoff=$(echo "$backoffs" | tr ' ' '\n' | grep -E '^[0-9]+$' | sort -n | tail -1)
    n_reconnect=$(grep -c "reconnecting to root" "$S1" 2>/dev/null)

    ok=1; why=""
    [ "${got_live:-0}" -ge 1 ] || { ok=0; why="$why not-live"; }
    [ "${clean_exit:-0}" -ge 1 ] || { ok=0; why="$why no-clean-exit"; }
    # backoff must have escalated to at least 2000ms (500->1000->2000) AND the
    # storm must be bounded (a 500ms-fixed storm over ~5s would be ~10 retries).
    [ "${max_backoff:-0}" -ge 2000 ] || { ok=0; why="$why no-backoff(max=${max_backoff:-0})"; }
    [ "${n_reconnect:-99}" -le 8 ] || { ok=0; why="$why storm(${n_reconnect})"; }

    if [ "$ok" = 1 ]; then
        pass=$((pass+1)); verdict="PASS"
    else
        fail=$((fail+1)); verdict="FAIL[$why ]"
    fi
    echo "run $i: $verdict  live=${got_live:-0} clean_exit=${clean_exit:-0} reconnects=${n_reconnect:-?} backoffs=[$backoffs]"
done
echo "========================================"
echo "[host-gone] $pass/$RUNS PASS, $fail/$RUNS FAIL -> logs in $OUT/run_NN/"
[ "$fail" = 0 ] && echo "HOST-GONE-TEST: ALL PASS" || echo "HOST-GONE-TEST: FAILURES"
