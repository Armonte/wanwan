#!/usr/bin/env python3
"""Phase 2b: parse cached atwiki HTML into structured per-game JSON.

Input:  data/fm2k_wiki/cache/<id>.html  (from atwiki_crawl_pages.py)
Output: data/fm2k_wiki/games/<id>.json

Per-game schema:
{
  "atwiki_id":   "297",
  "engine":      "FM95" | "FM2K",
  "title":       "Angolmoa　～In Your Sight～",     # from index
  "title_page":  "Angolmoa　～In Your Sight～",     # from <h1> on the page itself
  "body_text":   "...",                              # plain-text body
  "info_fields": { "ジャンル": "...", "作者": "...", ... },
  "characters":  [ "...", ... ],                     # heuristic char-list extract
  "screenshots": [ "https://...", ... ],
  "outbound":    [ {"url": "...", "label": "..."}, ... ]
}

Heuristics are best-effort — atwiki pages are free-form, not all fields
are present. Missing fields stay empty/null rather than failing.
"""

import argparse
import html as html_mod
import json
import re
import sys
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
INDEX = REPO / "data" / "fm2k_wiki" / "index.json"
CACHE_DIR = REPO / "data" / "fm2k_wiki" / "cache"
GAMES_DIR = REPO / "data" / "fm2k_wiki" / "games"


def slice_content(html: str) -> str:
    start = html.find('<div id="contents"')
    end = html.find('<div id="atwiki-page-tags"', start if start != -1 else 0)
    if start == -1 or end == -1:
        return html
    return html[start:end]


def strip_tags(html: str) -> str:
    no_script = re.sub(r"<script.*?</script>", "", html, flags=re.S)
    no_style = re.sub(r"<style.*?</style>", "", no_script, flags=re.S)
    plain = re.sub(r"<[^>]+>", " ", no_style)
    plain = html_mod.unescape(plain)
    plain = re.sub(r"\s+", " ", plain)
    return plain.strip()


def extract_title(html: str) -> str | None:
    # Page title appears as <h2 class="atwiki-page-title">…</h2>
    m = re.search(r'<h2[^>]+atwiki-page-title[^>]*>(.*?)</h2>', html, re.S)
    if m:
        return strip_tags(m.group(1))
    return None


def extract_outbound(content: str) -> list[dict]:
    """All non-atwiki, non-archive http(s) links in the article body."""
    out: list[dict] = []
    seen: set[str] = set()
    for m in re.finditer(
        r'<a[^>]+href="(?P<u>https?://[^"]+)"[^>]*>(?P<t>.*?)</a>',
        content, re.S,
    ):
        url = m.group("u")
        # Skip atwiki internal nav, archive crawler artifacts, image-CDN
        # auxiliary URLs (cdn1/cdn2 host the actual screenshots — those
        # we capture via extract_screenshots instead).
        if "atwiki.jp" in url:
            continue
        if "web.archive.org" in url:
            continue
        if "googletagmanager" in url or "google-analytics" in url:
            continue
        if url in seen:
            continue
        seen.add(url)
        out.append({"url": url, "label": strip_tags(m.group("t"))[:120]})
    return out


def extract_screenshots(content: str) -> list[str]:
    """User-uploaded images, typically hosted on atwiki's cdn{1,2}.atwikiimg
    domain. Skip layout sprites / ads / wiki UI icons."""
    out: list[str] = []
    seen: set[str] = set()
    for m in re.finditer(r'<img[^>]+src="(?P<u>https?://[^"]+)"', content):
        url = m.group("u")
        if not ("atwikiimg.com" in url or "atwiki.jp" in url):
            continue
        # CDN layout assets / icons live under /skin/, /img/, /image/icon/.
        if any(seg in url for seg in ("/skin/", "/css/", "/icon/")):
            continue
        if url in seen:
            continue
        seen.add(url)
        out.append(url)
    return out


def extract_info_fields(content: str) -> dict[str, str]:
    """atwiki templates render game info as a 2-column table:
       <th>field</th><td>value</td>. Slurp all such pairs."""
    out: dict[str, str] = {}
    for m in re.finditer(
        r'<th[^>]*>(?P<k>.*?)</th>\s*<td[^>]*>(?P<v>.*?)</td>',
        content, re.S,
    ):
        k = strip_tags(m.group("k"))
        v = strip_tags(m.group("v"))
        if not k or not v:
            continue
        out[k[:64]] = v[:512]
    return out


def parse_one(atwiki_id: str, engine: str, idx_title: str,
              cache_path: Path) -> dict | None:
    if not cache_path.exists():
        return None
    html = cache_path.read_text(encoding="utf-8")
    content = slice_content(html)
    return {
        "atwiki_id":   atwiki_id,
        "engine":      engine,
        "title":       idx_title,
        "title_page":  extract_title(html),
        "body_text":   strip_tags(content)[:4000],
        "info_fields": extract_info_fields(content),
        "screenshots": extract_screenshots(content),
        "outbound":    extract_outbound(content),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--limit", type=int, default=0)
    args = ap.parse_args()

    GAMES_DIR.mkdir(parents=True, exist_ok=True)
    with INDEX.open(encoding="utf-8") as f:
        games = json.load(f)["games"]

    n_ok = n_skip = 0
    n_outbound_total = 0
    for g in games[: args.limit] if args.limit else games:
        cache_path = CACHE_DIR / f"{g['atwiki_id']}.html"
        record = parse_one(g["atwiki_id"], g["engine"], g["title"], cache_path)
        if record is None:
            n_skip += 1
            continue
        out_path = GAMES_DIR / f"{g['atwiki_id']}.json"
        # MERGE, NEVER CLOBBER. This phase rebuilds a record from the cached
        # HTML, but LATER phases enrich the same file: resolve_archives adds
        # outbound[].archive, fetch_downloads adds downloads[]. A plain
        # overwrite silently discards both.
        #
        # Measured 2026-09-09, the hard way: re-running this phase to recover 40
        # unparsed pages reset all 336 records and took KNOWN_RECOVERABLE from
        # 157 to 0, which then made the download-discovery pass report "0
        # download URLs" across the whole catalogue. Everything had to be
        # re-resolved against Wayback. Re-parsing must be free.
        if out_path.exists():
            try:
                prev = json.loads(out_path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                prev = {}
            if prev.get("downloads"):
                record["downloads"] = prev["downloads"]
            prev_arch = {o.get("url"): o.get("archive")
                         for o in (prev.get("outbound") or []) if o.get("archive")}
            for o in record.get("outbound") or []:
                if o.get("url") in prev_arch:
                    o["archive"] = prev_arch[o["url"]]
        out_path.write_text(json.dumps(record, ensure_ascii=False, indent=2),
                            encoding="utf-8")
        n_ok += 1
        n_outbound_total += len(record["outbound"])

    print(f"parsed {n_ok} games ({n_skip} skipped — no cache yet)")
    print(f"  total outbound links: {n_outbound_total}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
