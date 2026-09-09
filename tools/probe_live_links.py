#!/usr/bin/env python3
"""Probe the atwiki outbound links that Wayback could not resolve.

WHY: 302 of 515 outbound links have no usable Wayback snapshot, but a large
share of them point at hosts that are STILL LIVE (vector.co.jp alone is 59).
Those need no archive at all. This separates "dead, archive-only" from "just
fetch it".

A host-root status code is not the test -- geocities.jp answers 301 and lands
on a service-closed notice. So this follows redirects and judges the FINAL
response, flagging the closure/parking pages that masquerade as success.

Read-only. Writes data/fm2k_wiki/live_probe.json.
"""
from __future__ import annotations

import argparse
import json
import time
import urllib.parse
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

REPO = Path(__file__).resolve().parents[1]
GAMES = REPO / "data" / "fm2k_wiki" / "games"
OUT = REPO / "data" / "fm2k_wiki" / "live_probe.json"
UA = "Mozilla/5.0 (compatible; fm2k-archive-crawler/1.0)"

# Landing pages that return 200 but mean "this content is gone". Matched
# against the FINAL url after redirects.
DEAD_MARKERS = [
    "geocities.co.jp/info/", "geocities.jp/close", "/close.html",
    "info.geocities.jp", "service_end", "serviceend", "closed",
    "infoseek.co.jp/error", "freett.com/error", "/notfound",
    "tok2.com/error", "nifty.com/end",
]
DEAD_TEXT = ["サービスは終了", "サービス終了", "は終了しました",
             "ページが見つかりません", "Not Found", "存在しません"]


def probe(url: str, timeout: float = 15.0) -> dict:
    req = Request(url, headers={"User-Agent": UA})
    try:
        with urlopen(req, timeout=timeout) as r:
            final = r.geturl()
            body = r.read(65536)
            status = r.status
    except HTTPError as e:
        return {"url": url, "status": e.code, "verdict": "http-error"}
    except (URLError, TimeoutError, OSError, ValueError) as e:
        return {"url": url, "status": 0, "verdict": "unreachable",
                "error": type(e).__name__}
    low_final = final.lower()
    text = body.decode("utf-8", errors="replace")
    verdict = "live"
    if any(m in low_final for m in DEAD_MARKERS):
        verdict = "dead-redirect"
    elif any(t in text for t in DEAD_TEXT):
        verdict = "dead-page"
    elif urllib.parse.urlparse(final).netloc != urllib.parse.urlparse(url).netloc:
        verdict = "redirected-offsite"
    return {"url": url, "status": status, "final": final,
            "verdict": verdict, "bytes": len(body)}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--throttle", type=float, default=0.8)
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    links = []
    for f in sorted(GAMES.glob("*.json")):
        d = json.loads(f.read_text(encoding="utf-8"))
        for o in (d.get("outbound") or []):
            if not (o.get("archive") or {}).get("wayback_url"):
                links.append({"atwiki_id": d["atwiki_id"],
                              "title": d.get("title", ""), "url": o["url"]})
    if args.limit:
        links = links[:args.limit]
    print(f"probing {len(links)} unresolved links")

    seen_host_last = {}
    out = []
    for i, L in enumerate(links, 1):
        host = urllib.parse.urlparse(L["url"]).netloc
        # per-host politeness on top of the global throttle
        last = seen_host_last.get(host, 0.0)
        wait = 1.5 - (time.time() - last)
        if wait > 0:
            time.sleep(wait)
        r = probe(L["url"])
        seen_host_last[host] = time.time()
        r.update(atwiki_id=L["atwiki_id"], title=L["title"])
        out.append(r)
        print(f"  [{i}/{len(links)}] {r['verdict']:<18} {L['url'][:70]}",
              flush=True)
        time.sleep(args.throttle)

    OUT.write_text(json.dumps(out, ensure_ascii=False, indent=2),
                   encoding="utf-8")
    import collections
    c = collections.Counter(r["verdict"] for r in out)
    print("\nverdicts:", c.most_common())
    print(f"-> {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
