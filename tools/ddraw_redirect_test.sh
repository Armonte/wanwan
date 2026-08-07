#!/usr/bin/env bash
# ddraw_redirect_test.sh — regression guard for the "game goes fullscreen" class.
#
# June 2026: FM2K games in Japanese (SJIS) folders "went fullscreen" for
# playtesters. Root cause: the cnc-ddraw import redirect (DDRAW.dll -> 2DFMD.dll)
# didn't take, so the game loaded STOCK DirectDraw and rendered exclusive-
# fullscreen with no rollback overlay — and nothing flagged it. The redirect is
# folder-independent (an in-process IAT patch), so this drives REAL games from
# the launcher's own discovery root (D:\Games\fm2k) — including SJIS-folder
# titles — and requires the hook to confirm cnc-ddraw is the loaded ddraw flavor.
#
# The hook emits an explicit verdict (FM2KHook/src/core/dllmain.cpp DDrawDiag):
#   OK    -> "DDrawDiag: OK -- cnc-ddraw (2DFMD.dll) is loaded"
#   FAIL  -> "DDrawDiag: *** cnc-ddraw redirect FAILED *** ... STOCK DirectDraw"
#
# Each game is launched via `--offline <filter>`, where <filter> is a
# case-insensitive substring of the game's full exe path (so an ASCII marker in
# the path selects an SJIS-folder game without any kanji crossing the CLI).
#
# Usage:
#   tools/ddraw_redirect_test.sh                 # default set of library games
#   FILTERS="lightgreen8 AC_R7 2021" tools/ddraw_redirect_test.sh
#
# Env: FILTERS (space-separated --offline substrings), SECS (launch dwell)
set -uo pipefail
LAUNCH="/mnt/c/games/FM2K_RollbackLauncher.exe"
GAMES_ROOT="/mnt/d/Games/fm2k"
SECS="${SECS:-11}"
# Default: the actual SJIS-folder games in the library. Each substring must be an
# ASCII run present in that game's exe path. Override via FILTERS=... as needed.
#   lightgreen8 -> ...\稲歩町ダイナマイトボム\ (the game that was reported broken)
#   AC_R7       -> ...\密会緋萃伝AC_R7\
#   2021        -> ...\ヒャッキャコー2021\
FILTERS="${FILTERS:-lightgreen8 AC_R7 2021}"

# Graceful skip on machines without the deployed launcher / game library (e.g.
# headless CI) so this can sit in the unified gate without turning it red there.
if [ ! -x "$LAUNCH" ] || [ ! -d "$GAMES_ROOT" ]; then
  echo "[ddraw] SKIP — launcher or game library not present ($LAUNCH / $GAMES_ROOT)"
  exit 0
fi

kill_fm2k() {  # kill launcher + any FM2K game child by PATH (kanji /IM won't match)
  powershell.exe -NoProfile -Command \
    "Get-CimInstance Win32_Process | Where-Object { \$_.ExecutablePath -like 'D:\Games\fm2k\*' -or \$_.Name -eq 'FM2K_RollbackLauncher.exe' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force -ErrorAction SilentlyContinue }" >/dev/null 2>&1
}

# $1 = --offline filter. Finds the game's fresh hook log under the root and grades it.
run_filter() {
  local filter="$1"
  kill_fm2k; sleep 0.6
  # clear stale hook logs so we read only this run's
  find "$GAMES_ROOT" -name "FM2K_P1_Debug.log" -delete 2>/dev/null
  "$LAUNCH" --offline "$filter" >/dev/null 2>&1 &
  local pid=$!; sleep "$SECS"
  kill_fm2k; kill "$pid" 2>/dev/null; sleep 0.7
  local log; log="$(find "$GAMES_ROOT" -name "FM2K_P1_Debug.log" -newermt '90 seconds ago' 2>/dev/null | head -1)"
  if [ -z "$log" ]; then echo "  [$filter] FAIL — no game launched (filter matched nothing?)"; return 1; fi
  local dir; dir="$(dirname "$log" | sed "s#$GAMES_ROOT/##; s#/logs##")"
  local ok=1
  grep -qa "DDrawDiag: OK -- cnc-ddraw" "$log" || { ok=0; }
  grep -qa "cnc-ddraw redirect FAILED" "$log" && ok=0
  grep -qa "game_mode changed: 0 -> 3000" "$log" || ok=0
  if [ "$ok" = 1 ]; then
    echo "  [$filter] PASS — cnc-ddraw loaded + reached battle  ($dir)"; return 0
  fi
  echo "  [$filter] FAIL  ($dir)"
  grep -aE "DDrawDiag|game_mode changed" "$log" | tail -4 | sed 's/^/       /'
  return 1
}

echo "======================================================================"
echo "[ddraw] cnc-ddraw redirect regression — real games from $GAMES_ROOT"
echo "        filters: $FILTERS"
echo "======================================================================"
fails=0; ran=0
for f in $FILTERS; do run_filter "$f" || fails=$((fails+1)); ran=$((ran+1)); done
kill_fm2k
echo "----------------------------------------------------------------------"
if [ "$ran" -eq 0 ]; then echo "[ddraw] NO cases ran (empty FILTERS)"; exit 2; fi
if [ "$fails" -eq 0 ]; then echo "[ddraw] PASS ($ran/$ran) — redirect applies, no silent fullscreen."; exit 0
else echo "[ddraw] FAIL ($fails/$ran) — redirect broken; games would go fullscreen."; exit 1; fi
