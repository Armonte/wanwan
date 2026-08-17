#!/bin/bash
# Manual real-netplay replay-determinism recorder.
#
# Spawns TWO launcher instances in loopback (real GekkoSession, no stress)
# with parity capture wired. You play the match manually:
#   1. Click through title on BOTH instances
#   2. Pick characters in CSS on BOTH (any chars, doesn't matter)
#   3. Play a round or two
#   4. Let MATCH_END fire OR close the windows
#
# After both windows close, the harness pulls the host's .fm2krep + parity
# stream and runs --replay against it. parity_diff reports any divergence.
#
# This is the FAITHFUL "real users" replay-determinism test — the stress-
# session harness (replay_selftest.py) skips real-netplay's predictive
# rollback, which is what production users actually hit.

set -e
LAUNCHER="C:/games/FM2K_RollbackLauncher.exe"
# FM2K_TEST_BACKGROUND: ON BY DEFAULT (opt out with FM2K_TEST_BACKGROUND=0).
# This script builds its own `cmd.exe /C "set K=V&& ..."` lines, so the variable
# is injected as another `set` below rather than exported (a plain export does
# not cross the WSL->Windows boundary).
BG="${FM2K_TEST_BACKGROUND:-1}"
GAME="C:/games/2dfm/wanwan/WonderfulWorld_ver_0946.exe"
OUT_DIR="/mnt/c/dev/wanwan/tools/.netplay_manual"
mkdir -p "$OUT_DIR"

P1_PTY="C:/dev/wanwan/tools/.netplay_manual/p1.pty"
P2_PTY="C:/dev/wanwan/tools/.netplay_manual/p2.pty"

rm -f "$OUT_DIR"/p1.pty "$OUT_DIR"/p2.pty

echo "==============================================================="
echo "Manual replay-record harness"
echo "==============================================================="
echo ""
echo "Two launcher windows will open. Steps:"
echo "  1. On P1 (host) and P2 (joiner): mash through title → CSS"
echo "  2. Pick any character on both sides"
echo "  3. Play a round (let RNG flow). Then close both windows."
echo ""
echo "Replays land in /mnt/c/games/2dfm/wanwan/replays/ as *.fm2krep."
echo "Parity captures: $P1_PTY + $P2_PTY"
echo ""
echo "Spawning the two clients now..."

# Start P1 (host) and P2 (joiner) in background. cmd.exe is needed so the
# Windows process sees the env vars (WSL Popen doesn't propagate to native).
P1_LOG="$OUT_DIR/p1.log"
P2_LOG="$OUT_DIR/p2.log"

cmd.exe /C "set FM2K_TEST_BACKGROUND=$BG&& set FM2K_PARITY_RECORD_PATH=$P1_PTY&& set FM2K_LOCAL_PORT=7000&& set FM2K_REMOTE_ADDR=127.0.0.1:7001&& $LAUNCHER --host $GAME --port 7000" > "$P1_LOG" 2>&1 &
P1_PID=$!
echo "P1 spawned: pid=$P1_PID  log=$P1_LOG"

sleep 1.0

cmd.exe /C "set FM2K_TEST_BACKGROUND=$BG&& set FM2K_PARITY_RECORD_PATH=$P2_PTY&& set FM2K_LOCAL_PORT=7001&& set FM2K_REMOTE_ADDR=127.0.0.1:7000&& $LAUNCHER --connect 127.0.0.1:7000 $GAME --port 7001" > "$P2_LOG" 2>&1 &
P2_PID=$!
echo "P2 spawned: pid=$P2_PID  log=$P2_LOG"

echo ""
echo "Waiting for BOTH launchers to exit (close the windows when done)..."
wait $P1_PID
wait $P2_PID

echo ""
echo "Both launchers exited."
echo "P1 parity: $(ls -la $OUT_DIR/p1.pty 2>/dev/null || echo MISSING)"
echo "P2 parity: $(ls -la $OUT_DIR/p2.pty 2>/dev/null || echo MISSING)"
echo "Latest .fm2krep: $(ls -t /mnt/c/games/2dfm/wanwan/replays/*.fm2krep | head -1)"
echo ""
echo "Now run:"
echo "  python3 tools/replay_netplay_diff.py <path-to-host-.fm2krep>"
echo "to replay it and diff against the P1 parity stream."
