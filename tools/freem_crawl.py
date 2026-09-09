#!/usr/bin/env python3
"""Crawl freem.ne.jp for FM2K games.

Freem (フリーム！) is a Japanese freeware portal. Unlike atwiki — which
only documents games + outbound links — freem actually hosts the
files. Pulling from here gives us direct ZIP bytes for games that
atwiki may list as NEEDS_HUNT.

Stages:
  1. Listing crawl: walk one or more search/tag pages, collect every
     game_id (the `/win/game/<id>` path component).
  2. Detail crawl: for each game_id, fetch the detail page + extract
     title / author / engine_tag / download_url / version /
     screenshots / description / size_bytes / players.
  3. (Separate tool) Download crawl via Playwright — freem's
     /dl/win/<id> is a JS-walled interstitial; treat like Vector.

Output:
  data/freem/index.json          — flat list of {id, title, author}
  data/freem/games/<id>.json     — per-game record

Listing pages we probe by default (override with --search):
  - https://www.freem.ne.jp/search/2D格闘ツクール
  - https://www.freem.ne.jp/search/2D格闘ツクール2nd
  - https://www.freem.ne.jp/search/格闘ツクール   (catch-all)

Designed to be re-runnable: detail JSON is overwritten only when the
incoming version differs.
"""

from __future__ import annotations

import argparse
import json
import re
import time
from pathlib import Path
from typing import Optional
from urllib.parse import quote, urljoin
from urllib.request import Request, urlopen
from urllib.error import HTTPError, URLError


REPO = Path(__file__).resolve().parents[1]
OUT_DIR = REPO / "data" / "freem"
GAMES_DIR = OUT_DIR / "games"

UA = ("Mozilla/5.0 (compatible; fm2k-archive-crawler/1.0; "
      "+https://github.com/Armonte/wanwan)")

DEFAULT_SEARCHES = (
    # The two canonical engine tags freem uses. The first matches FM95
    # entries (older, plain "2D格闘ツクール"). The second is FM2K-
    # specific. Each card on the listing has a tag chip that resolves
    # back to one of these terms.
    #
    # Earlier versions of this script also probed the bare "格闘ツクール"
    # which pulled ~225 non-FM games into the index (RPG Maker fighters,
    # custom-engine games). That noise has been dropped — runs are
    # tight on the two engine tags only.
    "2D格闘ツクール",
    "2D格闘ツクール2nd",
)


def fetch(url: str, timeout: float = 15.0) -> str:
    req = Request(url, headers={"User-Agent": UA,
                                "Accept-Language": "ja,en;q=0.7"})
    with urlopen(req, timeout=timeout) as r:
        raw = r.read()
    # freem is utf-8; be defensive about declared charset
    return raw.decode("utf-8", errors="replace")


# ─── Listing parser ──────────────────────────────────────────────────────

# Each search result card is anchored by an /win/game/<id> link plus a
# game title in the same block. Simplest regex pass is enough — we
# don't need full DOM parsing for an ID extraction.
_GAME_ID_RE = re.compile(r"/win/game/(\d+)")
# Engine tag chip appears as <a class="...tag...">2D格闘ツクール2nd</a>
# inside each card; capture the surrounding card for context.


def listing_url(term: str, page: int) -> str:
    """URL for one listing page.

    TWO SHAPES, and they are not interchangeable:
      search    https://www.freem.ne.jp/search/<term>?page=N
      category  https://www.freem.ne.jp/win/category/<N>/page-M
    A category id is passed as "category:45". Category listings are the
    curated ones (45 = Fighting games, 75 works); search matches the term
    ANYWHERE on a page, so it pulls in noise -- an RPG Maker or YU-RIS title
    whose blurb merely mentions 2D格闘ツクール. Both are useful, for different
    reasons, so both are supported rather than one replacing the other.
    """
    if term.startswith("category:"):
        cid = term.split(":", 1)[1]
        base = f"https://www.freem.ne.jp/win/category/{cid}"
        return base if page <= 1 else f"{base}/page-{page}"
    return (f"https://www.freem.ne.jp/search/{quote(term)}"
            + (f"?page={page}" if page > 1 else ""))


_REVIEW_BLOCK_RE = re.compile(r'class="game-list-review".{0,4000}?</li>', re.S)
_DECLARED_TOTAL_RE = re.compile(r'([0-9,]+)\s*作品')


def _listing_ids(html: str) -> list[str]:
    """Game ids from the LISTING ONLY, excluding the recommendation block.

    MEASURED on /win/category/45 (2026-09-09): each page carries 40 game links
    but only 20 are the category listing -- the other 20 sit inside
    `class="game-list-review"`, a site-wide recommendation rail that has
    nothing to do with the category. Taking every /win/game/N link gave 245
    ids for a category that declares 75 works, and spot-checking those extras
    found an RPG Maker horror game and a YU-RIS novel. A work list built that
    way spends the download budget on the wrong games.
    """
    review: set[str] = set()
    for m in _REVIEW_BLOCK_RE.finditer(html):
        review |= set(_GAME_ID_RE.findall(m.group(0)))
    out = []
    for m in _GAME_ID_RE.finditer(html):
        gid = m.group(1)
        if gid not in review and gid not in out:
            out.append(gid)
    return out


def crawl_listing(search_term: str, max_pages: int = 30) -> list[str]:
    """Returns a list of unique game_ids found on the search page +
    all pagination follow-ups. freem paginates via ?page=N.

    Important caveat for FM-tag use: `/search/2D格闘ツクール` returns
    games matching the term ANYWHERE on their page (description / title
    / tags). For tag-anchor verification check the detail page after
    calling this — see crawl_detail() which sets engine=None when no
    structured tag anchor is present (filters out text-match noise
    like RPG Maker games that mention `2D格闘ツクール` in their blurb)."""
    seen: list[str] = []
    declared: int | None = None
    page = 1
    while True:
        url = listing_url(search_term, page)
        print(f"  [listing] {url}", flush=True)
        try:
            html = fetch(url)
        except (HTTPError, URLError, TimeoutError) as e:
            print(f"  [listing] fetch failed: {e}")
            break
        # Honour the declared work count. freem renders a DIFFERENT layout for
        # an out-of-range page rather than an empty one -- category 45 has 4
        # real pages, and page-5 came back with 82 links from some other block.
        # Without this the crawl walks straight past the end collecting noise.
        if declared is None:
            m_tot = _DECLARED_TOTAL_RE.search(html)
            if m_tot:
                declared = int(m_tot.group(1).replace(",", ""))
                print(f"  [listing] declares {declared} works", flush=True)
        page_ids = [g for g in _listing_ids(html) if g not in seen]
        if declared is not None and len(seen) >= declared:
            break
        if not page_ids:
            # No game refs on this page → past the end.
            break
        for gid in page_ids:
            if gid not in seen:
                seen.append(gid)
        # Pagination: stop when the page renders fewer than ~20 cards
        # (freem default page size) OR when we've already seen the
        # full set on page 1.
        if declared is not None and len(seen) >= declared:
            break
        if len(page_ids) < 5:
            break
        page += 1
        time.sleep(0.4)
        # Safety cap: freem has ~530 entries on the FM tags, ~25
        # per page, so 30 pages is generous. Stop condition above
        # (len(page_ids) < 5) catches a clean tail; this is just a
        # belt for the case where freem renders 5+ phantom entries.
        if page > max_pages:
            print(f"  [listing] hit the {max_pages}-page cap -- recall is "
                  f"TRUNCATED here, not exhausted. Raise --max-pages.")
            break
    return seen


# ─── Detail parser ───────────────────────────────────────────────────────

_TITLE_RE   = re.compile(r'<meta property="og:title" content="([^"]+)"')
_DESC_RE    = re.compile(r'<meta name="description" content="([^"]+)"')
_AUTHOR_RE  = re.compile(r'/brand/(\d+)[^"]*"[^>]*>([^<]+)</a>')
# freem renders user-set tags as <a href="/search/<encoded>">#<name></a>.
# Detect FM tags via the structured anchor — NOT a body text match,
# because the search URL `/search/2D格闘ツクール` returns games that
# merely *mention* the term in their description (RPG Maker games that
# say "2D格闘ツクールのような" etc). The anchor is the only reliable
# signal that the author explicitly classified the game as FM.
#
# The URL-encoded form of "2D格闘ツクール" is:
#   2D%E6%A0%BC%E9%97%98%E3%83%84%E3%82%AF%E3%83%BC%E3%83%AB
# We accept both raw and encoded forms in case freem ever serves
# pre-decoded hrefs.
_FM_TAG_HREF = (
    r'/search/(?:2D%E6%A0%BC%E9%97%98%E3%83%84%E3%82%AF%E3%83%BC%E3%83%AB'
    r'|2D格闘ツクール)')
_TAG_ANCHOR_RE      = re.compile(r'href="' + _FM_TAG_HREF + r'"')
_TAG_2ND_ANCHOR_RE  = re.compile(r'href="' + _FM_TAG_HREF + r'2nd"')
_SIZE_RE    = re.compile(r'([\d,]+)\s*K?B?[Yy][Tt][Ee]')   # "330,708 KByte"
# Version label — accept "ver1.08", "Ver. 1.0", "v0.5", "1.2.3" with
# leading whitespace + bracket. Older regex caught "ver." alone with
# no digits, which is useless.
_VER_RE     = re.compile(
    r'(?:[Vv]er\.?\s*|[Vv])(\d+(?:\.\d+){0,3})\b')
_UPDATED_RE = re.compile(r'(\d{4}[-/]\d{1,2}[-/]\d{1,2})')
_SCREEN_RE  = re.compile(r'(https?://fpiccdn\.com/\d+s?/[^"\'\s]+)')
_DLPAGE_RE  = re.compile(r'/dl/win/(\d+)')
_PLAYERS_RE = re.compile(r'1\s*対\s*1|2人プレイ|対戦')


def parse_detail(html: str, gid: str) -> dict:
    """Extract structured fields from a freem game detail page."""
    title_m = _TITLE_RE.search(html)
    # Strip the freem-canonical title suffix that's on every page:
    #   "<game>：無料ゲーム配信中！ [ふりーむ！]"
    # Editors only want the bare game name.
    title_str = (title_m.group(1) if title_m else "").strip()
    title_str = re.sub(r"\s*[:：]\s*無料ゲーム配信中.*$", "", title_str)
    desc_m  = _DESC_RE.search(html)
    author_m = _AUTHOR_RE.search(html)
    size_m  = _SIZE_RE.search(html)
    ver_m   = _VER_RE.search(html)
    upd_m   = _UPDATED_RE.search(html)
    screens = list(dict.fromkeys(_SCREEN_RE.findall(html)))[:4]
    dlpage_m = _DLPAGE_RE.search(html)
    is_versus = bool(_PLAYERS_RE.search(html))

    # Engine sub-tag — detect via the structured tag anchor only.
    # The 2nd-specific anchor is more specific so check it first.
    engine_tag = None
    if _TAG_2ND_ANCHOR_RE.search(html):
        engine_tag = "2D格闘ツクール2nd"   # FM2K
    elif _TAG_ANCHOR_RE.search(html):
        engine_tag = "2D格闘ツクール"      # FM95 ("2D格闘ツクール")

    # freem reports size in KByte (or just bytes); normalize to bytes.
    size_bytes = None
    if size_m:
        try:
            n = int(size_m.group(1).replace(",", ""))
            # Heuristic: if the matched string contained "K" treat as
            # kibibytes (1024), else bytes. KByte on freem is
            # typically displayed for >100 KiB files.
            if "K" in size_m.group(0).upper():
                size_bytes = n * 1024
            else:
                size_bytes = n
        except ValueError:
            pass

    return {
        "freem_id":     gid,
        "title":        title_str,
        "author":       (author_m.group(2).strip()
                         if author_m else ""),
        "author_id":    (author_m.group(1) if author_m else ""),
        "engine_tag":   engine_tag,
        "engine":       ("FM2K" if engine_tag and "2nd" in engine_tag
                         else ("FM95" if engine_tag else None)),
        "size_bytes":   size_bytes,
        # Normalize captured version digits to "v1.08" form. ver_m.group(1)
        # is just the digits without the "v"/"ver" prefix.
        "version":      ("v" + ver_m.group(1)) if ver_m else "",
        "updated":      (upd_m.group(0) if upd_m else ""),
        "screenshots":  screens,
        "download_url": (f"https://www.freem.ne.jp/dl/win/{dlpage_m.group(1)}"
                         if dlpage_m else
                         f"https://www.freem.ne.jp/dl/win/{gid}"),
        "detail_url":   f"https://www.freem.ne.jp/win/game/{gid}",
        "description":  (desc_m.group(1).strip()
                         if desc_m else "")[:500],
        "is_versus":    is_versus,
    }


def crawl_detail(gid: str, throttle: float = 0.4,
                 force: bool = False) -> Optional[dict]:
    out_path = GAMES_DIR / f"{gid}.json"
    if out_path.exists() and not force:
        try:
            return json.loads(out_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            pass
    url = f"https://www.freem.ne.jp/win/game/{gid}"
    try:
        html = fetch(url)
    except (HTTPError, URLError, TimeoutError) as e:
        print(f"  [detail {gid}] fetch failed: {e}")
        return None
    rec = parse_detail(html, gid)
    GAMES_DIR.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(rec, ensure_ascii=False, indent=2),
                        encoding="utf-8")
    time.sleep(throttle)
    return rec


# ─── Main ────────────────────────────────────────────────────────────────

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--search", action="append",
                    help=("freem search term to crawl. Can be given "
                          "multiple times. Defaults to the FM2K/FM95 tag "
                          "set."))
    ap.add_argument("--force", action="store_true",
                    help="re-fetch detail pages even if already cached")
    ap.add_argument("--category", action="append", type=int, default=None,
                    help=("freem category id to crawl, e.g. 45 (Fighting "
                          "games). Repeatable. Category listings are curated; "
                          "--search matches the term anywhere on a page and so "
                          "pulls in unrelated engines."))
    ap.add_argument("--max-pages", type=int, default=30,
                    help="listing pagination cap (default 30)")
    ap.add_argument("--throttle", type=float, default=0.4)
    args = ap.parse_args()

    searches = list(args.search or ([] if args.category else DEFAULT_SEARCHES))
    searches += [f"category:{c}" for c in (args.category or [])]
    print(f"crawling {len(searches)} search term(s): {searches}", flush=True)

    # Crawl each listing separately so we know WHICH list each gid
    # belongs to. Membership in the "2nd" listing is freem's
    # CMS-level signal that the game is FM2K (the author may have
    # only tagged it bare "2D格闘ツクール" on the detail page, but
    # freem's listing-side engine field is the source of truth).
    by_search: dict[str, set[str]] = {}
    for term in searches:
        ids = crawl_listing(term, max_pages=args.max_pages)
        by_search[term] = set(ids)
        print(f"  [{term}] {len(ids)} ids")

    all_ids: list[str] = []
    for ids in by_search.values():
        for gid in ids:
            if gid not in all_ids:
                all_ids.append(gid)

    listing_2nd = by_search.get("2D格闘ツクール2nd", set())
    listing_any = by_search.get("2D格闘ツクール", set()) | listing_2nd

    print(f"\ntotal unique freem_ids: {len(all_ids)} "
          f"(in 2nd listing: {len(listing_2nd)})\n", flush=True)

    records: list[dict] = []
    for i, gid in enumerate(all_ids, 1):
        rec = crawl_detail(gid, throttle=args.throttle, force=args.force)
        if not rec:
            continue
        # Engine classification — ONLY the detail-page tag anchor is
        # trustworthy. Both /search/2D格闘ツクール and the more specific
        # /search/2D格闘ツクール2nd return any game whose description
        # text matches, which dumps RPGs / VNs / unrelated games into
        # the result set when authors mention the engine in their blurb.
        #
        #   detail has /search/...2nd href  → FM2K (author tagged 2nd)
        #   detail has bare tag href        → FM95 (author tagged bare)
        #   neither                         → not FM (text-match noise)
        #
        # The listing_2nd membership is recorded as a soft hint
        # (in_2nd_listing) but does not override the tag-anchor
        # classification. If we ever want to upgrade a bare-tag entry
        # to FM2K based on listing membership, we'd want a third
        # corroborating signal (screenshot OCR, file metadata, etc.).
        rec["in_2nd_listing"] = gid in listing_2nd
        rec["in_bare_listing"] = gid in listing_any and gid not in listing_2nd
        # rec["engine"] / rec["engine_tag"] already set by parse_detail
        # — leave it. crawl_detail wrote the tag-anchor-based values.

        # Persist the soft-hint flags back to disk (engine wasn't
        # changed but the listing flags are new).
        (GAMES_DIR / f"{gid}.json").write_text(
            json.dumps(rec, ensure_ascii=False, indent=2),
            encoding="utf-8")
        records.append(rec)
        ver_str = (f" {rec['version']}" if rec.get('version') else '')
        eng_disp = (rec['engine'] or '—')
        print(f"  [{i:3d}/{len(all_ids)}] {gid:>6} "
              f"{eng_disp:>4}{ver_str:<10s} "
              f"by {rec['author']:<20s} {rec['title'][:50]}",
              flush=True)

    # Write a flat index for downstream tools.
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    index = {
        "version":    1,
        "ts":         int(time.time()),
        "searches":   searches,
        "n_games":    len(records),
        "games":      [{
            "freem_id":  r["freem_id"],
            "title":     r["title"],
            "author":    r["author"],
            "engine":    r["engine"],
            "version":   r["version"],
            "size_bytes": r["size_bytes"],
        } for r in records],
    }
    (OUT_DIR / "index.json").write_text(
        json.dumps(index, ensure_ascii=False, indent=2),
        encoding="utf-8")
    print(f"\nwrote {OUT_DIR / 'index.json'} ({len(records)} games)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
