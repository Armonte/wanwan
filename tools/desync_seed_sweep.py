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

# Reuse the harness's replay-detection + live-edge helpers so the sweep computes
# the SAME three signals the harness does (no fragile stdout scraping). Importing
# is side-effect free -- spec_selftest only builds functions/constants at module
# scope; argparse/launch all live inside its main().
sys.path.insert(0, str(ROOT / "tools"))
import spec_selftest  # noqa: E402


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


def clear_live_logs():
    # Wipe the harness's preserved live_ logs so a run that dies before
    # preservation can't leave the PREVIOUS seed's logs to be misread as this
    # seed's spectator liveness.
    for name in ("live_FM2K_P1_Debug.log", "live_FM2K_P2_Debug.log",
                 "live_FM2K_S1_Debug.log"):
        try:
            (spec_selftest.OUT_DIR / name).unlink()
        except OSError:
            pass


def players_completed(since):
    # The match completed if the players wrote a FRESH timestamped harness
    # replay this run (host p0, else client p1).
    return (spec_selftest.find_latest_fm2krep(GAME_DIR, since, suffix="_p0_harness") is not None
            or spec_selftest.find_latest_fm2krep(GAME_DIR, since, suffix="_p1_harness") is not None)


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
    results = []
    for i, seed in enumerate(seeds):
        clear_desync_dumps()
        clear_live_logs()
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
        except subprocess.TimeoutExpired:
            pass
        dt = time.time() - t0

        # ---- THREE DISTINCT SIGNALS (do not conflate) --------------------------
        # 1) engine desync : the hook wrote FM2K_P*_desync_f<frame>.log.
        # 2) match complete : the players wrote a fresh *_p{0,1}_harness.fm2krep.
        # 3) spectator live : its applied battle-frame progression reached & held
        #    near the host's final battle frame (spec_selftest.spectator_liveness).
        desync = found_desync()
        completed = players_completed(t0)
        lv = spec_selftest.spectator_liveness(
            spec_selftest.OUT_DIR / "live_FM2K_P1_Debug.log",
            spec_selftest.OUT_DIR / "live_FM2K_S1_Debug.log")
        rec = dict(seed=seed, desync=desync, completed=completed,
                   reached_live=lv["reached"], stall_frame=lv["stall_frame"], lv=lv)
        results.append(rec)

        engine_s = f"YES ({desync[0]}@f{desync[1]})" if desync else "no"
        match_s = "yes" if completed else "NO"
        if lv["reached"]:
            spec_s = "yes"
        else:
            spec_s = (f"NO (stall_frame={lv['stall_frame']} host_final="
                      f"{lv['host_final']} spec_max={lv['spec_max']} gap={lv['gap']})")
        print(f"[sweep] seed={seed} ({i+1}/{len(seeds)}, {dt:.0f}s): "
              f"engine_desync={engine_s} | match_completed={match_s} | "
              f"spectator_reached_live={spec_s}")

        # A reproduced FAILURE of either kind (engine desync, or a completed match
        # whose spectator fell behind) is enough to pin -- stop to save time.
        if desync or (completed and not lv["reached"]):
            break

    # ---- SUMMARY: three separate signals, never one conflated FAIL ------------
    desync_hits = [d for d in results if d["desync"]]
    spec_behind = [d for d in results
                   if d["completed"] and not d["desync"] and not d["reached_live"]]
    incomplete  = [d for d in results if not d["completed"] and not d["desync"]]
    clean       = [d for d in results
                   if d["completed"] and d["reached_live"] and not d["desync"]]
    print(f"\n[sweep] SUMMARY over {len(results)} seed(s) "
          f"(loss={args.loss} delay={args.delay}ms) -- THREE DISTINCT SIGNALS:")
    print(f"  [1] engine desync (desync_f*.log)       : {len(desync_hits)} "
          f"{[(d['seed'], d['desync'][0], d['desync'][1]) for d in desync_hits]}")
    print(f"  [2] spectator failed to reach live edge : {len(spec_behind)} "
          f"{[(d['seed'], d['stall_frame']) for d in spec_behind]}")
    print(f"  [3] match didn't complete (no .fm2krep) : {len(incomplete)} "
          f"{[d['seed'] for d in incomplete]}")
    print(f"      clean (completed + spectator live)  : {len(clean)} "
          f"{[d['seed'] for d in clean]}")

    if desync_hits:
        d = desync_hits[0]
        print(f"[sweep] PIN: FM2K_NET_SEED={d['seed']} FM2K_NET_LOSS={args.loss} "
              f"FM2K_NET_DELAY_MS={args.delay} reproduces an ENGINE DESYNC "
              f"({d['desync'][0]} @ f{d['desync'][1]})")
        return 0
    if spec_behind:
        d = spec_behind[0]
        print(f"[sweep] PIN: FM2K_NET_SEED={d['seed']} FM2K_NET_LOSS={args.loss} "
              f"FM2K_NET_DELAY_MS={args.delay} reproduces a SPECTATOR-BEHIND "
              f"failure (match completed but spectator stalled @ frame "
              f"{d['stall_frame']}, host_final={d['lv']['host_final']}, "
              f"gap={d['lv']['gap']})")
        return 0
    if incomplete and not clean:
        print(f"\n[sweep] no engine desync / spectator-behind, but "
              f"{len(incomplete)} seed(s) never completed a match -- likely a "
              f"harness/timeout issue rather than the netcode bug; inspect "
              f"tools/.spec_selftest/ logs.")
        return 1
    print(f"\n[sweep] no engine desync and no spectator-behind in {len(seeds)} "
          f"seeds at loss={args.loss} -- failure rarer than 1/{len(seeds)} at "
          f"these settings; raise --loss or --range, or it may be timing-"
          f"sensitive beyond the seed.")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
