#!/usr/bin/env python3
"""Does the colour picker actually do what we claim?

Three claims, none of which the normal gate touches -- its auto-CSS drives
one fixed character and colour, so every colour path is a no-op there. A
green gate proves nothing was broken; it says nothing about this feature.

  1. DIFFERENT characters may now share a colour. The engine refused this:
     its collision loop compares only the colour, never the character, so two
     players on unrelated characters could not both pick palette 3.
  2. The SAME character still may not, so a mirror match stays legible.
  3. Palettes 6 and 7 are reachable at all, via FN1/FN2 (0x1000/0x2000) --
     the engine's ladder stops at 5 because it only tests six attack buttons.

Drives the pair through FM2K_TEST_AUTO_CSS=p1char,p1color,p2char,p2color,stage
and reads the [ALTCOLOR] lines back out of the game logs.

Usage: python3 tools/test_alt_colors.py
"""
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, "tools")
import spec_selftest as s

CASES = [
    # name, p1char, p1col, p2char, p2col, expected p1, expected p2
    ("different chars share a colour", 3, 2, 5, 2, 2, 2),
    ("same char must not share",       3, 2, 3, 2, 2, 3),
    ("palette 6 via FN1",              3, 6, 5, 1, 6, 1),
    ("palette 7 via FN2",              3, 7, 5, 4, 7, 4),
    ("both on the two new palettes",   3, 6, 5, 7, 6, 7),
]


def run_case(name, p1c, p1col, p2c, p2col, log_dir):
    env = dict(os.environ)
    env.update({
        "FM2K_TEST_AUTO_CSS": f"{p1c},{p1col},{p2c},{p2col},0",
        "FM2K_PARITY_AUTOPLAY": "1",
        "FM2K_AUTO_TITLE_SKIP": "1",
        "FM2K_TEST_ROUNDS": "1",
        "FM2K_TEST_ROUND_TIME": "10",
        "FM2K_TEST_OUT_DIR": f"/mnt/c/dev/wanwan/tools/.altcolor/{name.replace(' ','_')}",
    })
    keys = [k for k in env if k.startswith("FM2K_")]
    prev = env.get("WSLENV", "")
    env["WSLENV"] = ":".join(keys) + ((":" + prev) if prev else "")
    s.kill_strays(); time.sleep(0.8)
    p = subprocess.run(
        ["python3", "tools/spec_selftest.py", "--rounds", "1",
         "--round-time", "10", "--total-frames", "900"],
        env=env, capture_output=True, text=True, timeout=420)
    return p.returncode


def read_colours(log_dir):
    """Last [ALTCOLOR] verdict per slot, from whichever peer logged it."""
    got = {}
    import glob
    for f in glob.glob(str(log_dir / "FM2K_P*_Debug.log")):
        try:
            text = open(f, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        for m in re.finditer(r"\[ALTCOLOR\] slot=(\d+) char=(-?\d+) "
                             r"asked=(\d+) got=(\d+)", text):
            got[int(m.group(1))] = (int(m.group(2)), int(m.group(3)),
                                    int(m.group(4)))
    return got


def main():
    game = s.GAMES["wanwan"]
    if not game.exists():
        print(f"game not found: {game}"); return 2
    log_dir = game.parent / "logs"
    fails = 0
    for (name, p1c, p1col, p2c, p2col, exp1, exp2) in CASES:
        run_case(name, p1c, p1col, p2c, p2col, log_dir)
        got = read_colours(log_dir)
        c0 = got.get(0, (None, None, None))[2]
        c1 = got.get(1, (None, None, None))[2]
        ok = (c0 == exp1 and c1 == exp2)
        print(f"[{'PASS' if ok else 'FAIL'}] {name}: "
              f"p1 asked {p1col} got {c0} (want {exp1}), "
              f"p2 asked {p2col} got {c1} (want {exp2})")
        if not ok:
            fails += 1
            if c0 is None and c1 is None:
                print("       no [ALTCOLOR] lines at all -- the hook did not "
                      "run, so this case measured NOTHING")
    print(f"\n{'ALL PASS' if not fails else str(fails) + ' FAILED'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
