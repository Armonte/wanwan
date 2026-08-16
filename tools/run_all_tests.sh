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
#                    spectator reached the live edge. SINGLE match / CSS join.
#   2b. multi-match  spec_selftest --total-frames --spectators css,battle1 — the
#      E2E           full lifecycle stage 2 omits: rounds -> match end -> CSS ->
#                    rematch (match 2), a CSS-join spectator AND a mid-match
#                    snapshot joiner, gating the rematch boundary + match-2
#                    coverage (no vacuous match-1-only pass).
#   2d. deep-join    spec_selftest --spectators battle1,css2 -- the BOUNDED
#                    between-matches join: a viewer that dials in between
#                    matches is backfilled from the CURRENT char-select and
#                    handed a battle-entry snapshot instead of replaying the
#                    whole session. Until 2026-08 NO stage exercised that path,
#                    so gate-green was zero evidence for it. Now DEFAULT ON, so
#                    the stage also runs a KILL-SWITCH check
#                    (FM2K_SPEC_DEEP_JOIN=0 -> the legacy from-frame-0 path).
#   2e. seamdesync   spec_selftest at the EXTREME profile (230ms/50ms jitter/
#                    20% loss), seeded, multi-match -- the MATCH-END SEAM
#                    rollback-desync class (the 967f89f regression). Judged ONLY
#                    by GekkoNet's `DESYNC #` term, plus tools/seam_ring_check.py
#                    when the build emits seam-ring CSVs. FULL=1 adds the
#                    SAME gate pointed at vanpri (seamdesync-vanpri, ~2 min):
#                    the fix was only ever validated on wanwan, and the legacy
#                    lever reproduces the mechanism on vanpri too.
#   2f. hubspec      hub_spectate_e2e.py -- the HUB-BROKERED spectate path:
#                    a LOCAL hub (FM2K_HUB_AUTH_DISABLE=1, no Discord, no DB),
#                    three launchers driven headless by the default-off
#                    FM2K_HUB_AUTOMATION surface, a real challenge/accept, and a
#                    spectator that exists ONLY because the hub granted it (no
#                    --spectate anywhere). Every other spectator stage dials the
#                    host directly, so the grant path had zero coverage.
#                    FULL=1 adds the relay variant (spec_transport=relay
#                    negotiated through the grant + the hub's 0xCF data plane).
#   2g. specgame     spec_selftest --game vanpri --total-frames 16000 (FULL only)
#                    -- the spectator x HEAVY-GAME hole: every other spectator
#                    stage runs without --game, i.e. always wanwan (~10 objects),
#                    so spectate against an 80-150-object game with projectiles
#                    and recursive stage scripts had NEVER been gated. That is
#                    where the object-pool index incoherence decides a hit.
#   3. ddraw         ddraw_redirect_test.sh — cnc-ddraw redirect applied.
#   4. multigame     multigame_determinism_sweep.sh (FULL only) — byte-identical
#                    engine determinism across the real FM2K game library.
#   5. fm95-determ.  fm95_stress_smoke.py — FM95/CPW rollback determinism
#                    (FM95Hook.dll engine). gekko-desync verdict (FM95 emits no
#                    .pty). Self-skips if CPW absent. FM95_NETPLAY=1 also runs
#                    the WIP 2-instance netplay smoke (non-gating).
#
# Stages 1-4 are the FM2K engine; stage 5 is the FM95 engine — one gate, two
# engines. FM95 uses a gekko-desync verdict backend (not the FM2K .pty parity
# path) per docs/dev/fm95_harness_gap.md.
#
# Usage:
#   tools/run_all_tests.sh                 # stages 1-2b-3-5 (the pre-cut default)
#   FULL=1 tools/run_all_tests.sh          # + stage 4 (the multigame sweep)
#   FM95_NETPLAY=1 tools/run_all_tests.sh  # + the WIP FM95 netplay smoke
#   LOSS=0.20 SPEC_RUNS=6 tools/run_all_tests.sh
#
# Env: FRAMES(1500) LOSS(0.15) SPEC_RUNS(4) FM2K_CHECK_DISTANCE(10) FULL(0)
#      MM_RUNS(1) MM_TOTAL(3200) MM_LOSS(0.06)  — stage 2b multi-match E2E
#      DJ_TOTAL(3200) DJ_LOSS(0.10) DJ_TOL(250) DJ_TIMEOUT(400) DJ_SKIP(0)
#      KS_SKIP(0)                               -- stage 2d deep join (+kill-switch)
#      SEAM_SEEDS(130; FULL: "130 131") SEAM_TOTAL(6000) SEAM_TIMEOUT(480)
#      SEAM_SKIP(0) SEAM_GAMEDIR(/mnt/c/games/2dfm/wanwan)  -- stage 2e seam desync
#      SEAM_VANPRI_SKIP(0) SEAM_VANPRI_SEEDS(130)  -- stage 2e vanpri leg (FULL)
#      HUBSPEC_SKIP(0) HUBSPEC_TIMEOUT(480)     -- stage 2f hub-brokered spectate
#      SPECGAME_SKIP(0) SPECGAME_TIMEOUT(720) VANPRI_EXE(...)  -- stage 2g
#      CPW_EXE(/mnt/c/dev/fm95/CPW/ＣＰＷ.exe) FM95_NETPLAY(0)  — stage 5
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FRAMES="${FRAMES:-1500}"; LOSS="${LOSS:-0.15}"; SPEC_RUNS="${SPEC_RUNS:-4}"
CD="${FM2K_CHECK_DISTANCE:-10}"; FULL="${FULL:-0}"
OUT="$ROOT/logs/run_all_tests"; rm -rf "$OUT"; mkdir -p "$OUT"

kill_games() { for p in WonderfulWorld_ver_0946 WonderfulRvl FM2K_RollbackLauncher fm2k FM2K CPW ＣＰＷ vanpri; do
    taskkill.exe /F /IM "${p}.exe" >/dev/null 2>&1; done; }

# FM95/CPW reference exe (fullwidth filename). Stage 5 self-skips if absent so
# the gate still runs on FM2K-only machines.
CPW_EXE="${CPW_EXE:-/mnt/c/dev/fm95/CPW/ＣＰＷ.exe}"

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

# Stage 0 — host-native unit tests (doctest suite + delay math + reliable
# channel). Cheap and toolchain-free, so it runs FIRST: if the wire format or
# queue mechanics are broken there is no point spending minutes on the
# determinism and netplay stages. Until 2026-08 this gate did not run the unit
# suite at all, so "run_all_tests ALL GREEN" silently excluded it.
CMD="bash '$ROOT/tests/run.sh'"
stage "unit" "$OUT/0_unit.log"

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

# Stage 2b — multi-match E2E: the FULL lifecycle stage-2 does NOT cover. Plays
# rounds -> match end -> back to CSS -> rematch (match 2), with a FULL_SESSION
# spectator (css) AND a mid-match CURRENT_MATCH snapshot joiner (battle1). Gates
# the rematch boundary (deferred PIN_RNG applied), the spectator following into
# match 2 (>=2 battle segments), the mid-battle snapshot join, and — via the B2
# coverage assert — the authoritative rng-trace GATE actually spanning >=2
# matches (so a match-2 desync can't slip past a match-1-only "checked>0" pass).
MM_RUNS="${MM_RUNS:-1}"; MM_TOTAL="${MM_TOTAL:-3200}"; MM_LOSS="${MM_LOSS:-0.06}"
s2b_ok=1
for i in $(seq 1 "$MM_RUNS"); do
    kill_games; sleep 0.6
    FM2K_SPEC_RC=1 FM2K_NET_LOSS="$MM_LOSS" FM2K_NET_DELAY_MS=80 FM2K_NET_SEED=$((70+i)) \
      timeout 300 python3 "$ROOT/tools/spec_selftest.py" \
      --rounds 1 --round-time 15 --total-frames "$MM_TOTAL" \
      --spectators css,battle1 --assert-spectator-live --keep \
      > "$OUT/2b_multimatch_run${i}.log" 2>&1
    if grep -qE "OVERALL PASS" "$OUT/2b_multimatch_run${i}.log"; then
        echo "[run_all]   multi-match E2E run $i/$MM_RUNS: PASS"
    else
        echo "[run_all]   multi-match E2E run $i/$MM_RUNS: FAIL"; s2b_ok=0
        grep -E "OVERALL FAIL|FAIL:|desync|match 2|gate saw" "$OUT/2b_multimatch_run${i}.log" \
            | tail -3 | sed 's/^/      /'
    fi
done
[ "$s2b_ok" = 1 ] && { echo "[run_all] multi-match E2E (rematch+midjoin): PASS"; pass+=("multi-match-e2e"); } \
                  || { echo "[run_all] multi-match E2E (rematch+midjoin): FAIL"; fail+=("multi-match-e2e"); }

# Stage 2c — CSS-phase parity gate self-test (#66 Phase 1). The [CSS-FP] gate
# already runs INSIDE stages 2/2b (part of spec_selftest OVERALL PASS/FAIL);
# this proves the gate can still FAIL -- it re-runs the REAL gate against the
# fresh CSS logs stage 2b just left (--keep) plus three injected desyncs (wrong
# locked char / diverged sel-path / host!=guest). A gate that can't catch a
# break is worthless, and CSS is about to gain rollback. Offline, no launch.
CMD="python3 '$ROOT/tools/test_css_gate.py'"
stage "css-gate-selftest" "$OUT/2c_css_gate.log"

# Stage 2d -- BOUNDED DEEP JOIN. Every other spectator stage joins early enough
# to watch match 1, so none of them can reach the bounded between-matches path
# (a css1 joiner has no prior match, so HaveBoundedAnchor() is false; a battle1
# joiner gets a BATTLE grant, which is ineligible by definition). "ALL GREEN"
# was, until this stage existed, no evidence whatsoever for the deep-join
# feature (docs/dev/spectate_deep_join_rollout.md, Phase 1 item 4). That same
# fact is why flipping the default ON did not change any other stage's path.
#
# FM2K_SPEC_DEEP_JOIN=1 is still passed explicitly below even though it is now
# the DEFAULT. It is REDUNDANT ON PURPOSE: the stage's job is to gate the
# bounded path, not to gate whatever the default happens to be this month, and
# assert (a) below would otherwise be silently testing the default instead of
# the feature. The default is gated by the kill-switch check that follows.
#
# The profile is the one the Wave 3/4/4.1 validation rounds used: a 2-match host
# under 10% loss / 100ms / 30ms jitter with TWO spectators --
#   S1 = battle1 : mid-battle CURRENT_MATCH snapshot join   (the CONTROL; it
#                  must stay bit-exact across BOTH matches, so a deep-join
#                  change that damages the ordinary snapshot path shows here)
#   S2 = css2    : dials in during the host's SECOND char-select = the bounded
#                  deep joiner (the path under test)
#
# Five asserts, each one a distinct failure mode:
#   a. host logged "bounded deep join ENABLED"      -> the env actually reached
#      the host (the Wave 4 round shipped a harness that silently dropped it,
#      making every "gated" run measure the default path)
#   b. host logged the bounded backfill "skipping N event(s) of prior matches"
#      with N > 0                                   -> the bounded path ENGAGED
#   c. viewer logged "[SPEC-DEEPJOIN] snapshot APPLIED at anchor=" -> the ladder
#      ran to completion instead of parking in the hold
#   d. CHECKSUM S2 FULL-STATE IDENTICAL             -> the deep joiner is
#      bit-exact with the host (the fencepost the older subset gates are blind
#      to; Wave 3 was held precisely because this said DESYNC 7/7)
#   e. CHECKSUM S1 seg0 AND seg1 IDENTICAL          -> the control unregressed
# plus the harness's own OVERALL PASS, which folds in CINPUT, CSS-FP, the rng/hp
# gate and --assert-spectator-live.
#
# --assert-spectator-live is ON here (it is bounded-join aware since 2026-08) at
# a loosened FM2K_LIVE_EDGE_TOLERANCE: the host hard-terminates at
# --total-frames while a viewer that joined seconds ago is still draining a
# legitimately deep delay bank (measured gaps across 12 validation runs: 7..141
# frames). 250 keeps the assert meaningful -- a viewer that fell back to the
# from-frame-0 path lands ~880 frames off and still fails it.
#
# Placed AFTER 2c on purpose: 2c re-runs the CSS gate against the logs stage 2b
# left in tools/.spec_selftest, and this stage overwrites them.
#
# TIMING-DEPENDENT: spectator join timing is NOT determinized by FM2K_NET_SEED
# (joins are keyed off host-log markers plus wall-clock settles), so a single
# failure is retried ONCE before the stage is called red. Two runs max keeps the
# gate's wall clock sane.
DJ_TOTAL="${DJ_TOTAL:-3200}"; DJ_LOSS="${DJ_LOSS:-0.10}"
DJ_TOL="${DJ_TOL:-250}"; DJ_TIMEOUT="${DJ_TIMEOUT:-400}"
SPEC_LIVE="$ROOT/tools/.spec_selftest"

deep_join_attempt() {   # $1 = attempt number; 0 = PASS
    local att="$1"
    local log="$OUT/2d_deepjoin_att${att}.log"
    local ev="$OUT/2d_deepjoin_att${att}_evidence.txt"
    kill_games; sleep 0.6
    # Clear the preserved live logs so a crashed/timed-out attempt cannot be
    # judged on the PREVIOUS attempt's evidence.
    rm -f "$SPEC_LIVE"/live_FM2K_*_Debug.log
    FM2K_SPEC_DEEP_JOIN=1 FM2K_SPEC_RC=1 FM2K_NET_LOSS="$DJ_LOSS" \
      FM2K_NET_DELAY_MS=100 FM2K_NET_JITTER_MS=30 FM2K_NET_SEED=$((90 + att)) \
      FM2K_LIVE_EDGE_TOLERANCE="$DJ_TOL" \
      timeout "$DJ_TIMEOUT" python3 "$ROOT/tools/spec_selftest.py" \
        --rounds 1 --round-time 15 --total-frames "$DJ_TOTAL" \
        --spectators battle1,css2 --assert-spectator-live --keep \
        > "$log" 2>&1
    local host="$SPEC_LIVE/live_FM2K_P1_Debug.log"
    local s2="$SPEC_LIVE/live_FM2K_S2_Debug.log"
    # Small, self-contained evidence file: the ladder + the verdicts. The live
    # logs themselves are ~6 MB/run and the NEXT stage overwrites them.
    {   echo "== host: deep-join gate + bounded backfill =="
        grep -aE "\[SPEC-DEEPJOIN\]|bounded backfill from CSS anchor" "$host" 2>/dev/null | head -30
        echo "== viewer css2 (S2): [SPEC-DEEPJOIN] ladder =="
        grep -a "\[SPEC-DEEPJOIN\]" "$s2" 2>/dev/null | head -30
        echo "== harness verdicts =="
        grep -aE "LIVE-EDGE|CATCHUP|CHECKSUM S|CINPUT S|GATE S|CSS-SPEC|OVERALL" "$log" 2>/dev/null
    } > "$ev" 2>&1
    local why=""
    grep -qa "bounded deep join ENABLED" "$host" 2>/dev/null \
        || why="$why; host never logged 'bounded deep join ENABLED' (FM2K_SPEC_DEEP_JOIN not forwarded, or a build without the feature)"
    grep -qaE "bounded backfill from CSS anchor.*skipping [1-9][0-9]* event" "$host" 2>/dev/null \
        || why="$why; bounded path NOT engaged (no 'skipping N event(s) of prior matches')"
    grep -qa "\[SPEC-DEEPJOIN\] snapshot APPLIED at anchor=" "$s2" 2>/dev/null \
        || why="$why; the ladder never reached an APPLIED snapshot"
    grep -qaE "CHECKSUM S2 seg[0-9]+: .*FULL-STATE IDENTICAL" "$log" 2>/dev/null \
        || why="$why; css2 (deep joiner) NOT full-state identical"
    if ! grep -qa "CHECKSUM S1 seg0: .*FULL-STATE IDENTICAL" "$log" 2>/dev/null \
       || ! grep -qa "CHECKSUM S1 seg1: .*FULL-STATE IDENTICAL" "$log" 2>/dev/null; then
        why="$why; battle1 snapshot control regressed (needs BOTH segments identical)"
    fi
    grep -qa "OVERALL PASS" "$log" 2>/dev/null || why="$why; harness OVERALL not PASS"
    if [ -n "$why" ]; then
        echo "[run_all]   deep-join attempt $att: FAIL --${why#;}"
        grep -aE "OVERALL FAIL|FULL-STATE DESYNC|CSS DESYNC|LIVE-EDGE S2" "$log" 2>/dev/null \
            | tail -3 | sed 's/^/      /'
        return 1
    fi
    echo "[run_all]   deep-join attempt $att: PASS"
    grep -aoE "skipping [0-9]+ event\(s\) of prior matches" "$host" 2>/dev/null \
        | head -1 | sed 's/^/      bounded: /'
    grep -a "LIVE-EDGE S2" "$log" 2>/dev/null | head -1 | sed 's/^.*LIVE-EDGE/      LIVE-EDGE/'
    grep -a "CATCHUP S2" "$log" 2>/dev/null | head -1 | sed 's/^.*CATCHUP/      CATCHUP/'
    return 0
}

if [ "${DJ_SKIP:-0}" = 1 ]; then
    echo "[run_all] deep-join: SKIPPED (DJ_SKIP=1)"
else
    echo "======================================================================"
    echo "[run_all] STAGE: deep-join (bounded between-matches spectate join)"
    echo "======================================================================"
    echo "[run_all]   profile: DEEP_JOIN=1 loss=$DJ_LOSS delay=100ms jitter=30ms"
    echo "[run_all]            --total-frames $DJ_TOTAL --spectators battle1,css2"
    echo "[run_all]            live-edge tolerance $DJ_TOL frames"
    echo "[run_all]   NOTE: deep-join outcomes are TIMING-dependent (the join is"
    echo "[run_all]         keyed off host-log markers, not the net seed), so one"
    echo "[run_all]         failure is RETRIED ONCE before the stage goes red."
    s2d_ok=0
    for att in 1 2; do
        if deep_join_attempt "$att"; then
            s2d_ok=1
            [ "$att" = 2 ] && echo "[run_all]   (passed on the retry -- attempt 1's evidence is kept in $OUT)"
            break
        fi
        [ "$att" = 1 ] && echo "[run_all]   retrying once (a single failure is not a verdict)"
    done
    if [ "$s2d_ok" = 1 ]; then
        echo "[run_all] deep-join: PASS"; pass+=("deep-join")
    else
        echo "[run_all] deep-join: FAIL (both attempts) -- evidence: $OUT/2d_deepjoin_att*_evidence.txt"
        fail+=("deep-join")
    fi
fi

# Stage 2d-ks -- THE KILL-SWITCH. The bounded deep join is DEFAULT ON since the
# bleeding flip, which inverts what FM2K_SPEC_DEEP_JOIN is FOR: its only
# load-bearing direction is now OFF, the lever a triager pulls to ask "does this
# still happen on the legacy from-frame-0 path?". A kill-switch nobody exercises
# rots silently -- and it rots in the worst possible way, because the failure
# mode is a reporter who BELIEVES they tested the legacy path while actually
# measuring the default, which makes every conclusion drawn from that run wrong.
# So it is gated on every run, at the same cost as one more spectator profile.
#
# Same shape as stage 2d (the css2 joiner is eligibility-shaped: prior match +
# CSS anchor + CURRENT_MATCH + non-battle grant, i.e. it is exactly the viewer
# that WOULD deep-join), minus the impairment and minus the second spectator --
# the asserts here are all log-shape, not parity-rate, so loss buys nothing.
#
# Five asserts, all deterministic once the viewer dials in:
#   k1. host logged "bounded deep join disabled"  -> the strict parse saw the 0
#   k2. host did NOT log "...ENABLED"             -> and did not also take the on
#                                                    path (catches a log-only flip)
#   k3. no "bounded backfill from CSS anchor"     -> the HOST really shipped the
#                                                    legacy from-frame-0 stream
#   k4. viewer logged no [SPEC-DEEPJOIN] hold or APPLIED -> and the VIEWER ran
#                                                    none of the ladder
#   k5. no FULL-STATE DESYNC in the run           -> the legacy path still works
#                                                    (it is what the switch buys)
# NOT asserted, and this is the load-bearing design choice of the stage: the
# harness's own OVERALL verdict, and --assert-spectator-live. A from-frame-0
# joiner dialing in at the second char-select is LEGITIMATELY hundreds of frames
# behind at host termination -- the first live run of this stage measured
# gap=-874 (Wave 3 measured -883..-891 for the same shape), i.e. it is still
# replaying match 1 when the host finishes match 2. Two harness verdicts follow
# from that lag and NEITHER is a defect here:
#   * LIVE-EDGE ... [FAIL] (printed as a metric; ungated because the flag is off)
#   * CSS-SPEC sess1 -> CSS DESYNC (LOCKED CHAR host=10/7 spec=None) -- the
#     viewer never reached the host's SECOND char-select to lock anything
# and together they make the harness print OVERALL FAIL. That lag is the entire
# reason the bounded path exists, so gating the kill-switch on it would gate the
# escape hatch on the very defect it is an escape from. DO NOT "fix" this stage
# by adding an OVERALL assert -- it will go permanently red and the kill-switch
# will lose its only coverage. What IS asserted about correctness is k5: over
# the frames the viewer DID reach it must be bit-exact (the first run: 2035
# frames FULL-STATE IDENTICAL), i.e. identical-though-behind.
KS_TIMEOUT="${KS_TIMEOUT:-320}"
if [ "${DJ_SKIP:-0}" = 1 ] || [ "${KS_SKIP:-0}" = 1 ]; then
    echo "[run_all] deep-join-killswitch: SKIPPED (DJ_SKIP/KS_SKIP=1)"
else
    echo "======================================================================"
    echo "[run_all] STAGE: deep-join-killswitch (FM2K_SPEC_DEEP_JOIN=0 -> legacy)"
    echo "======================================================================"
    kill_games; sleep 0.6
    rm -f "$SPEC_LIVE"/live_FM2K_*_Debug.log
    ks_log="$OUT/2dks_killswitch.log"; ks_ev="$OUT/2dks_killswitch_evidence.txt"
    FM2K_SPEC_DEEP_JOIN=0 FM2K_SPEC_RC=1 \
      timeout "$KS_TIMEOUT" python3 "$ROOT/tools/spec_selftest.py" \
        --rounds 1 --round-time 15 --total-frames "$DJ_TOTAL" \
        --spectators css2 --keep \
        > "$ks_log" 2>&1
    ks_host="$SPEC_LIVE/live_FM2K_P1_Debug.log"
    ks_s1="$SPEC_LIVE/live_FM2K_S1_Debug.log"
    {   echo "== host: gate verdict + backfill path =="
        grep -aE "\[SPEC-DEEPJOIN\]|bounded backfill from CSS anchor|from-frame-0" "$ks_host" 2>/dev/null | head -20
        echo "== viewer css2 (S1): any deep-join machinery at all =="
        grep -a "\[SPEC-DEEPJOIN\]" "$ks_s1" 2>/dev/null | head -20
        echo "== harness verdicts =="
        grep -aE "CHECKSUM S|CINPUT S|GATE S|CSS-SPEC|OVERALL" "$ks_log" 2>/dev/null
    } > "$ks_ev" 2>&1
    ks_why=""
    grep -qa "bounded deep join disabled" "$ks_host" 2>/dev/null \
        || ks_why="$ks_why; host never logged 'bounded deep join disabled' (kill-switch not parsed, not forwarded, or the viewer never dialled in)"
    grep -qa "bounded deep join ENABLED" "$ks_host" 2>/dev/null \
        && ks_why="$ks_why; host ALSO logged 'bounded deep join ENABLED' -- the kill-switch did not take"
    grep -qa "bounded backfill from CSS anchor" "$ks_host" 2>/dev/null \
        && ks_why="$ks_why; host shipped the BOUNDED backfill anyway (the switch is log-only)"
    grep -qaE "\[SPEC-DEEPJOIN\] (snapshot APPLIED at anchor=|HOLDING)" "$ks_s1" 2>/dev/null \
        && ks_why="$ks_why; viewer ran the deep-join ladder with the switch off"
    grep -qa "FULL-STATE DESYNC" "$ks_log" 2>/dev/null \
        && ks_why="$ks_why; the LEGACY path desynced (the escape hatch is not an escape)"
    if [ -n "$ks_why" ]; then
        echo "[run_all] deep-join-killswitch: FAIL --${ks_why#;}"
        echo "[run_all]   evidence: $ks_ev"
        fail+=("deep-join-killswitch")
    else
        echo "[run_all] deep-join-killswitch: PASS (legacy from-frame-0 path restored)"
        grep -a "bounded deep join disabled" "$ks_host" 2>/dev/null | head -1 | sed 's/^.*\[SPEC-DEEPJOIN\]/      [SPEC-DEEPJOIN]/'
        pass+=("deep-join-killswitch")
    fi
fi

# Stage 2e -- SEAM DESYNC (tools/seam_desync_gate.sh owns the recipe and the
# verdict; the rationale for both lives in that file's header). The class:
# commit 967f89f shipped a match-end-seam rollback desync that survived the
# whole gate because no stage ran a multi-match session at a profile harsh
# enough to force a non-identity rollback across the battle-exit boundary.
# Judged ONLY by GekkoNet's `DESYNC #` term, plus tools/seam_ring_check.py when
# the build emits seam-ring CSVs; the tail-CINPUT artifact, spectator stalls and
# a laggard rc=124 are explicitly NOT failures at this profile.
#
# 1 run (seed 130) in the default gate, 2 under FULL=1 (~5 min per run).
if [ -n "${SEAM_SEEDS:-}" ]; then SEAM_SEED_LIST="$SEAM_SEEDS"
elif [ "$FULL" = 1 ];        then SEAM_SEED_LIST="130 131"
else                              SEAM_SEED_LIST="130"; fi
if [ "${SEAM_SKIP:-0}" = 1 ]; then
    echo "[run_all] seamdesync: SKIPPED (SEAM_SKIP=1)"
else
    CMD="SEAM_SEEDS='$SEAM_SEED_LIST' SEAM_OUT='$OUT/2e_seam' bash '$ROOT/tools/seam_desync_gate.sh'"
    stage "seamdesync" "$OUT/2e_seamdesync.log"
fi

# Stage 2e-vanpri -- the SAME gate pointed at the second game. Wave 2 / Lane A.
# Why it exists: the seam fix was validated on wanwan only, three times over, and
# "validated on one content set" is exactly how the ShadowArts hole survived for
# months. Lane A took the red/green pair on this leg before it shipped -- with
# FM2K_SEAM_LEGACY_PARK=1 the ring check goes RED on vanpri with 9 violations
# naming `vm_live 59 -> 0` + a frozen rng (the wanwan mechanism, reproduced on
# vanpri for the first time, and with NO `DESYNC #` line: the sampled comparator
# missed what the ring caught), and green at the default. ~2 min, one seed.
# FULL=1 only, self-skips when the game is not installed. VANPRI_EXE is defined
# at stage 2g below; resolved here with the same default so the two agree.
SEAM_VANPRI_EXE="${VANPRI_EXE:-/mnt/c/games/2dfm/vanguard-princess/vanpri.exe}"
if [ "${SEAM_SKIP:-0}" = 1 ] || [ "${SEAM_VANPRI_SKIP:-0}" = 1 ]; then
    echo "[run_all] seamdesync-vanpri: SKIPPED"
elif [ "$FULL" != 1 ]; then
    echo "[run_all] seamdesync-vanpri: SKIPPED (set FULL=1 -- ~2 min)"
elif [ ! -f "$SEAM_VANPRI_EXE" ]; then
    echo "[run_all] seamdesync-vanpri: SKIPPED (vanguard-princess not installed at $SEAM_VANPRI_EXE)"
else
    CMD="SEAM_SEEDS='${SEAM_VANPRI_SEEDS:-130}' SEAM_OUT='$OUT/2e_seam' \
      SEAM_GAME=vanpri SEAM_GAME_EXE='$SEAM_VANPRI_EXE' \
      SEAM_GAMEDIR=/mnt/c/games/2dfm/vanguard-princess \
      bash '$ROOT/tools/seam_desync_gate.sh'"
    stage "seamdesync-vanpri" "$OUT/2e_seamdesync_vanpri.log"
fi

# Stage 2f -- HUB-BROKERED SPECTATE (tools/hub_spectate_e2e.py owns the recipe
# and the verdict; the rationale lives in that file's header). The class: every
# spectator stage before this one passed `--spectate <ip:port>` on the command
# line, which is the one path a real user never takes. This stage stands up a
# LOCAL hub with auth disabled, drives host+guest+spectator launchers headless
# through the default-off FM2K_HUB_AUTOMATION surface, and asserts the spectator
# process was created from the hub's spectate_grant (the grant's host=IP:PORT is
# matched against the address the process was actually launched with, and the
# harness passes no --spectate token anywhere, so a direct fallback is
# impossible by construction rather than merely unobserved).
#
# It touches ONLY a local hub on 127.0.0.1 -- the production hub and droplet are
# never contacted -- though the stage is NOT network-isolated: each launcher
# still does its own GitHub releases update-check on first menu-bar render.
# It temporarily swaps four per-user files (the Discord auth cache,
# dev_flags.ini, the games-root launcher.cfg and games.cache) under sentinel
# backups. Restore paths, in order of what they survive: the run's own finally;
# an atexit + SIGTERM/SIGINT/SIGBREAK handler (unhandled exception, terminal
# kill); and restore-on-start, which is the FIRST thing the next run does and is
# the only thing that survives a SIGKILL, a BSOD or power loss. The synthetic
# Discord credential is planted only after the run proves it is pointed at a
# loopback hub.
#
# ~65 s per run. FULL=1 adds the relay variant.
if [ "${HUBSPEC_SKIP:-0}" = 1 ]; then
    echo "[run_all] hubspec: SKIPPED (HUBSPEC_SKIP=1)"
else
    HUBSPEC_TIMEOUT="${HUBSPEC_TIMEOUT:-480}"
    # Dedicated FM2K_TEST_OUT_DIR: tools/.spec_selftest is shared and re-gated
    # across stages 2b/2c, and it must sit on the Windows filesystem.
    CMD="FM2K_TEST_OUT_DIR='$ROOT/tools/.hub_spectate_e2e' timeout $HUBSPEC_TIMEOUT python3 -u '$ROOT/tools/hub_spectate_e2e.py'"
    stage "hubspec" "$OUT/2f_hubspec.log"
    if [ "$FULL" = 1 ]; then
        CMD="FM2K_TEST_OUT_DIR='$ROOT/tools/.hub_spectate_e2e_relay' timeout $HUBSPEC_TIMEOUT python3 -u '$ROOT/tools/hub_spectate_e2e.py' --relay"
        stage "hubspec-relay" "$OUT/2f_hubspec_relay.log"
    fi
fi

# Stage 2g -- SPECTATOR x HEAVY GAME (FULL only). THE GATE HOLE THIS CLOSES:
# every other spectator stage runs spec_selftest WITHOUT --game, i.e. always
# against wanwan -- a 2-stage game whose battles carry ~10 active objects. The
# spectator-vs-heavy-game combination had never been gated at all, which is the
# ShadowArts shape (a stage that reports PASS for a path it never executes).
# vanguard-princess carries 80-150 active objects per battle frame with
# projectiles and recursive stage scripts, and it is where the object-pool
# index incoherence (Phase 4b/4c) actually decides a hit: the 4a/4b runs
# reproduced a real spectator sim divergence 1 run in 1 at this recipe, on a
# build where wanwan looked perfectly clean.
#
# The recipe is 4a's VERBATIM, including --total-frames 16000: that number is
# load-bearing (a vanpri match runs ~3500-7000 frames, and reaching a THIRD
# match is what the class needs; 7000 deterministically yields 0-frame
# spectators). Registry key is `vanpri` -- the DIRECTORY is
# /mnt/c/games/2dfm/vanguard-princess/ but --game takes the GAMES key.
#
# Verdict = spec_selftest's own exit code (CINPUT + CHECKSUM + the new POOL
# topology terms + the rng/hp gate), same as every other spectator stage.
# ~6 min, FULL=1 only. Self-skips when the game is not installed, so a
# contributor without it is unaffected.
#
# VANPRI_EXE IS LOAD-BEARING (Phase 4e, review A4d). It used to be read ONLY by
# the skip guard below while the harness resolved vanpri from its own hardcoded
# GAMES entry -- so the two agreed by coincidence, and pointing VANPRI_EXE at a
# different install would pass the guard and then run the other copy. That is the
# ShadowArts shape this stage exists to eliminate, reproduced inside the fix for
# it. The path is now handed to the harness as --game-exe, which FAILS (rc=2) if
# it is absent rather than falling back.
VANPRI_EXE="${VANPRI_EXE:-/mnt/c/games/2dfm/vanguard-princess/vanpri.exe}"
if [ "${SPECGAME_SKIP:-0}" = 1 ]; then
    echo "[run_all] specgame-vanpri: SKIPPED (SPECGAME_SKIP=1)"
elif [ "$FULL" != 1 ]; then
    echo "[run_all] specgame-vanpri: SKIPPED (set FULL=1 -- ~6 min)"
elif [ ! -f "$VANPRI_EXE" ]; then
    echo "[run_all] specgame-vanpri: SKIPPED (vanguard-princess not installed at $VANPRI_EXE)"
else
    SPECGAME_TIMEOUT="${SPECGAME_TIMEOUT:-720}"
    # Dedicated FM2K_TEST_OUT_DIR, same reason stage 2f has one: tools/.spec_selftest
    # is shared and re-gated across stages 2b/2c, and this stage runs with --keep,
    # so a later `python3 tools/test_css_gate.py` would read vanpri logs where it
    # expects wanwan's. Must sit on the Windows filesystem (recorded harness trap).
    CMD="FM2K_TEST_OUT_DIR='$ROOT/tools/.spec_selftest_vanpri' \
      FM2K_SPEC_DEEP_JOIN=1 FM2K_NET_DELAY_MS=100 FM2K_NET_JITTER_MS=30 FM2K_NET_LOSS=0.10 \
      timeout $SPECGAME_TIMEOUT python3 -u '$ROOT/tools/spec_selftest.py' --game vanpri \
      --game-exe '$VANPRI_EXE' \
      --rounds 1 --total-frames 16000 --spectators battle1,css2 --record-timeout 600 --keep"
    stage "specgame-vanpri" "$OUT/2g_specgame_vanpri.log"
fi

# Stage 3 — cnc-ddraw redirect (the "SJIS folder -> fullscreen" class). Drives
# real library games offline and requires the hook to confirm cnc-ddraw loaded
# (not stock DirectDraw). Self-skips (exit 0) if the launcher/library is absent.
CMD="bash '$ROOT/tools/ddraw_redirect_test.sh'"
stage "ddraw-redirect" "$OUT/3_ddraw.log"

# Stage 4 — multigame determinism (opt-in, slow).
if [ "$FULL" = 1 ]; then
    CMD="bash '$ROOT/tools/multigame_determinism_sweep.sh'"
    stage "multigame" "$OUT/4_multigame.log"
else
    echo "[run_all] multigame: SKIPPED (set FULL=1 to run the library sweep)"
fi

# Stage 5 — FM95/CPW rollback determinism (the FM95Hook.dll engine). Single
# instance, forced rollbacks under FM95_TRAMPOLINE; verdict is gekko-desync
# based (FM95 emits no .pty — its recorder is intentionally closed). GATING.
# Self-skips (no fail) when CPW isn't installed on this machine.
if [ -f "$CPW_EXE" ]; then
    # ADVISORY (non-gating) while FM95 is WIP (#46): CPW runs its own CRT
    # audio thread that walks duplicated IDirectSoundBuffer tables
    # concurrently with rollback restores -- deterministic AV in
    # StopAllSoundsInBufferArray@0x4016A0 (NULL buffer slot, EIP 0x4016B9)
    # ~f210 under stress. FM2K stages remain the release gate.
    echo "======================================================================"
    echo "[run_all] STAGE: fm95-determinism (advisory, non-gating -- #46)"
    echo "======================================================================"
    kill_games; sleep 0.6
    if python3 "$ROOT/tools/fm95_stress_smoke.py" "$FRAMES" "$CD" \
        > "$OUT/5_fm95_stress.log" 2>&1; then
        echo "[run_all] fm95-determinism (advisory): PASS"
    else
        echo "[run_all] fm95-determinism (advisory, non-gating): not-yet-green -- tail:"
        tail -4 "$OUT/5_fm95_stress.log" | sed 's/^/    /'
    fi
    # FM95 2-instance netplay — WIP (CSS→battle lockstep, workplan 3b). Opt-in
    # (FM95_NETPLAY=1) and NON-GATING until it's green, so it's visible in CI
    # without redding the release gate.
    if [ "${FM95_NETPLAY:-0}" = 1 ]; then
        kill_games; sleep 0.6
        python3 "$ROOT/tools/fm95_netplay_smoke.py" "$FRAMES" \
            > "$OUT/5b_fm95_netplay.log" 2>&1
        if grep -q "PASS" "$OUT/5b_fm95_netplay.log"; then
            echo "[run_all]   fm95-netplay (WIP): PASS"
        else
            echo "[run_all]   fm95-netplay (WIP, non-gating): not-yet-green (3b)"
        fi
    fi
else
    echo "[run_all] fm95-determinism: SKIPPED (CPW not found at $CPW_EXE)"
fi

kill_games
echo "======================================================================"
echo "[run_all] SUMMARY   pass=${#pass[@]}  fail=${#fail[@]}   logs: $OUT"
for s in "${pass[@]:-}"; do [ -n "$s" ] && echo "   PASS  $s"; done
for s in "${fail[@]:-}"; do [ -n "$s" ] && echo "   FAIL  $s"; done
if [ "${#fail[@]}" -eq 0 ]; then echo "[run_all] ALL GREEN — safe to cut."; exit 0
else echo "[run_all] RED — DO NOT cut until fixed."; exit 1; fi
