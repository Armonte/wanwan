#!/usr/bin/env python3
"""Pick the thumbnail each game shows in the launcher, from its captures.

`capture_transcode.py` took the highest-numbered surviving front frame on
the theory that the front-end walk ends on the title screen. Measured on
101 games it often ends on a fade-out, a credits card, a blank or a plain
sky: a frame the walk happened to stop on, not the frame that identifies
the game. This scores EVERY candidate in the folder instead and takes the
best, and it respects a per-game `picks.txt` the launcher's review UI
writes (lead first, then approved shots in order) so a human decision
always wins over the heuristic.

Scoring, per full-size .webp (the .thumb.webp is derived from it):
  - rejected outright: mean luma < 0.06 (black / fade), luma spread < 0.05
    (flat card), or one colour covering > 85% of the frame (blank, sky);
  - score = 2.0 * luma spread + colourfulness (Hasler-Suesstrunk) +
    3.0 * edge density (mean of a FIND_EDGES pass);
  - front frames get 1.15x (a title screen is the identifying image), CSS
    1.0x, battle 0.9x (dominated by the FIGHT banner).

Usage:
    tools/capture_pick.py                 # rewrite captures/index.tsv
    tools/capture_pick.py --sheet out.png # also draw a contact sheet
    tools/capture_pick.py --explain 8-marbles-2
"""
import argparse, sys
from pathlib import Path

from PIL import Image, ImageFilter, ImageStat

ROOT = Path("/mnt/c/games/captures")


def score_image(path: Path):
    """Return (score, reason). score < 0 means rejected."""
    try:
        im = Image.open(path).convert("RGB")
    except Exception as e:  # noqa: BLE001
        return -1.0, f"unreadable: {e}"
    im.thumbnail((320, 240))
    lum = im.convert("L")
    st = ImageStat.Stat(lum)
    mean = st.mean[0] / 255.0
    spread = st.stddev[0] / 255.0
    if mean < 0.06:
        return -1.0, f"near-black (mean {mean:.2f})"
    if spread < 0.05:
        return -1.0, f"flat (spread {spread:.2f})"
    # dominant colour share: quantise to 4 bits per channel and count
    q = im.quantize(colors=16, method=Image.Quantize.MEDIANCUT)
    hist = q.histogram()
    share = max(hist) / float(sum(hist))
    if share > 0.85:
        return -1.0, f"one colour covers {share:.0%}"
    # colourfulness (Hasler & Suesstrunk)
    px = im.split()
    r, g, b = (ImageStat.Stat(c) for c in px)
    rg_mean = abs(r.mean[0] - g.mean[0])
    yb_mean = abs(0.5 * (r.mean[0] + g.mean[0]) - b.mean[0])
    rg_std = abs(r.stddev[0] - g.stddev[0]) + 1.0
    yb_std = abs(0.5 * (r.stddev[0] + g.stddev[0]) - b.stddev[0]) + 1.0
    colourful = ((rg_std ** 2 + yb_std ** 2) ** 0.5 + 0.3 * (rg_mean ** 2 + yb_mean ** 2) ** 0.5) / 255.0
    edges = ImageStat.Stat(lum.filter(ImageFilter.FIND_EDGES)).mean[0] / 255.0
    score = 2.0 * spread + colourful + 3.0 * edges
    return score, f"spread {spread:.2f} colour {colourful:.2f} edges {edges:.2f} dominant {share:.0%}"


def weight(name: str) -> float:
    if name.startswith("front_"):
        return 1.15
    if name.startswith("css"):
        return 1.0
    if name.startswith("battle"):
        return 0.9
    return 1.0


def candidates(d: Path):
    """The frames that could be the GAME's thumbnail.

    char_* is excluded: those are per-character portraits from the roster
    sweep, they only exist as thumbs, and one character's select-screen
    pose does not identify a game the way its title or CSS does.
    """
    return sorted(p for p in d.glob("*.webp")
                  if not p.name.endswith(".thumb.webp")
                  and not p.name.startswith("char_"))


def read_picks(d: Path):
    """picks.txt: `lead <name>`, `ok <name>`, `veto <name>` (host_captures.h)."""
    lead, ok, veto = None, [], []
    pk = d / "picks.txt"
    if pk.exists():
        for line in pk.read_text(encoding="utf-8").splitlines():
            parts = line.split()
            if len(parts) < 2 or parts[0].startswith("#"):
                continue
            if parts[0] == "lead":
                lead = parts[1]
            elif parts[0] == "ok":
                ok.append(parts[1])
            elif parts[0] == "veto":
                veto.append(parts[1])
    return lead, ok, veto


def pick(d: Path, explain=False):
    """The thumb path to show, or None. A reviewed decision wins."""
    lead, ok, veto = read_picks(d)
    for name in ([lead] if lead else []) + ok:
        t = d / f"{name}.thumb.webp"
        if t.exists():
            if explain:
                print(f"  picks.txt -> {t.name}")
            return t
    best, best_score = None, -1.0
    for p in candidates(d):
        if p.stem in veto:
            if explain:
                print(f"  {p.name:24s} vetoed")
            continue
        s, why = score_image(p)
        w = weight(p.name)
        ws = s * w if s >= 0 else s
        if explain:
            print(f"  {p.name:24s} {ws:6.3f}  {why}")
        if ws > best_score:
            best, best_score = p, ws
    if best is None:
        return None
    t = best.with_name(best.stem + ".thumb.webp")
    return t if t.exists() else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(ROOT))
    ap.add_argument("--sheet", help="write a contact sheet PNG of the picks")
    ap.add_argument("--explain", help="print every candidate's score for one game dir")
    args = ap.parse_args()
    root = Path(args.root)
    if args.explain:
        pick(root / args.explain, explain=True)
        return 0

    paths = {}
    pf = root / "paths.tsv"
    if pf.exists():
        for line in pf.read_text(encoding="utf-8").splitlines():
            if "\t" in line:
                gid, exe = line.split("\t", 1)
                paths[gid] = exe

    rows, tiles, unpicked = [], [], []
    for d in sorted(p for p in root.iterdir() if p.is_dir()):
        exe = paths.get(d.name)
        t = pick(d)
        if not t:
            unpicked.append(d.name)
            continue
        if exe:
            w = str(t)
            if w.startswith("/mnt/") and len(w) > 6:
                w = w[5].upper() + ":" + w[6:].replace("/", "\\")
            rows.append(f"{exe}\t{w}")
        tiles.append((d.name, t))
    (root / "index.tsv").write_text("\n".join(rows), encoding="utf-8")
    print(f"index.tsv: {len(rows)} game(s); no usable capture: {len(unpicked)} {unpicked[:8]}")

    if args.sheet:
        from PIL import ImageDraw
        cols = 11
        n = len(tiles)
        rws = (n + cols - 1) // cols
        sheet = Image.new("RGB", (cols * 162, max(1, rws) * 122), (20, 20, 20))
        for i, (name, t) in enumerate(tiles):
            im = Image.open(t).convert("RGB").resize((160, 120))
            dr = ImageDraw.Draw(im)
            dr.rectangle([0, 108, 160, 120], fill=(0, 0, 0))
            dr.text((2, 108), f"{name[:20]} {t.name.replace('.thumb.webp', '')}", fill=(255, 255, 0))
            sheet.paste(im, ((i % cols) * 162, (i // cols) * 122))
        sheet.save(args.sheet)
        print(f"sheet: {args.sheet}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
