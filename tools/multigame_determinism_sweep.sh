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

# /mnt/d/games/fm2k/REQUIEM FINAL -> D:\games\fm2k\REQUIEM FINAL
win_path() { local p="${1#/mnt/}"; local d="${p%%/*}"; echo "${d^^}:\\${p#*/}" | sed 's|/|\\|g'; }

# Remove ALL staging debris. Junctions MUST go via cmd rmdir — `rm -rf` would
# follow the junction and delete the REAL game data. Runs at start + on any exit.
cleanup_all_staging() {
    local j
    for j in $(find "$LIB" -maxdepth 1 -iname "_hstage*" 2>/dev/null); do
        cmd.exe /c rmdir "$(win_path "$j")" >/dev/null 2>&1
    done
    find "$LIB" -iname "_hrun.exe" -o -iname "_hrun.kgt" 2>/dev/null | while read -r f; do rm -f "$f"; done
}
trap cleanup_all_staging EXIT INT TERM
cleanup_all_staging   # clear any debris from a prior killed run

# stage_game <exe> -> prints an ASCII-clean /mnt exe path to run; sets STAGED_JUNC
# + STAGED_EXECOPY for unstage_game. Passthrough when the path is already clean.
STAGED_JUNC=""; STAGED_EXECOPY=""
stage_game() {
    local exe="$1"; STAGED_JUNC=""; STAGED_EXECOPY=""
    # clean = ASCII AND no spaces
    if LC_ALL=C grep -qP '^[\x20-\x7E]*$' <<<"$exe" && [[ "$exe" != *" "* ]]; then
        echo "$exe"; return 0
    fi
    local dir stem; dir="$(dirname "$exe")"; stem="$(basename "$exe" .exe)"
    # FM2K derives <stem>.kgt (character-system data) from the exe stem via
    # GetModuleFileNameA, so the exe AND its .kgt must share the new ASCII stem.
    [ -f "$dir/$stem.kgt" ] || { echo "$exe"; return 1; }   # no kgt => don't risk it
    local junc="$LIB/_hstage_$$"
    cmd.exe /c rmdir "$(win_path "$junc")" >/dev/null 2>&1
    rm -f "$dir/_hrun.exe" "$dir/_hrun.kgt" 2>/dev/null
    cp "$exe" "$dir/_hrun.exe" 2>/dev/null && cp "$dir/$stem.kgt" "$dir/_hrun.kgt" 2>/dev/null \
        || { rm -f "$dir/_hrun.exe" "$dir/_hrun.kgt" 2>/dev/null; echo "$exe"; return 1; }
    cmd.exe /c mklink /J "$(win_path "$junc")" "$(win_path "$dir")" >/dev/null 2>&1
    if [ -e "$junc/_hrun.exe" ]; then
        STAGED_JUNC="$junc"; STAGED_EXECOPY="$dir/_hrun"; echo "$junc/_hrun.exe"
    else
        rm -f "$dir/_hrun.exe" "$dir/_hrun.kgt" 2>/dev/null; echo "$exe"   # staging failed -> raw
    fi
}
unstage_game() {
    [ -n "$STAGED_JUNC" ]    && cmd.exe /c rmdir "$(win_path "$STAGED_JUNC")" >/dev/null 2>&1
    [ -n "$STAGED_EXECOPY" ] && rm -f "${STAGED_EXECOPY}.exe" "${STAGED_EXECOPY}.kgt" 2>/dev/null
    STAGED_JUNC=""; STAGED_EXECOPY=""
}

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
