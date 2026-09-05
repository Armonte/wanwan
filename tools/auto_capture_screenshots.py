#!/usr/bin/env python3
"""SUPERSEDED by tools/capture_batch.py -- do not extend this file.

It was scaffolded around a launcher CLI (`--capture-game`) that was never
written, so it could not drive a capture run: everything below the
docstring assumes a flag the launcher does not have.

What actually works, and what the 2026-09-03 library sweep used:

    tools/capture_batch.py --limit 200      # every game in the registry
    tools/capture_batch.py --games "D:\\...\\a.exe,D:\\...\\b.exe"
    tools/capture_transcode.py              # PNG -> WebP + thumbs
    tools/capture_pick.py --sheet out.png   # choose each game's thumbnail

capture_batch drives the SHIPPING launcher through its existing
`--offline` path with the hook's capture env set, which is why it needed
no new launcher flag. The hook writes a series per state (front x6,
css x3, battle x3) plus a per-character roster sweep, and signals
completion with a `.capture_done` sentinel.

This file is kept only because docs/dev/banner_pipeline.md and
docs/roadmaps/hub_catalog_rework.md still name it as the orchestrator;
delete it once those point at capture_batch.py instead.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parent.parent
REGISTRY_PATH = REPO_ROOT / "games" / "registry.json"
CAPTURE_ROOT = REPO_ROOT / "dist" / "captures"

# Per-run hard cap. Most FM2K games reach battle within 20 s with
# AutoTitleSkip mashing A; double that for slow installs / disk-cold
# first launches; triple for the genuinely-slow boot-to-CSS ones
# (vanpri w/ cnc-ddraw + locale spoof has been observed at 35 s).
RUN_TIMEOUT_S = 90
POLL_INTERVAL_S = 0.5

# Sentinel file the hook writes once it's captured all three core
# frames (title / css_initial / battle). The orchestrator polls for
# it and proceeds to the next game once present.
DONE_SENTINEL = ".capture_done"


def games_to_capture(only: list[str] | None) -> list[dict[str, Any]]:
    """Pick games from the registry that have a usable game_id and
    (optionally) intersect the `only` filter."""
    if not REGISTRY_PATH.exists():
        print(f"error: {REGISTRY_PATH} missing — "
              f"run tools/build_registry.py first", file=sys.stderr)
        return []
    recs = json.loads(REGISTRY_PATH.read_text(encoding="utf-8"))
    out: list[dict[str, Any]] = []
    for r in recs:
        gid = r.get("game_id") or ""
        if not gid:
            continue
        if only and gid not in only:
            continue
        out.append(r)
    return out


def already_captured(game_id: str) -> bool:
    """Has this game already been through capture? Look for the
    title.png + css_initial.png + battle.png triple under
    dist/captures/<game_id>/."""
    d = CAPTURE_ROOT / game_id
    return all((d / f).exists() for f in
               ("title.png", "css_initial.png", "battle.png"))


def run_one(game: dict[str, Any], launcher_exe: Path,
            refresh: bool) -> bool:
    gid = game["game_id"]
    out_dir = CAPTURE_ROOT / gid
    if not refresh and already_captured(gid):
        print(f"  ⏭  {gid:30s} already captured")
        return True

    out_dir.mkdir(parents=True, exist_ok=True)
    sentinel = out_dir / DONE_SENTINEL
    if sentinel.exists():
        sentinel.unlink()

    env = dict(os.environ)
    env["FM2K_AUTO_CAPTURE"]   = "1"
    env["FM2K_CAPTURE_DIR"]    = str(out_dir)
    # Force the hook to autostart through the title without prompting
    # for menu input. AutoTitleSkip is on by default but we explicitly
    # set it here in case the user disabled it for normal play.
    env["FM2K_AUTO_TITLE_SKIP"] = "1"
    # Make sure netplay paths stay quiet — capture is offline-only.
    env["FM2K_TRUE_OFFLINE"]   = "1"

    print(f"  ▶  {gid:30s} → {out_dir.relative_to(REPO_ROOT)}")
    cmd = [str(launcher_exe), "--capture-game", gid]
    proc = subprocess.Popen(cmd, env=env)

    deadline = time.monotonic() + RUN_TIMEOUT_S
    success = False
    while time.monotonic() < deadline:
        if sentinel.exists():
            success = True
            break
        if proc.poll() is not None:
            # Launcher exited before sentinel landed — usually the
            # hook crashed or the game refused to launch. Treat as
            # failure.
            break
        time.sleep(POLL_INTERVAL_S)

    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    if success:
        print(f"  ✓  {gid:30s} captured")
        # Write a thin manifest so a later post-processor can pick
        # the canonical banner without re-checking each PNG.
        manifest = {
            "game_id":      gid,
            "name":         game.get("name") or gid,
            "captured_at":  time.time(),
            "files":        sorted(p.name for p in out_dir.iterdir()
                                   if p.suffix == ".png"),
        }
        (out_dir / "manifest.json").write_text(
            json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8")
    else:
        print(f"  ✗  {gid:30s} timed out / crashed")
    return success


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="",
                    help="comma-separated game_ids to capture; "
                         "default = every registry entry")
    ap.add_argument("--refresh", action="store_true",
                    help="re-capture games that already have all "
                         "three core frames on disk")
    ap.add_argument("--launcher", default="",
                    help="path to FM2K_RollbackLauncher.exe; defaults "
                         "to /mnt/c/games/FM2K_RollbackLauncher.exe "
                         "(WSL convention) when unset")
    args = ap.parse_args()

    launcher = (Path(args.launcher) if args.launcher
                else Path("/mnt/c/games/FM2K_RollbackLauncher.exe"))
    if not launcher.exists():
        print(f"error: launcher not found at {launcher}",
              file=sys.stderr)
        print("       pass --launcher /path/to/exe", file=sys.stderr)
        return 1

    only = ([s.strip() for s in args.only.split(",") if s.strip()]
            if args.only else None)
    games = games_to_capture(only)
    if not games:
        print("no games matched", file=sys.stderr)
        return 1

    print(f"capturing {len(games)} game(s)")
    CAPTURE_ROOT.mkdir(parents=True, exist_ok=True)
    fails: list[str] = []
    for g in games:
        ok = run_one(g, launcher, args.refresh)
        if not ok:
            fails.append(g["game_id"])

    print()
    print(f"done. captured: {len(games) - len(fails)}, "
          f"failed: {len(fails)}")
    if fails:
        print(f"  failed: {', '.join(fails[:20])}"
              + (" ..." if len(fails) > 20 else ""))
    return 0 if not fails else 2


def _superseded() -> int:
    print(__doc__)
    print("Refusing to run: use tools/capture_batch.py.")
    return 2


if __name__ == "__main__":
    sys.exit(_superseded())
