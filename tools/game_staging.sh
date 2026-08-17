#!/usr/bin/env bash
# game_staging.sh -- SHARED path-staging helpers for library-wide game sweeps.
# Sourced (never executed) by tools/multigame_determinism_sweep.sh and
# tools/spec_rotate_gate.sh.
#
# WHY THIS FILE EXISTS. These four functions were written for the multigame
# determinism sweep and encode traps that cost real debugging time:
#
#   * SPACE / non-ASCII paths -- the WSL->Windows launcher invocation mangles
#     args with spaces, and FM2K derives <stem>.kgt (the character-system data)
#     from the exe stem via GetModuleFileNameA, so a staged copy needs BOTH the
#     exe and the .kgt renamed to the same ASCII stem. Staging refuses when no
#     .kgt exists rather than risking a half-staged game.
#   * JUNCTION TEARDOWN MUST USE `cmd /c rmdir`. `rm -rf` follows an NTFS
#     junction and DELETES THE REAL GAME DATA. This is the single most
#     destructive trap in the sweep, which is why the teardown, the
#     `trap ... EXIT INT TERM` and the unconditional startup sweep all have to
#     travel together with the staging.
#
# When the spectator rotation leg needed the same handling, copying the
# functions would have created a second, drifting copy of exactly the list that
# has already disagreed with itself once (the ShadowArts hole was two
# independent game lists). One source instead.
#
# Contract for a caller:
#   LIB=<root whose volume holds the junctions>   # required before sourcing
#   source tools/game_staging.sh
#   trap cleanup_all_staging EXIT INT TERM
#   cleanup_all_staging                 # clear debris from a prior killed run
#   run_exe="$(stage_game "$exe")"      # ASCII-clean path to run
#   ... run ...
#   unstage_game

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
