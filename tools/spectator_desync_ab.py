#!/usr/bin/env python3
"""Is the spectator desync caused by the palette work, or does it predate it?

Two FN netplay runs failed with a spectator desync (never a player-vs-player
one) while repeats of the same seeds passed, so seed-level determinism is out
and single runs prove nothing. This samples properly.

Arms, same seeds across all three so results pair:
  FN    -- palette hook installed, both peers confirm on palettes 6 and 7
  NOFN  -- palette hook installed, ordinary colours (isolates FN itself)
  BASE  -- palette hook NOT installed (needs its own build; run separately
           with --arm BASE after rebuilding with the install disabled)

Usage:
  python3 tools/spectator_desync_ab.py --arm FN --arm NOFN
  # rebuild with AltColors::Install() disabled, then:
  python3 tools/spectator_desync_ab.py --arm BASE
"""
import argparse
import os
import re
import subprocess
import sys
import time

SEEDS = [71, 73, 75]
ROOT = "/mnt/c/dev/wanwan"
RESULTS = os.path.join(ROOT, "tools", "spectator_desync_ab_results.txt")


def run(arm, seed):
    out = f"{ROOT}/tools/.spec_runs/ab_{arm}_{seed}"
    env = dict(os.environ)
    env.update({
        "FM2K_TEST_OUT_DIR": out,
        "FM2K_POOLSET": "1", "FM2K_CSS_ANIM": "1",
        "FM2K_NET_LOSS": "0.06", "FM2K_NET_DELAY_MS": "80",
        "FM2K_NET_SEED": str(seed),
    })
    if arm == "FN":
        env["FM2K_TEST_AUTO_CSS"] = "0,6,0,7,0"
    else:
        env.pop("FM2K_TEST_AUTO_CSS", None)
    keys = [k for k in env if k.startswith("FM2K_")]
    prev = env.get("WSLENV", "")
    env["WSLENV"] = ":".join(keys) + ((":" + prev) if prev else "")

    log = f"/tmp/ab_{arm}_{seed}.log"
    with open(log, "w") as fh:
        subprocess.run(
            ["python3", "tools/spec_selftest.py", "--rounds", "1",
             "--round-time", "15", "--total-frames", "3200",
             "--spectators", "css,battle1", "--assert-spectator-live",
             "--keep"],
            env=env, cwd=ROOT, stdout=fh, stderr=subprocess.STDOUT,
            timeout=600)
    text = open(log, encoding="utf-8", errors="replace").read()
    ok = "OVERALL PASS" in text
    sig = ""
    m = re.search(r"OVERALL FAIL: ([^\n]+)", text)
    if m:
        sig = m.group(1)[:80]
    # A player-vs-player desync would be a different, much worse animal.
    players = len(re.findall(r"DESYNC DETECTED|desync detected", text))
    return ok, sig, players


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", action="append", required=True,
                    choices=["FN", "NOFN", "BASE"])
    a = ap.parse_args()
    with open(RESULTS, "a") as res:
        res.write(f"\n=== {time.strftime('%Y-%m-%d %H:%M')} arms={a.arm}\n")
        for arm in a.arm:
            for seed in SEEDS:
                t0 = time.time()
                try:
                    ok, sig, players = run(arm, seed)
                except subprocess.TimeoutExpired:
                    ok, sig, players = False, "TIMEOUT", -1
                line = (f"{arm:5s} seed={seed} "
                        f"{'PASS' if ok else 'FAIL'} "
                        f"players_desync={players} {sig}")
                print(line, flush=True)
                res.write(line + "\n"); res.flush()
                print(f"      ({time.time()-t0:.0f}s)", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
