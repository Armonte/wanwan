#!/usr/bin/env python3
"""Transcode captured PNGs to WebP, dropping engine-common frames first.

Two things this does that a plain "convert everything" pass would not.

It DROPS ENGINE FRAMES. Every 2DFM game boots through the same sequence,
including a card that belongs to the engine rather than any game, so the
first several front frames are byte-identical across titles. Any frame whose
hash appears in three or more games is dropped. Three, not two, because two
versions of one game legitimately share frames and a >=2 rule discards their
real content.

It emits TWO sizes. The launcher's tiles are a couple of hundred pixels
wide, so a 320x240 thumb is all they need; the full-size frame is kept for
anywhere that wants detail. Encoding happens here, offline, so nothing new
has to ship inside the game process -- the hook keeps writing plain PNG.

Measured on real captures: WebP q75 at 320x240 runs 5-6x smaller than the
equivalent PNG. Lossless WebP is a trap at this size -- it came out LARGER
than a 256-colour PNG.

    tools/capture_transcode.py [--src DIR] [--quality 75] [--keep-png]
"""
import argparse
import hashlib
import shutil
import sys
from collections import Counter, defaultdict
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("PIL required: pip install pillow")
    sys.exit(2)

THUMB = (320, 240)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="/mnt/c/games/captures")
    ap.add_argument("--quality", type=int, default=75)
    ap.add_argument("--keep-png", action="store_true")
    args = ap.parse_args()

    src = Path(args.src)
    if not src.is_dir():
        print(f"no such directory: {src}")
        return 2

    # Hash every frame, then find the ones shared across three or more games.
    per_game = defaultdict(dict)
    counts = Counter()
    for d in sorted(p for p in src.iterdir() if p.is_dir()):
        for png in sorted(d.glob("*.png")):
            per_game[d.name][png] = hashlib.sha1(png.read_bytes()).hexdigest()
        for h in set(per_game[d.name].values()):
            counts[h] += 1
    engine = {h for h, n in counts.items() if n >= 3}

    png_bytes = webp_bytes = thumb_bytes = 0
    kept = dropped = 0

    for game, files in sorted(per_game.items()):
        for png, h in sorted(files.items()):
            if h in engine:
                dropped += 1
                png.unlink()
                continue
            kept += 1
            png_bytes += png.stat().st_size
            im = Image.open(png).convert("RGB")

            # THUMB ONLY for the roster sweep. A character shot is shown
            # small -- the review gallery's card, and later a face beside a
            # replay -- and there are dozens per game: keeping a 640x480
            # copy of each cost 126 MB across the library, four fifths of
            # the whole capture folder, for pixels nothing reads.
            if png.name.startswith("char_"):
                thumb = png.with_name(png.stem + ".thumb.webp")
                im.resize(THUMB, Image.NEAREST).save(
                    thumb, "WEBP", quality=args.quality, method=6)
                thumb_bytes += thumb.stat().st_size
                if not args.keep_png:
                    png.unlink()
                continue

            full = png.with_suffix(".webp")
            im.save(full, "WEBP", quality=args.quality, method=6)
            webp_bytes += full.stat().st_size

            # NEAREST, not Lanczos: these are hand-placed pixels and a smooth
            # filter turns readable sprite art into mush at half size.
            thumb = png.with_name(png.stem + ".thumb.webp")
            im.resize(THUMB, Image.NEAREST).save(
                thumb, "WEBP", quality=args.quality, method=6)
            thumb_bytes += thumb.stat().st_size

            if not args.keep_png:
                png.unlink()

    # ---- index the launcher reads -------------------------------------
    # Preference order matters. The front-end series walks splash cards ->
    # title, so the HIGHEST-numbered surviving front frame is the one most
    # likely to be the game's own title screen (measured on wanwan: front_06
    # was the title, front_01..04 were engine boot). CSS is the next best
    # thing to look at in a grid, and battle last -- it is dominated by the
    # FIGHT banner and reads less like an identifying image.
    paths = {}
    pf = src / "paths.tsv"
    if pf.exists():
        for line in pf.read_text(encoding="utf-8").splitlines():
            if "\t" in line:
                gid, exe = line.split("\t", 1)
                paths[gid] = exe

    def pick(d: Path):
        fronts = sorted(d.glob("front_*.thumb.webp"))
        if fronts:
            return fronts[-1]
        for name in ("css_initial.thumb.webp", "battle.thumb.webp"):
            if (d / name).exists():
                return d / name
        return None

    rows = []
    for d in sorted(p for p in src.iterdir() if p.is_dir()):
        exe = paths.get(d.name)
        thumb = pick(d)
        if exe and thumb:
            w = str(thumb)
            if w.startswith("/mnt/") and len(w) > 6:
                w = w[5].upper() + ":" + w[6:].replace("/", "\\")
            rows.append(f"{exe}\t{w}")
    (src / "index.tsv").write_text("\n".join(rows), encoding="utf-8")

    def mb(n): return n / (1024 * 1024)

    print(f"games            : {len(per_game)}")
    print(f"engine frames    : {len(engine)} distinct, {dropped} files dropped")
    print(f"frames kept      : {kept}")
    print(f"PNG   (in)       : {mb(png_bytes):7.1f} MB")
    print(f"WebP  full       : {mb(webp_bytes):7.1f} MB  "
          f"({webp_bytes * 100 // max(png_bytes, 1)}% of PNG)")
    print(f"WebP  thumb      : {mb(thumb_bytes):7.1f} MB")
    print(f"total shipped    : {mb(webp_bytes + thumb_bytes):7.1f} MB")
    print(f"index.tsv        : {len(rows)} game(s) mapped to a thumbnail")
    return 0


if __name__ == "__main__":
    sys.exit(main())
