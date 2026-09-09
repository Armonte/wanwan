#!/usr/bin/env bash
# Launch TWO local instances for HANDS-ON netplay testing -- no autoplay, no
# auto-CSS, no auto-terminate. You drive both windows yourself.
#
# WHY THIS EXISTS. The automated harness replaces the local input word outright
# (FM2K_TEST_AUTO_CSS / FM2K_PARITY_AUTOPLAY both assign to local_raw), so it
# can only press what it was written to press. Anything driven by a NEW button
# -- the CSS deselect on START, for one -- cannot be exercised by it without
# teaching the autoplay that button. A human at two windows presses whatever
# they like, which is also exactly what a player will do.
#
# WHAT TO TRY for the CSS deselect (2026-09-08):
#   1. Both windows: pick a character and confirm.
#   2. Press START on one window. That player un-confirms and STAYS in
#      character select; the other player is untouched.
#   3. Confirm again; the match starts normally.
#   4. Press START while UNCONFIRMED: online nothing happens (leaving CSS is a
#      session transition and needs a both-peers barrier). Offline it bails to
#      title, which is the engine's own behaviour.
#   NOTE: START is masked for the first 150 CSS frames (CSS_CONFIRM_LOCKOUT
#   strips 0x100/0x200/0x400), so give it ~1.5s after character select opens.
#
# Judge it by the logs, not by eye -- a desync is not always visible:
#   grep -c "DESYNC #[0-9]" logs/FM2K_P*_Debug.log     # must be 0
#   grep    "CSS-DESELECT]" logs/FM2K_P*_Debug.log     # the un-confirms
set -u
cd "$(dirname "$0")/.."

LAUNCHER="${FM2K_TEST_LAUNCHER:-C:/games/FM2K_RollbackLauncher.exe}"
GAME="${GAME:-C:/games/2dfm/wanwan/WonderfulWorld_ver_0946.exe}"
P1_PORT="${P1_PORT:-7100}"
P2_PORT="${P2_PORT:-7101}"
OUT="${OUT:-tools/.manual_2p}"
mkdir -p "$OUT"

# Deliberately NOT set: FM2K_PARITY_AUTOPLAY, FM2K_TEST_AUTO_CSS,
# FM2K_AUTO_TERMINATE_*. Those are what make a harness run hands-off, and all
# three would fight you for the controls.
# FM2K_TEST_BACKGROUND=0 so the windows are usable -- the harness forces 1 to
# keep the machine free during gate runs.
common='set FM2K_TEST_BACKGROUND=0'

run_peer() {  # $1=label  $2=extra args  $3=local port  $4=remote port
  local bat="$OUT/.launch_$1.bat"
  {
    echo "@echo off"
    echo "$common"
    echo "set FM2K_LOCAL_PORT=$3"
    echo "set FM2K_REMOTE_ADDR=127.0.0.1:$4"
    echo "\"$LAUNCHER\" $2 \"$GAME\" --port $3"
  } > "$bat"
  # unix2dos: cmd.exe needs CRLF or it mis-parses the last line
  sed -i 's/$/\r/' "$bat"
  echo "[manual_2p] $1: local=$3 remote=$4"
  cmd.exe /C "$(wslpath -w "$bat")" &
}

echo "[manual_2p] launcher: $LAUNCHER"
echo "[manual_2p] game:     $GAME"
run_peer p1 "--host" "$P1_PORT" "$P2_PORT"
sleep 3
run_peer p2 "--connect 127.0.0.1:$P1_PORT" "$P2_PORT" "$P1_PORT"

echo
echo "[manual_2p] two instances launched. Close both windows when done."
echo "[manual_2p] afterwards, check:"
echo "    grep -c 'DESYNC #[0-9]' /mnt/c/games/2dfm/wanwan/logs/FM2K_P*_Debug.log"
echo "    grep    'CSS-DESELECT]' /mnt/c/games/2dfm/wanwan/logs/FM2K_P*_Debug.log"
