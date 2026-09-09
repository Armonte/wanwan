#!/usr/bin/env python3
"""Walk the user's local FM2K/FM95 game roots, hash every .exe with
xxh64 (matches the launcher's identification hash), parse each .kgt
header for its project name, and join the results to the atwiki
registry. Output is the per-game inventory the audit step uses to
say "we have / we don't have / we have a different version".

Cheap to run end-to-end: no network, no archive operations. Roughly
linear in number of .exe + .kgt files (102 games × ~50ms hash =
~5s on a typical local install).

Output schema (data/fm2k_wiki/local_inventory.json):
{
  "scan_roots":  ["d:/games/fm2k", "c:/games/2dfm"],
  "scan_time":   "2026-05-09T...",
  "local_games": [
    {
      "exe_path":         "d:/games/fm2k/.../X.exe",
      "exe_xxh64":        "0x506FF9AB93D15134",
      "exe_size":         1234567,
      "exe_mtime":        1700000000,
      "kgt_path":         "d:/games/fm2k/.../X.kgt",
      "kgt_signature":    "2DKGT2K" | "2DKGT95" | ... ,
      "engine_guess":     "FM2K" | "FM95",
      "project_name":     "WonderfulWorld_ver_0946",
      "atwiki_match": {
        "atwiki_id":  "86",
        "title":      "WonderfulWorld",
        "score":      0.83,
        "method":     "project_name|dirname|filename"
      }
    }
  ]
}
"""

import argparse
import json
import os
import re
import sys
import time
from pathlib import Path
from typing import Optional

import xxhash


REPO = Path(__file__).resolve().parents[1]
INDEX = REPO / "data" / "fm2k_wiki" / "index.json"
OUT_PATH = REPO / "data" / "fm2k_wiki" / "local_inventory.json"

# Default scan roots — the user's typical install layouts. Override
# with --root on the command line. Best-effort; missing roots are
# silently skipped.
DEFAULT_ROOTS = [
    "/mnt/d/Games/fm2k",
    "/mnt/d/Games/fm95",   # 30 installed FM95 games were invisible without this
    "/mnt/c/games/2dfm",
    "/mnt/c/games/fm2k",
]

# .kgt signatures, MEASURED over all 150 .kgt in the local library
# 2026-09-09, not guessed:
#     2DKGT2K  86    2DKGT2G  34    KGTGAME  30
# Two corrections to the previous table, which produced NO correct answers:
#   1. It was keyed on 8 bytes (b"2DKGT2K\x00") but looked up with the full
#      16-byte signature, so every lookup missed and every game fell through
#      to the "FM2K" default -- including the 30 FM95 ones. Match on the
#      7-byte ASCII prefix instead.
#   2. "2DKGT95" does not exist in the wild; the real FM95 magic is
#      "KGTGAME", and "2DKGT2G" (a second FM2K variant) was absent entirely.
# "2DKGT95" is kept only because FM2K_KgtParser.cpp lists it.
SIG_TO_ENGINE = {
    b"2DKGT2K": "FM2K",
    b"2DKGT2G": "FM2K",
    b"KGTGAME": "FM95",
    b"2DKGT95": "FM95",
}


def engine_for_sig(sig: bytes) -> str:
    """Engine from the 16-byte .kgt signature, matched on its ASCII prefix."""
    return SIG_TO_ENGINE.get(sig[:7], "FM2K")


def hash_exe(p: Path) -> int:
    h = xxhash.xxh64(seed=0)
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.intdigest()


def parse_kgt_header(p: Path) -> tuple[bytes, str]:
    """Read the 16-byte signature + 256-byte NameInfo from a .kgt and
    return (signature, project_name). project_name is decoded via
    CP932 (Shift-JIS) since 2DFM was a Japanese-locale tool and the
    project name field is always SJIS bytes."""
    try:
        with p.open("rb") as f:
            head = f.read(272)
    except OSError:
        return b"", ""
    if len(head) < 272:
        return b"", ""
    sig = head[:16]
    name_bytes = head[16:272]
    # NameInfo is null-padded; strip and decode.
    name_bytes = name_bytes.split(b"\x00", 1)[0]
    try:
        name = name_bytes.decode("cp932", errors="replace").strip()
    except Exception:
        name = ""
    return sig, name


def normalize_for_match(s: str) -> str:
    """Canonicalize a name for fuzzy matching: lowercase, strip
    spaces/punctuation, drop common version suffixes. Goal is to make
    e.g. "WonderfulWorld_ver_0946" and "WonderfulWorld" collide."""
    s = s.lower()
    s = re.sub(r"_?ver[_\-\s]*\d[\d\._]*", "", s)         # _ver_0946
    s = re.sub(r"\s*v?\d+(?:\.\d+)+\s*$", "", s)          # trailing v1.0 / 0.5
    s = re.sub(r"[\s_\-　　]+", "", s)                # whitespace + JP fullwidth
    s = re.sub(r"[【】（）()「」『』:：・/\\|]+", "", s)
    return s


def best_atwiki_match(candidate: str, atwiki: list[dict]) -> tuple[Optional[dict], float, str]:
    """Pick the atwiki entry whose normalized title best matches
    `candidate`. Returns (entry, score, method).

    Scoring is a simple substring/symmetric overlap — JP titles defy
    fancy edit-distance heuristics anyway. Threshold the caller cares
    about lives in the audit script.
    """
    if not candidate:
        return None, 0.0, ""
    norm_cand = normalize_for_match(candidate)
    if not norm_cand:
        return None, 0.0, ""

    best: Optional[dict] = None
    best_score = 0.0
    best_method = ""
    for entry in atwiki:
        norm_title = normalize_for_match(entry["title"])
        if not norm_title:
            continue
        # Symmetric: how much of each name appears in the other?
        # We treat "exact substring" as a strong signal since a typical
        # atwiki title is a clean game name and the candidate is a
        # filename / dir / project_name with extra cruft.
        if norm_cand == norm_title:
            return entry, 1.0, "exact"
        if norm_cand in norm_title or norm_title in norm_cand:
            shorter, longer = sorted([norm_cand, norm_title], key=len)
            if not longer:
                continue
            score = len(shorter) / len(longer)
            if score > best_score:
                best, best_score = entry, score
                best_method = "substring"
    return best, best_score, best_method


def discover_local_games(roots: list[Path]) -> list[dict]:
    """Walk each root, find exe+kgt pairs (FM2K layout) and bare
    .player+.exe dirs (FM95 fallback). Mirrors the launcher's
    discovery semantics so the inventory matches what the launcher
    sees at runtime."""
    games: list[dict] = []
    for root in roots:
        if not root.exists():
            print(f"  skip (missing): {root}", flush=True)
            continue
        print(f"  scanning {root}", flush=True)
        for dirpath, dirs, files in os.walk(root, followlinks=False):
            d = Path(dirpath)
            kgts = [f for f in files if f.lower().endswith(".kgt")]
            exes = [f for f in files if f.lower().endswith(".exe")]
            # FM2K layout: kgt next to matching-stem exe
            for kgt in kgts:
                stem = kgt.rsplit(".", 1)[0]
                exe_match = next((e for e in exes
                                  if e.rsplit(".", 1)[0].lower() == stem.lower()), None)
                # KGT-FIRST. This used to `continue` when no same-stem .exe sat
                # beside the .kgt, which silently dropped 43 of 150 installed
                # games (many ship as <Game>.kgt next to a differently-named or
                # absent launcher exe). The .kgt IS the game; the exe is a
                # shared ~1.2 MB runtime stub and is optional here.
                exe_p = (d / exe_match) if exe_match else None
                kgt_p = d / kgt
                st = None
                if exe_p is not None:
                    try:
                        st = exe_p.stat()
                    except OSError:
                        exe_p = None
                sig, project = parse_kgt_header(kgt_p)
                games.append({
                    # exe_* stay in the schema (atwiki_audit.py and
                    # atwiki_merge_to_registry.py read them) but are now
                    # nullable, because a .kgt no longer requires a sibling exe.
                    "exe_path":      str(exe_p).replace("\\", "/") if exe_p else None,
                    "exe_xxh64":     f"0x{hash_exe(exe_p):016X}" if exe_p else None,
                    "exe_size":      st.st_size if st else None,
                    "exe_mtime":     int(st.st_mtime) if st else None,
                    "kgt_path":      str(kgt_p).replace("\\", "/"),
                    "kgt_signature": sig.decode("ascii", errors="replace").rstrip("\x00"),
                    "engine_guess":  engine_for_sig(sig),
                    "project_name":  project,
                })
    return games


def join_atwiki(local: list[dict], atwiki: list[dict]) -> None:
    """In-place: tag each local game with its best-fit atwiki entry."""
    for g in local:
        # Try matching in this priority: project_name (best signal),
        # exe basename, dir basename. Pick the highest score across
        # all three.
        # exe_path is nullable since the kgt-first walk landed, so fall back
        # to the .kgt for the filename/dirname candidates. The .kgt is always
        # present -- it is what the walk keys on.
        anchor = Path(g["exe_path"] or g["kgt_path"])
        candidates = [
            ("project_name", g.get("project_name", "")),
            ("filename",     anchor.stem),
            ("dirname",      anchor.parent.name),
        ]
        best = None
        best_score = 0.0
        best_method = ""
        for label, cand in candidates:
            entry, score, _ = best_atwiki_match(cand, atwiki)
            if entry and score > best_score:
                best, best_score, best_method = entry, score, label
        g["atwiki_match"] = ({
            "atwiki_id": best["atwiki_id"],
            "title":     best["title"],
            "score":     round(best_score, 3),
            "method":    best_method,
        } if best and best_score >= 0.5 else None)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", action="append", default=[],
                    help="scan root (repeatable). Defaults to common install paths.")
    ap.add_argument("--out", type=Path, default=OUT_PATH)
    args = ap.parse_args()

    roots = [Path(r) for r in (args.root or DEFAULT_ROOTS)]

    if not INDEX.exists():
        print(f"missing {INDEX} — run atwiki_extract_index.py first", file=sys.stderr)
        return 1
    atwiki = json.load(INDEX.open(encoding="utf-8"))["games"]

    print(f"scanning {len(roots)} roots:")
    t0 = time.time()
    local = discover_local_games(roots)
    print(f"  found {len(local)} local games in {time.time() - t0:.1f}s")

    join_atwiki(local, atwiki)
    matched = sum(1 for g in local if g.get("atwiki_match"))
    print(f"  {matched}/{len(local)} matched to an atwiki entry")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps({
        "scan_roots":  [str(r) for r in roots],
        "scan_time":   time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "local_games": local,
    }, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"  wrote -> {args.out.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
