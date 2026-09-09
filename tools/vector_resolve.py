#!/usr/bin/env python3
"""Resolve vector.co.jp game pages to their direct file URLs. Plain HTTP.

VECTOR DOES NOT NEED A BROWSER. The chain is two ordinary GETs:

  1. /soft/win95/game/seNNNNNN.html      (or /soft/dl/win95/game/...)
       -> href  /download/file/win95/game/fhNNNNNN.html
  2. /download/file/win95/game/fhNNNNNN.html
       -> href  https://ftp.vector.co.jp/<a>/<b>/<c>/<file>.<ext>   <- the file

I previously concluded this hop had "no href to the file at all" and routed
all 48 Vector links to the Playwright puller, where they failed 4/4 with
no_download_after_clicks. That conclusion came from inspecting a 0-BYTE
file -- an earlier curl had returned nothing and I read the empty result as
"the page has no link". Always check the size before concluding from content.

Pages are Shift-JIS (CP932); only the hrefs are ASCII, but decode properly
anyway so the titles are readable.
"""
from __future__ import annotations

import argparse
import json
import re
import time
import urllib.parse
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

REPO = Path(__file__).resolve().parents[1]
JS_WALLED = REPO / "data" / "fm2k_wiki" / "js_walled.json"
GAMES = REPO / "data" / "fm2k_wiki" / "games"
UA = "Mozilla/5.0 (compatible; fm2k-archive-crawler/1.0)"

FH_RE = re.compile(r'href="(/download/file/[^"]+\.html)"')
FTP_RE = re.compile(r'href="(https?://ftp\.vector\.co\.jp/[^"]+)"')


def get(url: str, timeout: float = 25.0) -> str | None:
    try:
        with urlopen(Request(url, headers={"User-Agent": UA}), timeout=timeout) as r:
            return r.read().decode("cp932", errors="replace")
    except (HTTPError, URLError, TimeoutError, OSError):
        return None


def resolve(page_url: str, throttle: float = 1.5) -> str | None:
    html = get(page_url)
    if not html:
        return None
    # Some links already point at the /download/file/ page.
    m = FTP_RE.search(html)
    if m:
        return m.group(1)
    m = FH_RE.search(html)
    if not m:
        # THE /soft/dl/ VARIANT. Vector has two detail URLs per title:
        #   /soft/win95/game/seNNNNNN.html      -- description only
        #   /soft/dl/win95/game/seNNNNNN.html   -- carries the /download/file/ link
        # atwiki links to whichever the author pasted, and roughly two thirds
        # are the description form, which is why a first pass resolved only
        # 2 of 6.
        if "/soft/dl/" not in page_url:
            alt = page_url.replace("/soft/", "/soft/dl/", 1)
            time.sleep(throttle)
            html_alt = get(alt)
            if html_alt:
                m2 = FTP_RE.search(html_alt)
                if m2:
                    return m2.group(1)
                m = FH_RE.search(html_alt)
                if m:
                    page_url = alt
        if not m:
            return None
    nxt = urllib.parse.urljoin(page_url, m.group(1))
    time.sleep(throttle)
    html2 = get(nxt)
    if not html2:
        return None
    m2 = FTP_RE.search(html2)
    return m2.group(1) if m2 else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--throttle", type=float, default=1.5)
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    items = json.loads(JS_WALLED.read_text(encoding="utf-8"))
    items = [i for i in items
             if "vector.co.jp" in urllib.parse.urlparse(i["url"]).netloc]
    if args.limit:
        items = items[: args.limit]
    print(f"resolving {len(items)} vector link(s)")

    found: dict[str, list[str]] = {}
    n = 0
    for i, it in enumerate(items, 1):
        url = it.get("final") or it["url"]
        direct = resolve(url, throttle=args.throttle)
        if direct:
            n += 1
            found.setdefault(str(it["atwiki_id"]), []).append(direct)
            print(f"  [{i}/{len(items)}] OK  {direct.split('/')[-1][:34]}", flush=True)
        else:
            print(f"  [{i}/{len(items)}] --  {url[:62]}", flush=True)
        time.sleep(args.throttle)

    print(f"\nresolved {n} of {len(items)}")
    if not args.apply:
        print("(dry run -- pass --apply to write into the game records)")
        return 0
    for gid, urls in found.items():
        p = GAMES / f"{gid}.json"
        if not p.exists():
            continue
        d = json.loads(p.read_text(encoding="utf-8"))
        have = {e.get("original_url") for e in (d.get("downloads") or [])}
        for u in urls:
            if u in have:
                continue
            d.setdefault("downloads", []).append({
                "original_url": u, "found_via": "vector",
                "filename": u.rsplit("/", 1)[-1], "source": "vector"})
        p.write_text(json.dumps(d, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"wrote into {len(found)} game record(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
