#!/usr/bin/env bash
# Run one side of the FM2K parity harness and write its dump.
#   run_parity.sh <port|orig> <run-name> [max_ticks] [raw_from-raw_to]
# Output: $RUNS/<run-name>/<side>.fm2kpar (+ .allocs.tsv, orig: .dll.log).
#
# Default game: WonderfulWorld (stock KGT2nd_GAME runtime, the exe our IDB is built on).
#   orig: WonderfulWorld_ver_0945.exe, started suspended by fm2k_parity_inject.exe with fm2k_parity.dll
#   port: the parity-branch build copied over WonderfulWorld_port.exe (loads WonderfulWorld_port.kgt,
#         a copy of the original .kgt: the engine names the .kgt after the exe up to the first '.')
# Launched through PowerShell Start-Process -WorkingDirectory (cmd.exe from WSL lands in a UNC cwd).
# Runs in background mode (no focus steal, forced window mode), one side at a time, game.ini is
# restored afterwards, and the side's own image is taskkill'ed at the end (no window or dialog left).
# Env overrides: GAME_DIR, ORIG_EXE, PORT_NAME, PORT_EXE, RUNS, BATTLE (default 1), STORY
# (FM2K_PARITY_AUTOPLAY_STORY), CSS_DWELL, TITLE_SKIP, CLOCK (real|lockstep), TIMEOUT (s),
# FRAME_HASH=1 (both sides: FM2K_FRAME_HASH -> $RUNS/<run-name>/<side>.framehash), FRAME_DUMP=a-b
# (FM2K_FRAME_DUMP: frames a..b also as <side>.framehash.<frame>.bmp), PASS_ENV="VAR ..."
# (forward those variables as they are set here; paths in them must already be Windows paths).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SIDE="$1"; NAME="$2"; MAX="${3:-6000}"; RAW="${4:-}"
FM2K_PORT_DIR="${FM2K_PORT_DIR:-$(cd "$(dirname "$0")/../../.." && pwd)/fm2k-port}"   # sibling checkout of the port
GAME_DIR="${GAME_DIR:-${FM2K_TEST_GAMES_DIR:-/mnt/d/games/fm2k-port-test}/ww_loose}"   # a TEST COPY: the harness copies the port exe and rewrites game.ini there
ORIG_EXE="${ORIG_EXE:-WonderfulWorld_ver_0945.exe}"
PORT_NAME="${PORT_NAME:-WonderfulWorld_port.exe}"
# the port build under test: the main fm2k-port checkout (the parity worktree's own build goes stale)
PORT_EXE="${PORT_EXE:-$FM2K_PORT_DIR/build/KGT2nd_GAME.exe}"
RUNS="${RUNS:-$FM2K_PORT_DIR/parity_runs/harness}"   # original-side baselines: $FM2K_PORT_DIR/parity_runs/baseline
OUT="$RUNS/$NAME"; mkdir -p "$OUT"
win() { wslpath -w "$1"; }

# THE GAME LIBRARY IS READ-ONLY (owner rule). This harness writes into GAME_DIR (the port exe, game.ini), and on
# 2026-10-17 a run against the library itself with the port named like the original overwrote two WonderfulWorld exes
# there (restored 2026-10-20 from their backups). Refuse a GAME_DIR inside FM2K_GAMES_DIR (the library) unless it is
# the explicit test root, and never let the port take the original's name.
if [ -n "${FM2K_GAMES_DIR:-}" ]; then
  gd="$(realpath -m "$GAME_DIR")"; lib="$(realpath -m "$FM2K_GAMES_DIR")"
  case "$gd/" in "$lib"/*) echo "run_parity: GAME_DIR $GAME_DIR is inside the read-only library $FM2K_GAMES_DIR -- use a test copy" >&2; exit 2 ;; esac
fi
case "$GAME_DIR" in */games/fm2k|*/games/fm2k/*) echo "run_parity: GAME_DIR $GAME_DIR looks like the read-only library -- use a test copy" >&2; exit 2 ;; esac
[ "$PORT_NAME" = "$ORIG_EXE" ] && { echo "run_parity: PORT_NAME equals ORIG_EXE ($ORIG_EXE): the port would overwrite the original" >&2; exit 2; }

cp -f "$GAME_DIR/game.ini" "$OUT/game.ini.before"
if [ "$SIDE" = port ]; then
  cp -f "$PORT_EXE" "$GAME_DIR/$PORT_NAME"
  IMG="$PORT_NAME"
  RUN_EXE="$(win "$GAME_DIR/$PORT_NAME")"; RUN_ARGS=""
else
  IMG="$ORIG_EXE"
  RUN_EXE="$(win "$HERE/build/fm2k_parity_inject.exe")"
  RUN_ARGS="'\"$(win "$GAME_DIR/$ORIG_EXE")\"','\"$(win "$HERE/build/fm2k_parity.dll")\"'"
fi
PS="$OUT/run_$SIDE.ps1"
{
  echo "\$env:FM2K_PARITY_AUTOPLAY='1'"
  echo "\$env:FM2K_PARITY_AUTOPLAY_BATTLE='${BATTLE:-1}'"
  [ -n "${STORY:-}" ] && echo "\$env:FM2K_PARITY_AUTOPLAY_STORY='$STORY'"
  [ -n "${CSS_DWELL:-}" ] && echo "\$env:FM2K_AUTOPLAY_CSS_DWELL='$CSS_DWELL'"
  [ -n "${TITLE_SKIP:-}" ] && echo "\$env:FM2K_AUTO_TITLE_SKIP='$TITLE_SKIP'"
  [ -n "${CLOCK:-}" ] && echo "\$env:FM2K_PARITY_CLOCK='$CLOCK'"
  [ -n "${FRAME_HASH:-}" ] && echo "\$env:FM2K_FRAME_HASH='$(win "$OUT")\\$SIDE.framehash'"
  [ -n "${FRAME_DUMP:-}" ] && echo "\$env:FM2K_FRAME_DUMP='$FRAME_DUMP'"
  for v in ${PASS_ENV:-}; do echo "\$env:$v='${!v:-}'"; done
  echo "\$env:FM2K_TEST_BACKGROUND='1'"
  echo "\$env:FM2K_PARITY_MAX_FRAMES='$MAX'"
  echo "\$env:FM2K_PARITY_DUMP_PATH='$(win "$OUT")\\$SIDE.fm2kpar'"
  [ -n "$RAW" ] && echo "\$env:FM2K_PARITY_DUMP_FRAMES='$RAW'"
  if [ -n "$RUN_ARGS" ]; then
    echo "Start-Process -FilePath '$RUN_EXE' -ArgumentList $RUN_ARGS -WorkingDirectory '$(win "$GAME_DIR")' -WindowStyle Hidden -Wait"
  else
    echo "Start-Process -FilePath '$RUN_EXE' -WorkingDirectory '$(win "$GAME_DIR")' -Wait"
  fi
} > "$PS"
TIMEOUT="${TIMEOUT:-$(( MAX / 50 + 90 ))}"
t0=$(date +%s)
timeout "$TIMEOUT" powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(win "$PS")" \
  || echo "[$SIDE] timed out or failed after ${TIMEOUT}s"
taskkill.exe /F /IM "$IMG" >/dev/null 2>&1 || true
# the injector exits with its game: killing every fm2k_parity_inject.exe here ended another run's original side
# (two runs at once: the other side's Start-Process -Wait returned and its game was taskkill'ed mid-run)
cp -f "$OUT/game.ini.before" "$GAME_DIR/game.ini"
echo "[$SIDE] done in $(( $(date +%s) - t0 ))s -> $OUT/$SIDE.fm2kpar ($(stat -c %s "$OUT/$SIDE.fm2kpar" 2>/dev/null || echo 0) bytes)"
