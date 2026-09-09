#!/usr/bin/env python3
"""Find download links on atwiki outbound pages that are STILL LIVE.

The Wayback path (atwiki_resolve_archives.py -> atwiki_fetch_downloads.py)
only covers links with a usable snapshot. Measured 2026-09-09, 156 of the 302
links WITHOUT a snapshot are simply still online -- 48 of them on vector.co.jp
-- so they need no archive at all. This walks those pages directly.

It reuses atwiki_fetch_downloads' extractor and archive-extension regex rather
than growing a second, divergent copy.

COMMERCIAL STOREFRONTS ARE SKIPPED, not scraped: DLsite / Suruga-ya / Digiket
links are product listings for paid doujin games. They are recorded as
purchase references so the registry keeps the pointer, and no bytes are taken.

Read-only over the network; writes downloads[] into the per-game JSON with
source="live" so a later pull can tell them apart from Wayback finds.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.parse
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import atwiki_fetch_downloads as A  # noqa: E402

REPO = Path(__file__).resolve().parents[1]
GAMES = REPO / "data" / "fm2k_wiki" / "games"
PROBE = REPO / "data" / "fm2k_wiki" / "live_probe.json"

COMMERCIAL_HOSTS = ("dlsite.com", "suruga-ya.jp", "digiket.com", "dmm.co.jp",
                    "melonbooks.co.jp", "toranoana.jp", "booth.pm")

# Hosts whose final download hop is JS/POST-driven, so plain HTML scraping
# finds nothing and only wastes their bandwidth. MEASURED on vector.co.jp:
#   /soft/win95/game/seNNNNNN.html   detail page
#     -> /download/file/win95/game/fhNNNNNN.html   interstitial
#        -> no href to the file at all; the last hop needs a click.
# These are handed to tools/atwiki_playwright_pull.py, which already drives
# exactly this ("find and click the Download / ダウンロード / DL button").
JS_WALLED_HOSTS = ("vector.co.jp", "getuploader.com", "axfc.net", "fc2.com",
                   "mediafire.com", "onedrive.live.com", "drive.google.com")

# Per-host politeness. These are one-person Japanese fan sites; vector.co.jp is
# the only one that sees real volume here (48 links) and it is a large host.
HOST_MIN_INTERVAL = 2.0


def is_js_walled(url: str) -> bool:
    host = urllib.parse.urlparse(url).netloc.lower()
    return any(h in host for h in JS_WALLED_HOSTS)


def is_commercial(url: str) -> bool:
    host = urllib.parse.urlparse(url).netloc.lower()
    return any(h in host for h in COMMERCIAL_HOSTS)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--host", default=None,
                    help="only probe links on this host (e.g. www.vector.co.jp)")
    ap.add_argument("--throttle", type=float, default=1.0)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if not PROBE.exists():
        print(f"missing {PROBE} -- run tools/probe_live_links.py first")
        return 2
    probe = json.loads(PROBE.read_text(encoding="utf-8"))
    live = [r for r in probe if r["verdict"] in ("live", "redirected-offsite")]
    if args.host:
        live = [r for r in live
                if urllib.parse.urlparse(r["url"]).netloc == args.host]
    skipped = [r for r in live if is_commercial(r["url"])]
    live = [r for r in live if not is_commercial(r["url"])]
    js_walled = [r for r in live if is_js_walled(r["url"])]
    live = [r for r in live if not is_js_walled(r["url"])]
    if args.limit:
        live = live[:args.limit]

    print(f"{len(live)} live links to crawl "
          f"({len(skipped)} commercial listings recorded, not scraped; "
          f"{len(js_walled)} JS-walled -> atwiki_playwright_pull.py)")
    (REPO / "data" / "fm2k_wiki" / "js_walled.json").write_text(
        json.dumps(js_walled, ensure_ascii=False, indent=2), encoding="utf-8")

    last_hit: dict[str, float] = {}
    found_total = 0
    per_game: dict[str, list[str]] = {}
    for i, r in enumerate(live, 1):
        url = r.get("final") or r["url"]
        host = urllib.parse.urlparse(url).netloc
        wait = HOST_MIN_INTERVAL - (time.time() - last_hit.get(host, 0.0))
        if wait > 0:
            time.sleep(wait)
        body = A.fetch_url(url, timeout=25.0)
        last_hit[host] = time.time()
        if not body:
            print(f"  [{i}/{len(live)}] FETCH-FAILED  {url[:66]}", flush=True)
            continue
        html = body.decode("utf-8", errors="replace")
        links = A.extract_archive_links(html, url)
        if links:
            per_game.setdefault(r["atwiki_id"], []).extend(links)
            found_total += len(links)
        print(f"  [{i}/{len(live)}] {len(links):>2} link(s)  {url[:62]}",
              flush=True)
        time.sleep(args.throttle)

    print(f"\n{found_total} download link(s) across {len(per_game)} game(s)")
    if args.dry_run:
        print("(dry run -- nothing written)")
        return 0

    for gid, links in per_game.items():
        p = GAMES / f"{gid}.json"
        if not p.exists():
            continue
        d = json.loads(p.read_text(encoding="utf-8"))
        have = {e.get("url") for e in (d.get("downloads") or [])}
        for u in links:
            if u in have:
                continue
            d.setdefault("downloads", []).append({"url": u, "source": "live"})
            have.add(u)
        p.write_text(json.dumps(d, ensure_ascii=False, indent=2),
                     encoding="utf-8")
    for r in skipped:
        p = GAMES / f"{r['atwiki_id']}.json"
        if not p.exists():
            continue
        d = json.loads(p.read_text(encoding="utf-8"))
        refs = d.setdefault("purchase_links", [])
        if r["url"] not in refs:
            refs.append(r["url"])
        p.write_text(json.dumps(d, ensure_ascii=False, indent=2),
                     encoding="utf-8")
    print(f"wrote downloads into {len(per_game)} game record(s), "
          f"{len(skipped)} purchase reference(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
