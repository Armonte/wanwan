#!/usr/bin/env python3
"""Replay self-test harness.

Drives the record-then-replay determinism test end-to-end on one machine:

  1. Launches FM2K_RollbackLauncher.exe in --stress mode with
     FM2K_AUTO_TERMINATE_AT_FRAME=N. The hook plays N battle frames of
     deterministic-random autoplay, writes parity snapshots to
     <out>/record.pty, dumps a replay slice to
     <game_dir>/replays/<ts>_harness.fm2krep, then TerminateProcess(0).

  2. Picks the just-written .fm2krep and re-launches the launcher in
     --replay mode with FM2K_PARITY_RECORD_PATH=<out>/replay.pty. The
     hook drives the engine from the recorded INPUT events, producing
     a second .pty stream.

  3. Runs tools/parity_diff.py record.pty replay.pty and reports the
     first frame where the replay sim state diverges from the original
     record. "ALL ALIGNED FRAMES IDENTICAL" means replay determinism
     held for the test window; a divergence is the bug to chase.

Defaults assume the developer install at C:/games (mirrored under
/mnt/c/games on WSL). Override with --launcher / --game / --out.

Usage:
  python3 tools/replay_selftest.py --frames 300
  python3 tools/replay_selftest.py --frames 1500 --game wanwan
  python3 tools/replay_selftest.py --frames 600 --out /tmp/selftest \
      --keep   # don't delete <out> when done
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

# Repo paths. Override with CLI flags if your install lives elsewhere.
DEFAULT_LAUNCHER     = Path("/mnt/c/games/FM2K_RollbackLauncher.exe")
DEFAULT_GAMES_ROOT   = Path("/mnt/c/games/2dfm")
DEFAULT_GAME_NAME    = "wanwan"      # substring match for --stress filter
# Direct-path bypass: when the --game value is itself a path to a .exe,
# the launcher's --stress flag skips the launcher.cfg-rooted discovery
# scan and just launches that file. Saves 30+ seconds on installs with a
# big D:\Games\fm2k\ tree.
DEFAULT_GAME_EXE     = Path("/mnt/c/games/2dfm/wanwan/WonderfulWorld_ver_0946.exe")
DEFAULT_OUT          = Path("/mnt/c/dev/wanwan/tools/.selftest")
DEFAULT_FRAMES       = 600
PARITY_DIFF          = Path(__file__).parent / "parity_diff.py"


def to_windows_path(p: Path) -> str:
    """Translate /mnt/c/foo/bar -> C:/foo/bar so the Windows .exe sees it."""
    s = str(p)
    if s.startswith("/mnt/") and len(s) > 6 and s[6] == "/":
        return s[5].upper() + ":" + s[6:]
    return s


def find_latest_fm2krep(game_dir: Path, since_ts: float) -> Path | None:
    """Newest *_harness.fm2krep written after since_ts (epoch seconds)."""
    replays = game_dir / "replays"
    if not replays.is_dir():
        return None
    candidates = []
    for f in replays.glob("*_harness.fm2krep"):
        try:
            mtime = f.stat().st_mtime
        except OSError:
            continue
        if mtime + 1.0 >= since_ts:  # 1s slack for filesystem clock drift
            candidates.append((mtime, f))
    if not candidates:
        return None
    candidates.sort()
    return candidates[-1][1]


def kill_strays() -> None:
    """Kill any lingering launcher / WonderfulWorld processes from a prior run."""
    for image in ("FM2K_RollbackLauncher.exe", "FM2KUpdater.exe",
                  "WonderfulWorld_ver_0946.exe"):
        subprocess.run(
            ["taskkill.exe", "/F", "/T", "/IM", image],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )


def run_phase(label: str, args: list[str], env: dict[str, str],
              timeout: float, log_path: Path,
              done_when: callable = None) -> int:
    """Run a launcher invocation with extra env vars; capture combined output.

    WSL→Windows note: passing env vars via Popen(env=...) when invoking a
    Windows .exe from a Linux Python does NOT propagate the var into the
    Windows process — the /init translation layer drops them. The reliable
    path is `cmd.exe /C "set K=V&& K2=V2&& <exe> args..."` so the Windows
    side does the env setup itself. The launcher then passes the resulting
    env block to its CreateProcessW child via GetEnvironmentStringsW.

    Auto-terminate note: even when the game process self-terminates via
    FM2K_AUTO_TERMINATE_AT_FRAME, the launcher process keeps running its
    UI loop. `done_when` is an optional predicate the harness polls every
    250 ms; once it returns True we kill the launcher tree and treat the
    phase as completed regardless of launcher exit code. Use this to wait
    on artifact appearance (.fm2krep file, .pty file) instead of process
    exit.
    """
    # Forward opt-in debug/profiler env vars from the calling shell so a dev
    # can do `FM2K_PERF_PROFILE=1 python3 replay_selftest.py ...` and have it
    # reach the Windows launcher (and thus the injected hook). The phase env
    # dicts only carry the harness-essential vars; anything in this passthrough
    # list is layered on top if present in os.environ (phase env wins on clash).
    env = dict(env)
    # FM2K_TEST_BACKGROUND: ON BY DEFAULT. The determinism gate and the
    # multigame sweep both drive this harness, so a single default here
    # backgrounds every RECORD/REPLAY launcher + game window those stages open.
    # Watch a run with FM2K_TEST_BACKGROUND=0.
    env.setdefault("FM2K_TEST_BACKGROUND",
                   os.environ.get("FM2K_TEST_BACKGROUND", "1"))
    for k in ("FM2K_PERF_PROFILE", "FM2K_STRESS_DIAG", "FM2K_FULL_CRCS",
              "FM2K_RUNAHEAD", "FM2K_PRED_WINDOW", "FM2K_PREDICTION_WINDOW", "FM2K_LOCAL_DELAY",
              "FM2K_CHECK_DISTANCE", "FM2K_PARITY_CAPTURE_TRACE",
              "FM2K_CAMTRACE", "FM2K_CAMTRACE_HEX", "FM2K_CAM_DIAG",
              "FM2K_RING_TRACE",
              # Envelope inversion phase 1 (SHADOW MODE). This harness is the
              # instrument's HIGHEST-POWER regime by a wide margin: it forces
              # rollbacks with FM2K_CHECK_DISTANCE on an autoplay session where
              # both players' inputs are local, so a resim consumes IDENTICAL
              # inputs and essentially every forward-vs-replay comparison is
              # usable. Under a lossy netplay profile the opposite holds -- the
              # rollbacks exist BECAUSE an input was corrected, and the control
              # discards >99% of comparisons. Dark by default in the hook.
              "FM2K_ENVELOPE_SHADOW", "FM2K_ENVELOPE_SHADOW_WITNESS"):
        if k not in env and os.environ.get(k):
            env[k] = os.environ[k]

    print(f"[selftest] {label}: launching")
    print(f"[selftest]   args: {' '.join(args)}")
    if any(k.startswith("FM2K_") and k not in
           ("FM2K_AUTO_TERMINATE_AT_FRAME", "FM2K_PARITY_RECORD_PATH")
           for k in env):
        extra = {k: v for k, v in env.items() if k not in
                 ("FM2K_AUTO_TERMINATE_AT_FRAME", "FM2K_PARITY_RECORD_PATH")}
        print(f"[selftest]   extra env: {extra}")
    print(f"[selftest]   FM2K_AUTO_TERMINATE_AT_FRAME="
          f"{env.get('FM2K_AUTO_TERMINATE_AT_FRAME', '(unset)')}")
    print(f"[selftest]   FM2K_PARITY_RECORD_PATH="
          f"{env.get('FM2K_PARITY_RECORD_PATH', '(unset)')}")
    print(f"[selftest]   log: {log_path}")

    # Build `set X=Y&& ` prefix for each env var. cmd.exe's `set` is the
    # only reliable way to land env vars inside a Windows process when the
    # parent is a WSL Python.
    set_parts: list[str] = []
    for k, v in env.items():
        # cmd.exe is sensitive to & and ^ and " in values; our values are
        # frame counts (digits) and Windows paths (drive-letter form,
        # no special chars) so a basic quote suffices. Add escaping
        # here if we ever pass user-controlled strings.
        set_parts.append(f"set {k}={v}")
    # Translate /mnt/c/... -> C:/... so cmd.exe can execute the .exe.
    # cmd.exe doesn't understand WSL-style mount paths even though the
    # /init wrapper would. This belt-and-suspenders matches the env-var
    # values (already converted via to_windows_path).
    win_args = []
    for a in args:
        if a.startswith("/mnt/") and len(a) > 6 and a[6] == "/":
            a = a[5].upper() + ":" + a[6:]
        # cmd.exe wants double-quoted args containing spaces; our paths
        # are drive-letter form (no spaces in dev install). Quote anyway
        # for safety against future install paths with spaces.
        win_args.append(f'"{a}"' if " " in a else a)
    # IMPORTANT: cmd.exe's `set K=V` includes ALL trailing whitespace in V
    # (treats `set K=V ` as V=`V `), so the joiner must NOT have a leading
    # space — only a trailing one before the next token. `&& ` (no leading)
    # keeps the assigned value clean and produces a working separator
    # because cmd.exe accepts `&&` without whitespace either side.
    cmd_inner = "&& ".join(set_parts + [" ".join(win_args)])
    cmd_args = ["cmd.exe", "/C", cmd_inner]
    deadline = time.time() + timeout
    with open(log_path, "wb") as logf:
        proc = subprocess.Popen(cmd_args, stdout=logf,
                                stderr=subprocess.STDOUT)
        rc = None
        while time.time() < deadline:
            # Subprocess finished naturally — happy path for cases where
            # the launcher does exit on its own (errors / direct mode).
            poll_rc = proc.poll()
            if poll_rc is not None:
                rc = poll_rc
                break
            # Predicate-based completion: the game produced the artifact
            # we wanted (e.g., the .fm2krep file). Stop here, kill the
            # launcher's UI loop, treat the run as success.
            if done_when is not None and done_when():
                print(f"[selftest]   {label}: done_when() satisfied; "
                      f"killing launcher (still running its UI loop)")
                kill_strays()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                rc = 0
                break
            time.sleep(0.25)
        if rc is None:
            print(f"[selftest]   TIMEOUT after {timeout:.0f}s — killing")
            proc.kill()
            kill_strays()
            return 124
    print(f"[selftest] {label}: exit rc={rc}")
    return rc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--launcher", type=Path, default=DEFAULT_LAUNCHER,
                    help=f"Path to FM2K_RollbackLauncher.exe (default: {DEFAULT_LAUNCHER})")
    ap.add_argument("--game-exe", type=Path, default=DEFAULT_GAME_EXE,
                    help=f"Direct path to the game .exe (default: {DEFAULT_GAME_EXE}). "
                         f"Passed verbatim to --stress so the launcher skips its tree scan.")
    ap.add_argument("--games-root", type=Path, default=DEFAULT_GAMES_ROOT,
                    help=f"Root containing per-game folders (used only to locate "
                         f"the produced .fm2krep; default: {DEFAULT_GAMES_ROOT})")
    ap.add_argument("--frames", type=int, default=DEFAULT_FRAMES,
                    help=f"Battle frames to record before terminate (default: {DEFAULT_FRAMES})")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT,
                    help=f"Workspace for .pty + logs (default: {DEFAULT_OUT})")
    ap.add_argument("--keep", action="store_true",
                    help="Don't clean <out> when done (useful for poking at .pty files)")
    ap.add_argument("--record-timeout", type=float, default=60.0,
                    help="Max seconds the record phase can run (default: 60)")
    ap.add_argument("--replay-timeout", type=float, default=60.0,
                    help="Max seconds the replay phase can run (default: 60)")
    args = ap.parse_args()

    if not args.launcher.exists():
        print(f"[selftest] launcher not found: {args.launcher}", file=sys.stderr)
        return 2
    if not args.game_exe.exists():
        print(f"[selftest] game exe not found: {args.game_exe}", file=sys.stderr)
        return 2

    # Locate the game dir we're driving against. The .fm2krep gets written
    # to <game_dir>/replays/ by the hook (relative to the game's CWD,
    # which is the .exe's parent).
    game_dir = args.game_exe.parent

    args.out.mkdir(parents=True, exist_ok=True)
    record_pty = args.out / "record.pty"
    replay_pty = args.out / "replay.pty"
    record_log = args.out / "record.log"
    replay_log = args.out / "replay.log"

    # Clean stale outputs so we don't accidentally diff against an old run.
    for p in (record_pty, replay_pty, record_log, replay_log):
        if p.exists():
            p.unlink()

    # Kill any leftover launcher / game processes from a previous run that
    # would otherwise grab the input record file or steal focus.
    kill_strays()

    # ---------------------------------------------------------------------
    # Phase 1 — RECORD: --stress autoplay, terminate at frame N, emit .fm2krep
    # ---------------------------------------------------------------------
    record_start_ts = time.time()
    rec_env = {
        "FM2K_AUTO_TERMINATE_AT_FRAME": str(args.frames),
        "FM2K_PARITY_RECORD_PATH": to_windows_path(record_pty),
        # No FM2K_FULL_CRCS — too slow for >300 frame test runs; the
        # parity recorder captures everything we need for the diff.
    }
    # Phase completes once the .fm2krep slice has been written. The
    # launcher's UI loop doesn't self-exit on game termination, so polling
    # for the artifact is the cleanest signal.
    def record_done():
        return find_latest_fm2krep(game_dir, record_start_ts) is not None
    rc = run_phase(
        "RECORD",
        [str(args.launcher), "--stress", to_windows_path(args.game_exe)],
        rec_env, args.record_timeout, record_log,
        done_when=record_done,
    )
    if rc not in (0, 1):  # 1 = TerminateProcess(GetCurrentProcess(), 0) on
                          # a hook side path that doesn't set exit code 0
        print(f"[selftest] record phase failed (rc={rc}) — check {record_log}", file=sys.stderr)
        return 3
    if not record_pty.exists():
        print(f"[selftest] record .pty not produced: {record_pty}", file=sys.stderr)
        print(f"  (parity recorder not opened? check {record_log} for "
              f"'ParityRecorder: opened .pty output')", file=sys.stderr)
        return 3

    fm2krep = find_latest_fm2krep(game_dir, record_start_ts)
    if fm2krep is None:
        print(f"[selftest] no *_harness.fm2krep in {game_dir/'replays'} "
              f"newer than record start", file=sys.stderr)
        print(f"  (SpectatorNode_WriteCurrentBattleFile probably returned false — "
              f"check {record_log} for 'Harness: .fm2krep slice')", file=sys.stderr)
        return 3
    print(f"[selftest] record .fm2krep: {fm2krep} ({fm2krep.stat().st_size} bytes)")

    # ---------------------------------------------------------------------
    # Phase 2 — REPLAY: --replay <fm2krep>, parity-record into replay.pty
    # ---------------------------------------------------------------------
    kill_strays()
    rep_env = {
        # Headroom over the record-side frame count: replay can advance
        # slightly past the record terminator if the .fm2krep was sliced
        # at MATCH_END and the last batch of events resolved a frame or
        # two later. +20 keeps the runs comparable without truncating
        # the test window.
        "FM2K_AUTO_TERMINATE_AT_FRAME": str(args.frames + 20),
        "FM2K_PARITY_RECORD_PATH": to_windows_path(replay_pty),
        # Skip CSS on replay side too. Stress mode records with
        # FM2K_BOOT_TO_BATTLE=1 (engine jumps direct to game_mode=3000
        # with rng=0x12345678), and the recorded .fm2krep slice begins
        # at MATCH_START with synthetic char_id=0/color=0. Without
        # boot-to-battle on the replay side the CssAutoConfirm path
        # tries to drive a CSS that has no valid selection state and
        # spin-locks. Matching boot-to-battle on both sides puts the
        # engines in identical pre-input state and the .fm2krep INPUT
        # events drive identical battle evolution.
        "FM2K_BOOT_TO_BATTLE": "1",
        "FM2K_AUTO_TITLE_SKIP": "1",
        # Replays now play 1:1 for humans (the bank drains are disabled
        # in offline replay); the harness opts back into full-speed
        # drain so test wall time stays short.
        "FM2K_SPECTATOR_ALWAYS_CATCHUP": "1",
    }
    # Isolation toggle: SELFTEST_NO_CATCHUP=1 runs the replay at 1:1 (render every
    # frame, no fast-forward) so a determinism pass proves SIM determinism alone,
    # separate from catch-up/fast-forward correctness. Default keeps catch-up on
    # (faster wall time, and exercises the live-spectator catch-up path).
    if os.environ.get("SELFTEST_NO_CATCHUP"):
        rep_env.pop("FM2K_SPECTATOR_ALWAYS_CATCHUP", None)
    # Replay phase: target the SAME snapshot count as the record side so
    # the parity diff has comparable windows. 32-byte header + 260 bytes
    # per snapshot.
    #
    # Off-by-one: the record-side terminator slices the .fm2krep BEFORE
    # the final frame's INPUT lands, so the replay queue drains after
    # record_snap_count - 1 sim frames and the .pty plateaus one snapshot
    # short of the record. Demanding an exact match here is unsatisfiable
    # -- the 2026-06-08 "truncated" artifact and a 240s timeout on
    # 2026-06-11 were both this predicate spinning at N-1. Accept a
    # 2-snapshot tolerance plus 3s of quiescence (no growth) so we don't
    # kill the launcher mid-flush.
    record_snap_count = max(0, (record_pty.stat().st_size - 32) // 260)
    REPLAY_DONE_MIN_SIZE = 32 + 260 * max(0, record_snap_count - 2)
    print(f"[selftest] record produced {record_snap_count} snapshots; "
          f"waiting for replay to reach >= {record_snap_count - 2}")
    replay_done_state = {"size": -1, "since": 0.0}
    def replay_done():
        try:
            sz = replay_pty.stat().st_size
        except OSError:
            return False
        now = time.time()
        if sz != replay_done_state["size"]:
            replay_done_state["size"] = sz
            replay_done_state["since"] = now
            return False
        return sz >= REPLAY_DONE_MIN_SIZE and (now - replay_done_state["since"]) >= 3.0
    rc = run_phase(
        "REPLAY",
        [str(args.launcher), "--replay", to_windows_path(fm2krep)],
        rep_env, args.replay_timeout, replay_log,
        done_when=replay_done,
    )
    if rc not in (0, 1):
        print(f"[selftest] replay phase failed (rc={rc}) — check {replay_log}", file=sys.stderr)
        return 4
    if not replay_pty.exists():
        print(f"[selftest] replay .pty not produced: {replay_pty}", file=sys.stderr)
        return 4
    print(f"[selftest] replay parity: {replay_pty} ({replay_pty.stat().st_size} bytes)")

    # ---------------------------------------------------------------------
    # Phase 3 — DIFF
    # ---------------------------------------------------------------------
    print()
    print(f"[selftest] === parity_diff ===")
    diff_rc = subprocess.call(
        [sys.executable, str(PARITY_DIFF), str(record_pty), str(replay_pty)]
    )

    if not args.keep:
        # Leave logs on failure for inspection; remove on clean pass.
        if diff_rc == 0:
            for p in (record_pty, replay_pty, record_log, replay_log):
                p.unlink(missing_ok=True)
            try:
                args.out.rmdir()
            except OSError:
                pass  # non-empty (user dropped files there) — leave alone

    return diff_rc


if __name__ == "__main__":
    sys.exit(main())
