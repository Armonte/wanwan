#!/usr/bin/env python3
"""Pull the atwiki links whose final hop needs a click (Vector, getuploader...).

WHY A SEPARATE DRIVER: atwiki_playwright_pull.py drives off games/registry.json
resources. The JS-walled links found by probe_live_links.py live in the atwiki
per-game records instead, and the registry writers are non-atomic so wiring
them in mid-campaign risks the 450-row file. This reuses that tool's
`try_pull` -- the actual Playwright click chain, which already knows Vector's
two-hop shape (game.html -> click -> fh??????.html -> click -> ZIP) -- rather
than reimplementing it.

Measured 2026-09-09: 83 of the 156 live atwiki links are JS-walled, 48 of them
vector.co.jp. Plain HTTP finds nothing on those pages because the file link
does not exist in the HTML at all.

Paywalled storefronts (DLsite etc.) are skipped via the same is_paywalled()
guard the sibling tool uses, so we never appear to hand out paid content.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.parse
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import atwiki_playwright_pull as P  # noqa: E402

from playwright.sync_api import sync_playwright  # noqa: E402
from playwright_stealth import Stealth  # noqa: E402

REPO = Path(__file__).resolve().parents[1]
JS_WALLED = REPO / "data" / "fm2k_wiki" / "js_walled.json"
DL_DIR = Path("/mnt/d/fm2k-archive/atwiki-downloads")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--host", default=None, help="only this host")
    ap.add_argument("--throttle", type=float, default=2.0)
    ap.add_argument("--max-mb", type=int, default=400)
    args = ap.parse_args()

    if not JS_WALLED.exists():
        print(f"missing {JS_WALLED} -- run tools/live_discover_downloads.py")
        return 2
    items = json.loads(JS_WALLED.read_text(encoding="utf-8"))
    items = [i for i in items if not P.is_paywalled(i["url"])]
    if args.host:
        items = [i for i in items
                 if urllib.parse.urlparse(i["url"]).netloc == args.host]
    if args.limit:
        items = items[: args.limit]
    print(f"{len(items)} JS-walled link(s) to pull")

    max_bytes = args.max_mb * 1024 * 1024
    n_ok = n_fail = 0
    last_host: dict[str, float] = {}
    Stealth_ = Stealth(navigator_languages_override=("ja-JP", "ja", "en-US", "en"))
    with Stealth_.use_sync(sync_playwright()) as p:
        browser = p.chromium.launch(headless=True,
            args=["--disable-blink-features=AutomationControlled", "--no-sandbox"])
        ctx = browser.new_context(user_agent=P.USER_AGENT, locale="ja-JP",
                                  viewport={"width": 1280, "height": 900},
                                  accept_downloads=True)
        for i, it in enumerate(items, 1):
            url = it.get("final") or it["url"]
            host = urllib.parse.urlparse(url).netloc
            wait = args.throttle - (time.time() - last_host.get(host, 0.0))
            if wait > 0:
                time.sleep(wait)
            dest = DL_DIR / str(it["atwiki_id"])
            dest.mkdir(parents=True, exist_ok=True)
            page = ctx.new_page()
            try:
                r = P.try_pull(page, url, dest, max_bytes=max_bytes)
            except Exception as e:                      # noqa: BLE001
                r = {"ok": False, "method": "driver_exception",
                     "error": f"{type(e).__name__}: {e}"[:120]}
            finally:
                page.close()
                last_host[host] = time.time()
            if r.get("ok"):
                n_ok += 1
                print(f"[{i:3d}/{len(items)}] OK   {it['atwiki_id']:>5} "
                      f"{r.get('filename','?')[:34]} "
                      f"({r.get('size_bytes',0)/1e6:.1f} MB)", flush=True)
            else:
                n_fail += 1
                # try_pull reports failures under method/error, NOT "why".
                reason = r.get("method", "?")
                if r.get("error"):
                    reason += f" ({r['error'][:60]})"
                if r.get("kind_seen"):
                    reason += f" [got {r['kind_seen']}]"
                print(f"[{i:3d}/{len(items)}] --   {it['atwiki_id']:>5} "
                      f"{reason[:78]}", flush=True)
        browser.close()
    print(f"\npulled {n_ok}, failed {n_fail}, of {len(items)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
