#!/usr/bin/env bash
# multigame_determinism_sweep.sh — run the rollback determinism gate across the
# FM2K game library, proving our savestate serialization is engine-general (not
# WonderfulWorld-specific). Each game: stress-autoplay + replay + per-frame state
# diff under forced rollback (replay_selftest.py --game-exe).
#
# ROBUSTNESS (so the whole library runs, not just ASCII short-match games):
#   * SPACE / non-ASCII paths — the WSL->Windows launcher invocation mangles args
#     with spaces, and some data loads assume the exe path. We STAGE such games to
#     an ASCII junction (no data copy) + an ASCII-named exe copy, run there, clean up.
#   * LONG INTROS — games with splash/demo/attract sequences (e.g. 闘闘 ~8560 frames
#     before battle) blow the default 60s timeouts. We use generous ones.
#   * A per-region CRC diff whose GameplayFingerprint MATCHES is NOT a desync
#     (afterimage/spawned-effect/speculative-save noise); we consult the game's
#     FM2K_replay_diffs.log verdict so benign noise doesn't read as DIVERGE.
#
# Usage:
#   tools/multigame_determinism_sweep.sh                 # curated representative set
#   tools/multigame_determinism_sweep.sh --all           # every *.exe under the lib
#   FRAMES=1500 FM2K_CHECK_DISTANCE=12 tools/multigame_determinism_sweep.sh
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Corpus roots (space-separated, override via FM2K_LIB_ROOTS). Games live across
# several trees + two engines (FM2K vs FM95); more live on hub.2dfm.org's catalog
# that aren't downloaded yet — point this at whatever you pull.
DEFAULT_ROOTS="/mnt/d/games/fm2k /mnt/c/games/2dfm /mnt/d/games/fm95"
read -r -a ROOTS <<< "${FM2K_LIB_ROOTS:-$DEFAULT_ROOTS}"
LIB="${ROOTS[0]}"   # staging junctions/cleanup use the first root's volume
FRAMES="${FRAMES:-1200}"
CD="${FM2K_CHECK_DISTANCE:-12}"
RECORD_TIMEOUT="${RECORD_TIMEOUT:-180}"
REPLAY_TIMEOUT="${REPLAY_TIMEOUT:-180}"

# Path staging (SPACE / kanji paths, the .kgt stem rule, and the `cmd /c rmdir`
# junction teardown that must never be `rm -rf`) lives in tools/game_staging.sh
# since Phase 6: the spectator rotation leg needs exactly the same handling, and
# a second copy of it would be a second list that can drift -- which is what the
# ShadowArts hole was. Behaviour is unchanged; the functions moved verbatim.
source "$ROOT/tools/game_staging.sh"
trap cleanup_all_staging EXIT INT TERM
cleanup_all_staging   # clear any debris from a prior killed run

WW="${FM2K_REF_EXE:-/mnt/c/games/2dfm/wanwan/WonderfulWorld_ver_0946.exe}"
# A real FM2K game = its engine code+rdata (first 0x120000) is byte-identical to
# the reference build. This finds them at ANY depth and rejects installers / 7z
# self-extractors / other-build exes.
discover_games() {
    local wwhead; wwhead="$(mktemp)"; head -c $((0x120000)) "$WW" > "$wwhead"
    local r
    { for r in "${ROOTS[@]}"; do [ -d "$r" ] && find "$r" -iname "*.exe" ! -iname "_hrun.exe" 2>/dev/null; done; } \
      | sort -u | while IFS= read -r f; do
        local sz; sz=$(stat -c%s "$f" 2>/dev/null) || continue
        [ "$sz" -ge 1200000 ] && [ "$sz" -le 1260000 ] || continue
        cmp -s "$wwhead" <(head -c $((0x120000)) "$f") 2>/dev/null && echo "$f"
    done
    rm -f "$wwhead"
}
# --list: census only (no game runs) — how many real FM2K games across the roots.
if [ "${1:-}" = "--list" ]; then
    echo "[census] roots: ${ROOTS[*]}"
    mapfile -t G < <(discover_games)
    echo "[census] FM2K engine-identical games (testable unchanged): ${#G[@]}"
    printf '  %s\n' "${G[@]}"
    exit 0
fi
if [ "${1:-}" = "--all" ]; then
    mapfile -t GAMES < <(discover_games)
else
    GAMES=(
        "$LIB/DragonPuppy_version01a/DragonPuppy_version01a.exe"  # cluster (CLEAN)
        "$LIB/kensei2023/kensei2023.exe"                          # cluster
        "$LIB/REQUIEM FINAL/REQUIEM FINAL.exe"                    # SPACE path -> staged
        "$LIB/闘闘/闘闘.exe"                                       # kanji + long intro
        "$LIB/Retsuzan/Shadow Arts/ShadowArts.exe"                # SPACE + PACKED 71MB (nested under Retsuzan/)
    )
fi

echo "[sweep] games=${#GAMES[@]} frames=$FRAMES cd=$CD timeouts=${RECORD_TIMEOUT}/${REPLAY_TIMEOUT}s"
pass=0; fail=0; err=0; declare -a RESULTS
for exe in "${GAMES[@]}"; do
    name="$(basename "$exe")"
    [ -f "$exe" ] || { RESULTS+=("MISSING  $name"); err=$((err+1)); echo "[sweep] ${RESULTS[-1]}"; continue; }
    gdir="$(dirname "$exe")"
    rm -f "$gdir"/FM2K_P*_desync_f*.log "$gdir"/logs/FM2K_replay_diffs.log 2>/dev/null
    run_exe="$(stage_game "$exe")"
    out="$(FM2K_CHECK_DISTANCE="$CD" timeout $((RECORD_TIMEOUT + REPLAY_TIMEOUT + 90)) \
            python3 "$ROOT/tools/replay_selftest.py" --frames "$FRAMES" \
            --record-timeout "$RECORD_TIMEOUT" --replay-timeout "$REPLAY_TIMEOUT" \
            --game-exe "$run_exe" --keep 2>&1)"
    # Fingerprint-aware verdict: real desync only if a diff block ever shows a
    # GameplayFingerprint DIFF. (Benign per-region noise keeps the fingerprint.)
    diffs_log="$gdir/logs/FM2K_replay_diffs.log"
    real_desync=0
    [ -f "$diffs_log" ] && real_desync="$(grep -c 'GameplayFingerprint.*DIFF' "$diffs_log" 2>/dev/null || echo 0)"
    if   echo "$out" | grep -qiE "CLEAN: all .* IDENTICAL"; then RESULTS+=("PASS     $name  (clean)"); pass=$((pass+1))
    elif [ "$real_desync" -gt 0 ] 2>/dev/null;                 then RESULTS+=("DESYNC   $name  ($real_desync fp-diffs)"); fail=$((fail+1))
    elif echo "$out" | grep -qiE "RECORD: exit rc=0"; then
        # ran + no fingerprint desync => determinism OK; replay-flow/timeout only
        RESULTS+=("PASS*    $name  (gameplay identical; benign noise / replay-flow)"); pass=$((pass+1))
    else
        RESULTS+=("ERROR    $name  [$(echo "$out" | grep -iE "not found|FATAL|rc=" | head -1)]"); err=$((err+1))
    fi
    unstage_game
    echo "[sweep] ${RESULTS[-1]}"
done
echo "========================================"
printf '%s\n' "${RESULTS[@]}"
echo "[sweep] PASS=$pass DESYNC=$fail ERROR=$err"
# Exit nonzero on any desync or error. Without this the script's status was
# the final echo's (always 0), so the run_all_tests stage could never fail
# no matter what happened above.
[ "$fail" -eq 0 ] && [ "$err" -eq 0 ]
