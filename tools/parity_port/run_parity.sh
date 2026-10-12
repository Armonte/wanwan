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
# (FM2K_PARITY_AUTOPLAY_STORY), CSS_DWELL, TITLE_SKIP, CLOCK (real|lockstep), TIMEOUT (s).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SIDE="$1"; NAME="$2"; MAX="${3:-6000}"; RAW="${4:-}"
GAME_DIR="${GAME_DIR:-/mnt/d/games/fm2k/Project Atsuki/WonderfulWorld}"
ORIG_EXE="${ORIG_EXE:-WonderfulWorld_ver_0945.exe}"
PORT_NAME="${PORT_NAME:-WonderfulWorld_port.exe}"
# the port build under test: the main fm2k-port checkout (the parity worktree's own build goes stale)
PORT_EXE="${PORT_EXE:-/mnt/d/dev/fm2k-port/build/KGT2nd_GAME.exe}"
RUNS="${RUNS:-/mnt/d/dev/fm2k-port-parity/parity_runs}"
OUT="$RUNS/$NAME"; mkdir -p "$OUT"
win() { wslpath -w "$1"; }

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
taskkill.exe /F /IM fm2k_parity_inject.exe >/dev/null 2>&1 || true
cp -f "$OUT/game.ini.before" "$GAME_DIR/game.ini"
echo "[$SIDE] done in $(( $(date +%s) - t0 ))s -> $OUT/$SIDE.fm2kpar ($(stat -c %s "$OUT/$SIDE.fm2kpar" 2>/dev/null || echo 0) bytes)"
