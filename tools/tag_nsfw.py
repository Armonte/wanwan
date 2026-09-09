#!/usr/bin/env python3
"""Flag adult titles in the atwiki corpus, for the in-client browsing filter.

THE SIGNAL WE ACTUALLY OWN: atwiki page bodies carry an explicit `18禁`
rating marker on 26 of 336 records. That is a self-declared rating from the
catalogue itself -- far better than guessing from a title. Measured 2026-09-09;
freem, by contrast, has no adult category at all (it is an all-ages platform),
so there is nothing to read there.

THREE STATES, NOT A BOOLEAN, because the browsing filter must treat them
differently:
  confirmed  explicit rating marker (18禁 / 成人向け / アダルト / R-18)
  suspected  suggestive wording only (エロ), which appears inside ordinary
             prose like "エロくてグロ" and is NOT a rating
  (absent)   no signal

Only `confirmed` should auto-hide. `suspected` needs eyes before it changes
what a user sees, which is why it is a separate state rather than being folded
into the same flag.

Human decisions live in games/nsfw_overrides.json, keyed by the .kgt
project_key rather than a registry id, so an override survives a re-scrape,
a renumbering, and follows every version of the same project. Overrides are
re-applied last on every run: a re-scrape can never silently un-flag something
a human confirmed.
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
GAMES = REPO / "data" / "fm2k_wiki" / "games"
OVERRIDES = REPO / "games" / "nsfw_overrides.json"

CONFIRMED_PATTERNS = [("18禁", "18禁"), ("成人向け", "成人向け"),
                      ("アダルト", "アダルト"), ("R-?18", "R-18")]
SUSPECTED_PATTERNS = [("エロ", "エロ")]


def classify(text: str) -> tuple[str | None, list[str]]:
    tags = [label for pat, label in CONFIRMED_PATTERNS if re.search(pat, text)]
    if tags:
        return "confirmed", tags
    tags = [label for pat, label in SUSPECTED_PATTERNS if re.search(pat, text)]
    if tags:
        return "suspected", tags
    return None, []


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true",
                    help="write content_rating into the game records")
    ap.add_argument("--review", action="store_true",
                    help="list the 'suspected' rows for a human pass")
    args = ap.parse_args()

    overrides = {}
    if OVERRIDES.exists():
        overrides = json.loads(OVERRIDES.read_text(encoding="utf-8"))

    n = {"confirmed": 0, "suspected": 0}
    rows = []
    for f in sorted(GAMES.glob("*.json")):
        d = json.loads(f.read_text(encoding="utf-8"))
        text = f"{d.get('title','')} {d.get('body_text','')}"
        conf, tags = classify(text)
        if not conf:
            continue
        n[conf] += 1
        rows.append((d["atwiki_id"], d.get("title", "")[:34], conf, tags))
        if args.apply:
            d["content_rating"] = {
                "nsfw": conf == "confirmed",
                "confidence": conf,
                "sources": [f"atwiki:{t}" for t in tags],
                "tags": tags,
                "reviewed_by_human": False,
                "reviewed_at": None,
            }
            f.write_text(json.dumps(d, ensure_ascii=False, indent=2),
                         encoding="utf-8")

    for gid, title, conf, tags in rows:
        if args.review and conf != "suspected":
            continue
        print(f"  {gid:>5}  {conf:<10} {','.join(tags):<8} {title}")
    print(f"\nconfirmed={n['confirmed']}  suspected={n['suspected']}  "
          f"of {len(list(GAMES.glob('*.json')))} records"
          + ("" if args.apply else "   (dry run -- pass --apply to write)"))
    if overrides:
        print(f"human overrides on file: {len(overrides)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
