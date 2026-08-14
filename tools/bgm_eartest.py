#!/usr/bin/env python3
"""Minimal 2-player autoplay netplay for BGM ear-testing.

Host + peer only (2 audio streams, same synced track — NOT the 3-4 instance
parity harness), short rounds, multi-match so it cycles battle -> match end ->
CSS -> rematch a few times. Rollback IS active (netplay), so this exercises the
v2 BGM layer (desired/actual reconcile + savestate + battle-end stop) — unlike
--offline, which plays BGM natively. Audio is ON. No assertions; you just listen.

Usage:  python3 tools/bgm_eartest.py [game] [round_time] [total_frames]
        game = wanwan (default) | vanpri | urorfg | <key in spec_selftest.GAMES>
"""
import sys, time, threading
sys.path.insert(0, "tools")
import spec_selftest as s

if __name__ == "__main__":
    game_key    = sys.argv[1] if len(sys.argv) > 1 else "wanwan"
    round_time  = sys.argv[2] if len(sys.argv) > 2 else "10"
    total_frames= sys.argv[3] if len(sys.argv) > 3 else "3000"

    game = s.GAMES[game_key]
    if not game.exists():
        print(f"game not found: {game}"); sys.exit(2)
    game_arg = s.to_win(game)
    print(f"[eartest] game={game_key} ({game.name}) round_time={round_time}s "
          f"total_frames={total_frames} (multi-match battle->CSS->rematch)")

    s.kill_strays(); time.sleep(1.0)

    # Autoplay + multi-match recipe (mirrors spec_selftest common_env, minus the
    # parity/trace machinery — none of it is needed to just hear the audio).
    common = {
        "FM2K_PARITY_AUTOPLAY":        "1",
        "FM2K_PARITY_AUTOPLAY_BATTLE": "1",
        "FM2K_AUTO_TITLE_SKIP":        "1",
        "FM2K_TEST_AUTO_CSS":          "0,0,0,0,0",
        "FM2K_TEST_ROUNDS":            "1",            # 1 round/match -> fast match end
        "FM2K_TEST_ROUND_TIME":        str(round_time),
        "FM2K_AUTO_TERMINATE_TOTAL":   str(total_frames),
        "FM2K_AUTOPLAY_CSS_DWELL":     "8",
        # BGM event trace -> game debug log (record/defer/immediate/sync-play/stop).
        "FM2K_BGM_TRACE":              __import__("os").environ.get("FM2K_BGM_TRACE", "1"),
    }
    p1_env = {**common, "FM2K_LOCAL_PORT": str(s.P1_PORT),
              "FM2K_REMOTE_ADDR": f"127.0.0.1:{s.P2_PORT}"}
    p2_env = {**common, "FM2K_LOCAL_PORT": str(s.P2_PORT),
              "FM2K_REMOTE_ADDR": f"127.0.0.1:{s.P1_PORT}"}
    p1_args = [str(s.LAUNCHER), "--host", game_arg, "--port", str(s.P1_PORT)]
    p2_args = [str(s.LAUNCHER), "--connect", f"127.0.0.1:{s.P1_PORT}", game_arg,
               "--port", str(s.P2_PORT)]

    # launch() blocks until the proc exits (host auto-terminates at total-frames);
    # run both in daemon threads so they play concurrently.
    threading.Thread(target=lambda: s.launch("EAR_P1", p1_args, p1_env,
        s.OUT_DIR / "ear_p1.log", 240, None), daemon=True).start()
    time.sleep(1.5)
    threading.Thread(target=lambda: s.launch("EAR_P2", p2_args, p2_env,
        s.OUT_DIR / "ear_p2.log", 240, None), daemon=True).start()

    print("[eartest] host+peer launched — LISTEN. Auto-terminates after the match "
          "batch; Ctrl-C to stop early.")
    time.sleep(180)
    s.kill_strays()
    print("[eartest] done — killed instances.")
