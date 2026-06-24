#!/usr/bin/env python3
"""desync_seed_sweep.py -- find a FM2K_NET_SEED that reproduces the high-loss desync.

NetherRealm's method (GDC doc 13): capture the exact match conditions so the
desync reproduces offline, then add breadcrumbs. Step 1 is a deterministic
trigger. The in-process link impairment (FM2K_NET_DELAY_MS/LOSS/SEED) is seeded,
so a fixed seed replays the SAME loss pattern -> the SAME desync (or clean run)
every time. This sweeps seeds at a target loss until one desyncs, then pins it.

A desync is detected by the hook writing FM2K_P*_desync_f<frame>.log into the
game dir (the DESYNC-terminate path). The first reproducing seed + frame is
reported; re-run spec_selftest with FM2K_NET_SEED=<seed> to reproduce on demand.

Usage:
    python3 tools/desync_seed_sweep.py [--loss 0.25] [--delay 100] \
        [--seeds 1,2,3,...|--range N] [--frames 3200]
"""
import argparse
import glob
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME_DIR = Path("/mnt/c/games/2dfm/wanwan")
SELFTEST = ROOT / "tools" / "spec_selftest.py"


def clear_desync_dumps():
    for p in glob.glob(str(GAME_DIR / "FM2K_P*_desync_f*.log")):
        try:
            os.unlink(p)
        except OSError:
            pass


def found_desync():
    # returns (player, frame) of the first desync dump, or None
    dumps = sorted(glob.glob(str(GAME_DIR / "FM2K_P*_desync_f*.log")))
    for d in dumps:
        name = os.path.basename(d)
        # FM2K_P1_desync_f1548.log
        try:
            who = name.split("_")[1]
            frame = int(name.split("_f")[1].split(".")[0])
            return (who, frame)
        except (IndexError, ValueError):
            continue
    return None


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--loss", default="0.25")
    ap.add_argument("--delay", default="100")
    ap.add_argument("--frames", type=int, default=3200)
    ap.add_argument("--seeds", default="")
    ap.add_argument("--range", type=int, default=12)
    ap.add_argument("--timeout", type=int, default=240)
    args = ap.parse_args(argv[1:])

    if args.seeds:
        seeds = [int(s) for s in args.seeds.split(",") if s.strip()]
    else:
        seeds = list(range(1, args.range + 1))

    print(f"[sweep] loss={args.loss} delay={args.delay}ms frames={args.frames} "
          f"seeds={seeds}")
    hits = []
    for i, seed in enumerate(seeds):
        clear_desync_dumps()
        env = dict(os.environ)
        env["FM2K_NET_DELAY_MS"] = str(args.delay)
        env["FM2K_NET_LOSS"] = str(args.loss)
        env["FM2K_NET_SEED"] = str(seed)
        cmd = [sys.executable, str(SELFTEST), "--game", "wanwan",
               "--rounds", "1", "--round-time", "15",
               "--total-frames", str(args.frames),
               "--spectators", "battle1", "--keep"]
        t0 = time.time()
        try:
            r = subprocess.run(cmd, env=env, cwd=str(ROOT),
                               capture_output=True, text=True,
                               timeout=args.timeout + 120)
            tail = "\n".join(r.stdout.splitlines()[-3:])
        except subprocess.TimeoutExpired:
            tail = "(harness timeout)"
        dt = time.time() - t0
        hit = found_desync()
        if hit:
            who, frame = hit
            hits.append((seed, who, frame))
            print(f"[sweep] seed={seed} ({i+1}/{len(seeds)}, {dt:.0f}s) -> "
                  f"*** DESYNC reproduced *** {who} @ frame {frame}")
            print(f"[sweep] PIN: FM2K_NET_SEED={seed} FM2K_NET_LOSS={args.loss} "
                  f"FM2K_NET_DELAY_MS={args.delay} reproduces a desync at f{frame}")
            # keep going to gather a couple more for confidence, but the first
            # is enough to pin -- stop to save time.
            break
        else:
            print(f"[sweep] seed={seed} ({i+1}/{len(seeds)}, {dt:.0f}s) -> clean "
                  f"| {tail.splitlines()[-1] if tail.strip() else ''}")
    if hits:
        print(f"\n[sweep] reproducing seed(s): {hits}")
        return 0
    print(f"\n[sweep] no desync in {len(seeds)} seeds at loss={args.loss} "
          f"-- desync rarer than 1/{len(seeds)} at these settings; raise --loss "
          f"or --range, or it may be timing-sensitive beyond the seed.")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
