#!/usr/bin/env python3
"""Harvest archived download files from Wayback by DIRECTORY, not per URL.

WHY THIS EXISTS. The per-URL flow costs, for every outbound link:
  1 CDX query  ->  1 homepage snapshot fetch  ->  N link resolutions
Across 515 links that is thousands of requests, and it only ever finds files
that happen to be linked from the one page we archived. On 2026-09-09 that
approach got us rate-limited by web.archive.org (curl and python both refused
at the IP level) after ~300 queries.

CDX can answer the whole question in ONE request per directory:

    url=<host>/<path>&matchType=prefix
      &filter=original:.*\\.(zip|lzh|rar|exe|7z)$
      &collapse=urlkey&fl=original,timestamp,statuscode

That returns every archive file ever captured under that path -- including
files the homepage never linked, and files from page revisions we do not hold.
515 links collapse to a few hundred distinct directories, so it is roughly an
order of magnitude fewer requests AND strictly better recall.

This is not rate-limit evasion; it is asking for the same information in the
shape the index is built to answer. Being lighter on the archive is the point.
"""
from __future__ import annotations

import argparse
import json
import time
import urllib.parse
import urllib.request
from collections import defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
GAMES = REPO / "data" / "fm2k_wiki" / "games"
OUT = REPO / "data" / "fm2k_wiki" / "cdx_harvest.json"
UA = "fm2k-archive-crawler/1.0 (+https://github.com/Armonte/wanwan)"
EXT_FILTER = r"original:.*\.(zip|lzh|lha|rar|exe|7z|tar\.gz|tgz)$"


def prefix_for(url: str) -> str | None:
    """The directory a file would sit in, as a CDX prefix key.

    Shared hosts (geocities, fc2, plala...) put each author under their own
    path, so the host alone would be far too broad -- one query for all of
    geocities.jp would be millions of rows. The containing directory is the
    right granularity: broad enough to catch files the page never linked,
    narrow enough to stay one author's space.
    """
    p = urllib.parse.urlparse(url)
    if not p.netloc:
        return None
    path = p.path
    if "." in path.rsplit("/", 1)[-1]:      # strip a filename
        path = path.rsplit("/", 1)[0]
    return (p.netloc + path).rstrip("/").lstrip("/") or p.netloc


def cdx(prefix: str, timeout: float = 60.0) -> list[list[str]]:
    q = urllib.parse.urlencode({
        "url": prefix, "matchType": "prefix", "output": "json",
        "fl": "original,timestamp,statuscode", "collapse": "urlkey",
        "filter": EXT_FILTER, "limit": "1000",
    })
    req = urllib.request.Request(
        "https://web.archive.org/cdx/search/cdx?" + q, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        body = r.read().decode("utf-8", "replace")
    if not body.strip():
        return []
    return json.loads(body)[1:]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--throttle", type=float, default=2.0,
                    help="seconds between CDX queries (be generous; this is a "
                         "free public archive and we need very few requests)")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--plan-only", action="store_true",
                    help="print the query plan and exit, no network")
    args = ap.parse_args()

    by_prefix: dict[str, set[str]] = defaultdict(set)
    for f in sorted(GAMES.glob("*.json")):
        d = json.loads(f.read_text(encoding="utf-8"))
        for o in (d.get("outbound") or []):
            pre = prefix_for(o["url"])
            if pre:
                by_prefix[pre].add(d["atwiki_id"])
    prefixes = sorted(by_prefix)
    n_links = sum(len(v) for v in by_prefix.values())
    print(f"{n_links} link-game pairs collapse to {len(prefixes)} CDX queries")
    if args.plan_only:
        for p in prefixes[:25]:
            print(f"   {len(by_prefix[p]):>2} game(s)  {p}")
        return 0
    if args.limit:
        prefixes = prefixes[: args.limit]

    results: dict[str, list] = {}
    n_files = fails = 0
    for i, pre in enumerate(prefixes, 1):
        try:
            rows = cdx(pre)
        except Exception as e:                          # noqa: BLE001
            fails += 1
            print(f"  [{i}/{len(prefixes)}] ERR {type(e).__name__:<18} {pre[:52]}",
                  flush=True)
            time.sleep(args.throttle * 3)
            continue
        if rows:
            results[pre] = [{"url": r[0], "timestamp": r[1], "status": r[2],
                             "games": sorted(by_prefix[pre])} for r in rows]
            n_files += len(rows)
        print(f"  [{i}/{len(prefixes)}] {len(rows):>3} file(s)  {pre[:56]}",
              flush=True)
        time.sleep(args.throttle)

    OUT.write_text(json.dumps(results, ensure_ascii=False, indent=2),
                   encoding="utf-8")
    print(f"\n{n_files} archived file(s) across {len(results)} directories "
          f"({fails} query failures)\n-> {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
