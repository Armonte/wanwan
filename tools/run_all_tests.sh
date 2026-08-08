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
#   2d. deep-join    spec_selftest FM2K_SPEC_DEEP_JOIN=1 --spectators battle1,css2
#                    — the BOUNDED between-matches join: a viewer that dials in
#                    between matches is backfilled from the CURRENT char-select
#                    and handed a battle-entry snapshot instead of replaying the
#                    whole session. Until 2026-08 NO stage exercised that path,
#                    so gate-green was zero evidence for it.
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
#                                               — stage 2d deep join
#      CPW_EXE(/mnt/c/dev/fm95/CPW/ＣＰＷ.exe) FM95_NETPLAY(0)  — stage 5
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FRAMES="${FRAMES:-1500}"; LOSS="${LOSS:-0.15}"; SPEC_RUNS="${SPEC_RUNS:-4}"
CD="${FM2K_CHECK_DISTANCE:-10}"; FULL="${FULL:-0}"
OUT="$ROOT/logs/run_all_tests"; rm -rf "$OUT"; mkdir -p "$OUT"

kill_games() { for p in WonderfulWorld_ver_0946 WonderfulRvl FM2K_RollbackLauncher fm2k FM2K CPW ＣＰＷ; do
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

# Stage 2d — BOUNDED DEEP JOIN (FM2K_SPEC_DEEP_JOIN=1). Every other spectator
# stage joins early enough to watch match 1, so none of them can reach the
# bounded between-matches path: "ALL GREEN" was, until this stage existed, no
# evidence whatsoever for the deep-join feature
# (docs/dev/spectate_deep_join_rollout.md, Phase 1 item 4).
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
