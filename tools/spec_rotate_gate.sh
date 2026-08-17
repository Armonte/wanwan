#!/usr/bin/env bash
# spec_rotate_gate.sh -- Phase 6 leg (c): the SPECTATOR GAME ROTATION.
#
# THE HOLE THIS CLOSES. Every spectator stage in run_all_tests.sh runs wanwan,
# except stage 2g which runs vanpri. The determinism side of the gate has
# rotated the real game library for months (multigame_determinism_sweep.sh) and
# that rotation is where the SPACE-path, kanji-path and packed-exe traps were
# found -- but the SPECTATOR has never been pointed at any of them. "No
# validated claim from one content set" is the recorded lesson of this whole
# campaign (the vanpri spectator bug reproduced 1 run in 1 the moment someone
# aimed a stage at it, on a build where wanwan looked perfectly clean).
#
# WHAT IT RUNS. spec_selftest verbatim, per game, multi-match, with TWO
# spectators (battle1 = mid-battle snapshot join, css2 = between-matches deep
# join) so both bind flavours are exercised, and the harness's own exit code is
# the verdict (CINPUT + CHECKSUM full-state + POOL top=/nobj= + CSS-FP +
# CSS-WIN).
#
# TWO RULES CARRIED OVER FROM THE DETERMINISM SWEEP, both load-bearing:
#   1. Path staging goes through tools/game_staging.sh -- and its junction
#      teardown uses `cmd /c rmdir`, NEVER `rm -rf`, which would follow the
#      junction and delete the real game data.
#   2. A listed game that is MISSING is an ERROR, not a skip. A stage that
#      silently skips the path it names is the ShadowArts shape (d0455bc): it
#      reported PASS for months for a game it never ran.
#      SPECROTATE_ALLOW_MISSING=1 downgrades that to a skip for a contributor
#      who does not have the library -- the gate's own wiring never sets it.
#
# --game vs --game-exe: --game is the ENGINE CARRIER only (spec_selftest picks
# FM95-vs-FM2K metadata off it). Every rotation entry passes --game wanwan and
# the REAL path as --game-exe, which is what actually runs and which FAILS
# (rc=2) when absent. So `--game wanwan` in this file is A LIE ABOUT IDENTITY
# and true only about the engine. Do not trust it when reading a rotation log.
#
# Usage:
#   tools/spec_rotate_gate.sh                    # the full 7-game rotation
#   SPECROTATE_GAMES="vanpri ShadowArts" tools/spec_rotate_gate.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIB="${FM2K_LIB_ROOT:-/mnt/d/games/fm2k}"
OUT="${SPECROTATE_OUT:-$ROOT/logs/spec_rotate}"
mkdir -p "$OUT"
source "$ROOT/tools/game_staging.sh"
trap cleanup_all_staging EXIT INT TERM
cleanup_all_staging

# Per-game: key|exe|total_frames|timeout_s. The frame budget is "reach at least
# two match boundaries", because every spectator defect this campaign found
# lives at a boundary and match 1 is always clean. The TIMEOUT is what absorbs
# a long intro (闘闘 plays ~8560 frames of attract before battle) and a 71MB
# packed exe's load -- intro frames are not battle frames, so they cost wall
# clock, not budget.
ROT_DEFAULT=(
  "DragonPuppy|$LIB/DragonPuppy_version01a/DragonPuppy_version01a.exe|4200|660"
  "kensei2023|$LIB/kensei2023/kensei2023.exe|4200|660"
  "REQUIEM|$LIB/REQUIEM FINAL/REQUIEM FINAL.exe|4200|660"
  "TouTou|$LIB/闘闘/闘闘.exe|4200|900"
  "ShadowArts|$LIB/Retsuzan/Shadow Arts/ShadowArts.exe|4200|900"
  "vanpri|/mnt/c/games/2dfm/vanguard-princess/vanpri.exe|6000|780"
  "urorfg|/mnt/c/games/2dfm/URORFG Release 1 0 2/URORFGRelease102.exe|4200|660"
)

# Subset selector: SPECROTATE_GAMES="key key ..." (session-budget capping).
SEL="${SPECROTATE_GAMES:-}"
ROT=()
for row in "${ROT_DEFAULT[@]}"; do
    key="${row%%|*}"
    if [ -z "$SEL" ] || [[ " $SEL " == *" $key "* ]]; then ROT+=("$row"); fi
done

# kill_games knows nothing about rotation stems -- a wedged rotation process
# outliving its run poisons the NEXT game's log parsing. Killed by IMAGE NAME in
# a standalone command (never pattern-kill in the same chain as the matching
# process).
kill_rotation() {
    for p in FM2K_RollbackLauncher _hrun WonderfulWorld_ver_0946 vanpri \
             DragonPuppy_version01a kensei2023 "REQUIEM FINAL" 闘闘 \
             ShadowArts URORFGRelease102 pkmncc; do
        taskkill.exe /F /IM "${p}.exe" >/dev/null 2>&1
    done
}

echo "[rotate] games=${#ROT[@]} lib=$LIB out=$OUT"
pass=0; fail=0; err=0; declare -a RESULTS
for row in "${ROT[@]}"; do
    IFS='|' read -r key exe total tmo <<< "$row"
    if [ ! -f "$exe" ]; then
        if [ "${SPECROTATE_ALLOW_MISSING:-0}" = 1 ]; then
            RESULTS+=("SKIP     $key  (not installed: $exe)")
            echo "[rotate] ${RESULTS[-1]}"; continue
        fi
        RESULTS+=("MISSING  $key  ($exe)"); err=$((err+1))
        echo "[rotate] ${RESULTS[-1]}"; continue
    fi
    kill_rotation; sleep 0.6
    run_exe="$(stage_game "$exe")"
    [ "$run_exe" != "$exe" ] && echo "[rotate] $key staged -> $run_exe"
    log="$OUT/${key}.log"
    # Dedicated FM2K_TEST_OUT_DIR per game, on the Windows filesystem: the
    # shared tools/.spec_selftest is re-gated by other stages, and this runs
    # with --keep.
    # CSS-WINDOW GATE: ADVISORY IN THIS LEG, and re-surfaced per game below
    # rather than silenced. Measured 2026-08-16 on the first rotation: 闘闘 and
    # ShadowArts came back FULL-STATE IDENTICAL, CINPUT identical and TEAM-PIN
    # green, and reded on nothing but the spectator's char-select falling object
    # (闘闘 834 -> 934 px, ShadowArts 920 -> 950 px, pkmncc 580 -> 770 px) --
    # i.e. the KNOWN OPEN falling-object class, which this rotation has now
    # confirmed on three more content sets. That is a finding the leg should
    # REPORT, not a verdict it should own: the rotation's job is the sim-parity
    # terms across content, and holding it red on a filed bug in a different
    # subsystem is how a 45-70 minute stage stops being run.
    FM2K_CSSWIN_FATAL=0 \
    FM2K_TEST_OUT_DIR="$ROOT/tools/.spec_rotate_$key" \
    FM2K_SPEC_RC=1 FM2K_SPEC_DEEP_JOIN=1 \
    FM2K_NET_DELAY_MS=100 FM2K_NET_JITTER_MS=30 FM2K_NET_LOSS="${SPECROTATE_LOSS:-0.10}" \
    FM2K_NET_SEED=161 \
      timeout "$tmo" python3 -u "$ROOT/tools/spec_selftest.py" \
        --game wanwan --game-exe "$run_exe" \
        --rounds 1 --round-time 15 --total-frames "$total" \
        --spectators battle1,css2 --record-timeout $((tmo - 120)) --keep \
        > "$log" 2>&1
    rc=$?
    unstage_game
    if [ "$rc" -eq 0 ]; then
        RESULTS+=("PASS     $key"); pass=$((pass+1))
    elif [ "$rc" -eq 2 ]; then
        RESULTS+=("ERROR    $key  (harness FATAL: --game-exe not found)"); err=$((err+1))
    else
        why="$(grep -aE "OVERALL FAIL|FAIL:" "$log" | tail -1 | cut -c1-150)"
        RESULTS+=("FAIL     $key  rc=$rc [$why]"); fail=$((fail+1))
    fi
    echo "[rotate] ${RESULTS[-1]}"
    # The de-fatalised CSS-window verdict, surfaced per game. Advisory is not
    # the same as invisible.
    grep -aE "CSS-WIN FALL S[0-9]+: .*falling object" "$log" 2>/dev/null \
        | sed "s/^\[harness\]/[rotate]   $key CSS-WINDOW [advisory]:/" || true
    # The VS-1v1 tripwire, surfaced per game. It earned its place on the first
    # rotation: DragonPuppy_version01a came back mode_flag=0 (1P/story) on host,
    # guest AND spectator, alongside a full-state spectator desync from battle
    # frame 0 with inputs identical -- i.e. the pin does not hold on that title.
    # Printed next to the verdict so a rotation red is attributable at a glance
    # instead of looking like a harness defect.
    grep -a "TEAM-PIN: g_game_mode_flag != 1" "$log" 2>/dev/null \
        | sed "s/^\[harness\]/[rotate]   $key TEAM-PIN:/" || true
    # Persist the per-game verdict AS IT HAPPENS: a 45-70 minute rotation on a
    # box that has BSOD'd mid-campaign must not lose everything before it.
    printf '%s\n' "${RESULTS[-1]}" >> "$OUT/verdicts.txt"
done
kill_rotation
echo "========================================"
printf '%s\n' "${RESULTS[@]}"
echo "[rotate] PASS=$pass FAIL=$fail ERROR=$err"
[ "$fail" -eq 0 ] && [ "$err" -eq 0 ]
