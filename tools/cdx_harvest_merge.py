#!/usr/bin/env python3
"""Turn cdx_harvest.json into download candidates on the atwiki game records.

Each harvested row is (original_url, timestamp, status) for a file captured
under a directory that one or more games link to. The fetchable URL is the
Wayback replay form with the `id_` modifier, which serves the ORIGINAL bytes
rather than a rewritten page:

    https://web.archive.org/web/<timestamp>id_/<original>

ONE DIRECTORY CAN SERVE SEVERAL GAMES. Authors host multiple titles in one
folder, so a file found under a shared prefix is attached to every game that
links there. That is deliberately a CANDIDATE list, not an assertion -- the
.kgt classifier decides what each archive actually is after download, and the
identity index dedupes. Attaching a file to the wrong game costs one download;
missing it costs the game.

Skips files already present, and files whose capture was not a 2xx.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
GAMES = REPO / "data" / "fm2k_wiki" / "games"
HARVEST = REPO / "data" / "fm2k_wiki" / "cdx_harvest.json"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--max-per-game", type=int, default=12,
                    help="cap candidates per game so one huge author folder "
                         "cannot dominate the pull queue")
    args = ap.parse_args()

    if not HARVEST.exists():
        print(f"missing {HARVEST} -- run tools/cdx_harvest.py first")
        return 2
    harvest = json.loads(HARVEST.read_text(encoding="utf-8"))

    per_game: dict[str, list[dict]] = {}
    skipped_status = 0
    for prefix, rows in harvest.items():
        for r in rows:
            if not str(r.get("status", "")).startswith("2"):
                skipped_status += 1
                continue
            wb = f"https://web.archive.org/web/{r['timestamp']}id_/{r['url']}"
            for gid in r["games"]:
                per_game.setdefault(gid, []).append({
                    "original_url": wb,
                    "found_via": f"cdx:{prefix}",
                    "filename": r["url"].rsplit("/", 1)[-1],
                    "source": "cdx-harvest",
                })

    n_new = 0
    for gid, cands in sorted(per_game.items()):
        p = GAMES / f"{gid}.json"
        if not p.exists():
            continue
        d = json.loads(p.read_text(encoding="utf-8"))
        have = {e.get("original_url") for e in (d.get("downloads") or [])}
        have_names = {e.get("filename") for e in (d.get("downloads") or [])}
        added = 0
        for c in cands:
            if added >= args.max_per_game:
                break
            if c["original_url"] in have or c["filename"] in have_names:
                continue
            d.setdefault("downloads", []).append(c)
            have.add(c["original_url"]); have_names.add(c["filename"])
            added += 1; n_new += 1
        if added and args.apply:
            p.write_text(json.dumps(d, ensure_ascii=False, indent=2),
                         encoding="utf-8")

    print(f"{len(harvest)} directories -> {n_new} new candidate(s) "
          f"across {len(per_game)} game(s)  (skipped {skipped_status} non-2xx)")
    if not args.apply:
        print("(dry run -- pass --apply to write)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
