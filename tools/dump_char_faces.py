#!/usr/bin/env python3
"""Extract a small face image per character, for every installed game.

Feeds the replay browser: a replay header stores p1_char_id / p2_char_id,
kgt_roster.py turns those into a .player file, and this turns that into a
picture small enough to sit in a table row.

USES THE STAGE FACE (background script 23), not the CSS portrait (22).
The portrait is around 520x530 -- a character illustration, forty times
the pixels a row needs and the wrong shape besides. The stage face is the
~80x78 in-game head icon, which is what a row actually wants. Falls back
to the portrait for characters that have no face.

Index 0 is transparent by construction in this codec (opcode 0 = SKIP,
leaving the zero fill), so it becomes the alpha channel rather than a
black rectangle around every head.
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image


def to_win(p):
    """/mnt/c/x -> C:\\x. The index is READ BY A WINDOWS EXE, so it has to
    carry Windows paths even though this script runs under WSL -- the same
    convention the thumbnail index already uses."""
    p = str(p)
    if p.startswith("/mnt/") and len(p) > 6 and p[6] == "/":
        return p[5].upper() + ":" + p[6:].replace("/", "\\")
    return p

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kgt_roster import roster  # noqa: E402

HERE = Path(__file__).resolve().parent
DUMPER = HERE / "dump_player_pics.py"


def _to_rgba(bmp_path):
    """8bpp indexed BMP -> RGBA, with palette index 0 as transparent."""
    im = Image.open(bmp_path)
    if im.mode != "P":
        return im.convert("RGBA")
    # Read the RAW palette indices. im.point() on a P-mode image maps
    # through the palette rather than over the index values, so building
    # the mask that way yields an almost entirely zero alpha -- every face
    # came out invisible against the dark UI, which looks like a bad
    # extraction rather than a bad mask.
    raw = Image.frombytes("L", im.size, im.tobytes())
    alpha = raw.point(lambda v: 0 if v == 0 else 255)
    rgb = im.convert("RGB")
    out = rgb.convert("RGBA")
    out.putalpha(alpha)
    return out


def _trim(im):
    """Crop to the visible pixels. These images are mostly empty space."""
    box = im.getbbox()
    return im.crop(box) if box else im


def find_games(root, max_depth=3):
    """Directories that hold a .kgt, at any depth up to max_depth.

    NOT just root's immediate children. Many installs are a container
    folder holding the game one level further down -- "AHamUWU/Hypothetical
    Horse Race Tests Fighting Game Release 0.1" -- and a one-level scan
    silently skipped every one of them. That looked like a format problem
    (packed assets) when it was only a depth problem.
    """
    out = []
    def walk(d, depth):
        if depth > max_depth:
            return
        try:
            kids = sorted(d.iterdir())
        except OSError:
            return
        if any(k.suffix.lower() == ".kgt" for k in kids if k.is_file()):
            out.append(d)
            return          # a game does not contain another game
        for k in kids:
            if k.is_dir():
                walk(k, depth + 1)
    walk(Path(root), 0)
    return out


def faces_for_game(game_dir, out_dir, height=40, quality=80):
    game_dir = Path(game_dir)
    kgts = list(game_dir.glob("*.kgt"))
    if not kgts:
        return []
    try:
        slots = roster(kgts[0])
    except Exception as e:
        print("  roster failed: %s" % e, file=sys.stderr)
        return []

    players = [p for p in game_dir.glob("*.player")]
    if not players:
        return []

    written = []
    with tempfile.TemporaryDirectory() as tmp:
        # One dumper invocation for the whole game rather than one per
        # character: it accepts many files, and process startup dominates
        # the work for images this small.
        subprocess.run(
            [sys.executable, str(DUMPER), "--portrait", "--out", tmp]
            + [str(p) for p in players],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        dumped = list(Path(tmp).glob("*.bmp"))

        out_dir = Path(out_dir) / game_dir.name
        out_dir.mkdir(parents=True, exist_ok=True)
        for slot, name in slots:
            # The dumper names files "<stem>_bg23_stageAvatar_...bmp".
            face = [f for f in dumped if f.name.startswith(name + "_bg23")]
            if not face:
                face = [f for f in dumped if f.name.startswith(name + "_bg22")]
            if not face:
                continue
            im = _trim(_to_rgba(face[0]))
            if im.width == 0 or im.height == 0:
                continue
            w = max(1, round(im.width * height / im.height))
            im = im.resize((w, height), Image.LANCZOS)
            dst = out_dir / ("%02d.webp" % slot)
            im.save(dst, "WEBP", quality=quality, method=6)
            written.append((game_dir.name, slot, name, dst))
    return written


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("roots", nargs="+", help="games root directories")
    ap.add_argument("--out", default="/mnt/c/games/captures/faces")
    ap.add_argument("--height", type=int, default=40)
    ap.add_argument("--index", default=None,
                    help="index tsv path (default <out>/faces.tsv)")
    a = ap.parse_args()

    rows = []
    for root in a.roots:
        for game in find_games(Path(root)):
            got = faces_for_game(game, a.out, a.height)
            if got:
                print("%-40s %d faces" % (game.name, len(got)))
                rows.extend(got)

    index = Path(a.index) if a.index else Path(a.out) / "faces.tsv"
    index.parent.mkdir(parents=True, exist_ok=True)
    with open(index, "w", encoding="utf-8") as f:
        for game, slot, name, dst in rows:
            f.write("%s\t%d\t%s\t%s\n" % (game, slot, name, to_win(dst)))
    print("== %d faces across %d games -> %s ==" %
          (len(rows), len({r[0] for r in rows}), index))


if __name__ == "__main__":
    main()
