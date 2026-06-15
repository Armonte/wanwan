#!/usr/bin/env python3
"""compare_regions.py -- lossless gate for the C++ .fpk reconstructor.

Parses BOTH the C++ reconstructor output (OUT.bin) and the matching original
.player/.stage/.demo with tools/kgt/fpk.py's Container, then asserts the
non-audio regions are BYTE-IDENTICAL:

    header, scripts_region, script_items_region,
    sprite_headers (all frames), sprite_contents (all frames),
    palettes_region, tail

Audio is lossy (Opus) so it is only compared structurally: sound_n must match.

usage: compare_regions.py ORIGINAL OUT.bin
exit 0 = PASS, 1 = FAIL, 2 = usage/parse error.
"""
import os
import sys

# import Container from the proven oracle
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "kgt"))
import fpk as fpk_oracle  # noqa: E402


def load(path):
    with open(path, "rb") as f:
        return fpk_oracle.Container(f.read())


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: compare_regions.py ORIGINAL OUT.bin\n")
        return 2

    orig_path, out_path = argv[1], argv[2]
    try:
        src = load(orig_path)
        rc = load(out_path)
    except Exception as e:
        print(f"FAIL  parse error: {e}")
        return 2

    name = os.path.basename(orig_path)
    regions = [
        ("header", src.header, rc.header),
        ("scripts_region", src.scripts_region, rc.scripts_region),
        ("script_items_region", src.script_items_region, rc.script_items_region),
        ("sprite_headers", b"".join(src.sprite_headers), b"".join(rc.sprite_headers)),
        ("sprite_contents", b"".join(src.sprite_contents), b"".join(rc.sprite_contents)),
        ("palettes_region", src.palettes_region, rc.palettes_region),
        ("tail", src.tail, rc.tail),
    ]

    all_ok = True
    diverged = []
    for rname, a, b in regions:
        ok = (a == b)
        if not ok:
            all_ok = False
            diverged.append(rname)
            # find first differing byte offset for diagnostics
            lim = min(len(a), len(b))
            first = next((i for i in range(lim) if a[i] != b[i]), lim)
            print(f"  [FAIL] {rname}: {len(a)} vs {len(b)} B, first diff @ {first}")
        else:
            print(f"  [OK ] {rname} ({len(a)} B)")

    # sprite frame count must also match (already implied by header equality, but explicit)
    if src.sprite_n != rc.sprite_n:
        all_ok = False
        diverged.append("sprite_n")
        print(f"  [FAIL] sprite_n: {src.sprite_n} vs {rc.sprite_n}")

    # audio: structural only -- sound count must match
    snd_ok = (src.sound_n == rc.sound_n)
    print(f"  [{'OK ' if snd_ok else 'FAIL'}] sound_n: {src.sound_n} vs {rc.sound_n}")
    if not snd_ok:
        all_ok = False
        diverged.append("sound_n")

    if all_ok:
        print(f"PASS  {name}")
        return 0
    print(f"FAIL  {name}  (diverged: {', '.join(diverged)})")
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
