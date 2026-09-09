#!/usr/bin/env python3
"""Phase 4: walk Wayback snapshots of game homepages, extract download
URLs, and (optionally) pull the archives down.

For each per-game record produced by atwiki_parse_pages and resolved
by atwiki_resolve_archives, this script:

  1. Fetches each archived outbound page (game homepage on Wayback).
  2. Scans the snapshot HTML for download-archive links — *.zip,
     *.lzh, *.rar, *.7z, *.tar.gz — both relative and absolute.
  3. Re-resolves each archive URL against Wayback CDX (the homepage
     snapshot may not co-host the archive bytes).
  4. Records the candidate download URLs back into the game JSON
     under "downloads".
  5. With --pull, actually downloads the archives to
     data/fm2k_wiki/downloads/<atwiki_id>/<basename>, computes
     sha256 + size, and stores those alongside the URL.

Conservative defaults: parsing only (no big downloads). Pass --pull
to fetch the archives. Each download is capped at --max-mb (default
500 MB) to keep accidental huge files from blowing up disk.
"""

import argparse
import hashlib
import json
import re
import sys
import time
from pathlib import Path
from urllib.parse import quote, urljoin, urlparse
from urllib.request import Request, urlopen
from urllib.error import URLError, HTTPError


REPO = Path(__file__).resolve().parents[1]
GAMES_DIR = REPO / "data" / "fm2k_wiki" / "games"
DL_DIR    = REPO / "data" / "fm2k_wiki" / "downloads"

CDX_BASE = "https://web.archive.org/cdx/search/cdx"
USER_AGENT = ("Mozilla/5.0 (compatible; fm2k-archive-crawler/1.0; "
              "+https://github.com/Armonte/wanwan)")
PRE_SHUTDOWN_CUTOFF = "20181231"

# File extensions we treat as "this is a game download". Doujin
# authors of the Geocities/freett era shipped a mix:
#   - .lzh / .zip  — Lhasa-style or modern zip archives
#   - .rar / .7z   — appeared later (post-2005)
#   - .exe         — bare self-extractor OR the actual game binary,
#                    common when the .kgt + assets fit small enough
#                    to ship a single file (red eclipse / EORR family)
ARCHIVE_EXTS = (".zip", ".lzh", ".rar", ".7z", ".tar.gz", ".tgz", ".exe")
ARCHIVE_RE = re.compile(
    r'href="(?P<u>[^"]+\.(?:zip|lzh|rar|7z|tar\.gz|tgz|exe)(?:\?[^"]*)?)"',
    re.IGNORECASE,
)

# Old Japanese doujin homepages frequently use <frameset> with the
# real content (menu + game info + downloads) loaded into sub-frames.
# Without following these our scanner only sees the empty wrapper page.
FRAME_RE = re.compile(
    r'<(?:i?frame)\s[^>]*src="(?P<u>[^"]+)"',
    re.IGNORECASE,
)
# Generic href->*.html / *.htm matcher. We pull every same-host
# sub-page candidate at depth 2, then let the bounded recursion
# (max_depth) keep exploration finite. A label-restricted regex
# missed too many cases (e.g. WonderfulWorld's nav link "GAME"
# doesn't contain any of "download/ダウンロード/DL/データ", but
# htm/game.html is exactly where the .zip lives).
SUBPAGE_HINT_RE = re.compile(
    r'href="(?P<u>(?!https?:)[^"]+\.html?)(?:[\#\?][^"]*)?"',
    re.IGNORECASE,
)


def fetch_url(url: str, timeout: float = 30.0,
              max_bytes: int | None = None) -> bytes | None:
    req = Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urlopen(req, timeout=timeout) as r:
            if max_bytes is not None:
                return r.read(max_bytes + 1)
            return r.read()
    except (URLError, HTTPError, TimeoutError):
        return None


def cdx_pick_snapshot(url: str, timeout: float = 15.0) -> dict | None:
    """Latest pre-shutdown 200, else latest 200 of any era. Mirror of
    the policy in atwiki_resolve_archives.py — duplicated here to
    keep this script standalone."""
    q = (f"{CDX_BASE}?url={quote(url, safe=':/?&=')}"
         f"&output=json&filter=statuscode:200&limit=500")
    raw = fetch_url(q, timeout=timeout)
    if not raw:
        return None
    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        return None
    if not data or len(data) < 2:
        return None
    snaps = sorted((row[1], row[2]) for row in data[1:] if len(row) >= 3)
    if not snaps:
        return None
    pre = [s for s in snaps if s[0] <= PRE_SHUTDOWN_CUTOFF + "999999"]
    ts, original = (pre[-1] if pre else snaps[-1])
    return {
        "wayback_url": f"https://web.archive.org/web/{ts}id_/{original}",
        "timestamp":   ts,
        "preferred":   "wayback_pre2019" if pre else "wayback_latest",
    }


def extract_archive_links(html: str, base_url: str) -> list[str]:
    """Pull every archive-extension href out of homepage HTML and
    resolve relatives against the homepage URL. Dedupe but preserve
    discovery order."""
    out: list[str] = []
    seen: set[str] = set()
    for m in ARCHIVE_RE.finditer(html):
        href = m.group("u")
        # Strip Wayback's URL-rewriting prefix if present so we get the
        # original target.
        m2 = re.match(r'^/web/\d+(?:[a-z_]+)?/(.+)$', href)
        if m2:
            href = m2.group(1)
            if not href.startswith("http"):
                href = "http://" + href.split("/", 1)[0] + "/" + href.split("/", 1)[1] \
                    if "/" in href else "http://" + href
        full = urljoin(base_url, href)
        if full in seen:
            continue
        seen.add(full)
        out.append(full)
    return out


def same_timestamp_url(homepage_snap_url: str, target_url: str) -> str | None:
    """Construct a Wayback URL for `target_url` using the same timestamp
    the homepage snapshot was captured at. Wayback often crawls linked
    resources in the same session, so this catches archives that don't
    have their own CDX record but were grabbed alongside the homepage
    (the eorr070717.exe / 20070818013942 case)."""
    m = re.search(r'/web/(\d{8,14})(?:[a-z_]+)?/', homepage_snap_url)
    if not m:
        return None
    ts = m.group(1)
    return f"https://web.archive.org/web/{ts}id_/{target_url}"


# Snapshot fetches that FAILED, as opposed to pages that genuinely had no
# download links. Without this the two are indistinguishable: the crawl below
# does `if not html: return []`, so a refused fetch reports "no downloads here"
# and the run's summary line looks like a real (empty) result.
#
# Measured 2026-09-09: a full 336-game discovery pass ran while Wayback was
# rate-limiting us and reported "22 download URLs" with no errors at all. 155
# records had a snapshot to try. That number was meaningless and there was no
# way to tell from the output.
FETCH_FAILURES: list[str] = []


def fetch_wayback_html(snap_url: str) -> str | None:
    if "id_" not in snap_url:
        snap_url = re.sub(r'/web/(\d+)/', r'/web/\1id_/', snap_url)
    body = fetch_url(snap_url)
    if not body:
        FETCH_FAILURES.append(snap_url)
        return None
    return body.decode("utf-8", errors="replace")


def crawl_homepage_for_archives(snap_url: str, original_base: str,
                                throttle: float = 0.3,
                                max_depth: int = 2,
                                visited: set | None = None) -> list[str]:
    """Recursively scan a Wayback-archived homepage for download links.

    Follows two kinds of internal navigation:
      - <frame> / <iframe> sub-frames (very common on doujin sites
        of the FrontPage/Homepage Builder era — the wrapper page is
        empty and the real content sits in menu.html + main.html).
      - <a href="*.html"> with a label suggesting "download" /
        ダウンロード / DL / データ — a few homepages put the dl link
        one click deeper rather than on the front page.

    Capped at `max_depth` to avoid following every link on the site.
    Same Wayback timestamp is reused across all sub-fetches so we
    stay within one capture session (consistent content view).
    """
    if visited is None:
        visited = set()
    if max_depth <= 0:
        return []
    if snap_url in visited:
        return []
    visited.add(snap_url)

    html = fetch_wayback_html(snap_url)
    if not html:
        return []

    # Direct download hits at this level.
    archives = list(extract_archive_links(html, original_base))

    # Find sub-pages to recurse into.
    next_targets: list[tuple[str, str]] = []  # (snap_url, original_base)
    ts_match = re.search(r'/web/(\d{8,14})', snap_url)
    ts = ts_match.group(1) if ts_match else None

    def _wayback_for(target_original: str) -> str | None:
        # Construct a same-timestamp Wayback URL. If ts unknown, give
        # up on this branch.
        if not ts:
            return None
        return f"https://web.archive.org/web/{ts}id_/{target_original}"

    for m in FRAME_RE.finditer(html):
        frame_src = m.group("u")
        # Skip the Wayback toolbar / fc2 footer scripts that have
        # frame-shaped src attributes we don't care about.
        if "web.archive.org" in frame_src or "fc2.com/apis/" in frame_src:
            continue
        full_orig = urljoin(original_base, frame_src)
        wb = _wayback_for(full_orig)
        if wb:
            next_targets.append((wb, full_orig))

    for m in SUBPAGE_HINT_RE.finditer(html):
        page = m.group("u")
        full_orig = urljoin(original_base, page)
        wb = _wayback_for(full_orig)
        if wb:
            next_targets.append((wb, full_orig))

    for wb, orig in next_targets:
        time.sleep(throttle)
        archives.extend(
            crawl_homepage_for_archives(wb, orig, throttle=throttle,
                                        max_depth=max_depth - 1,
                                        visited=visited)
        )
    return archives


def gather_downloads_for_game(record: dict, throttle: float = 0.3) -> int:
    """For each archived outbound link in the game record, fetch the
    snapshot (recursing into framesets / nav sub-pages), extract
    archive links, and resolve each via Wayback. Returns count of new
    download candidates added."""
    if "downloads" not in record:
        record["downloads"] = []
    seen_urls = {d["original_url"] for d in record["downloads"]}
    n_added = 0

    for link in record.get("outbound", []):
        archive = link.get("archive") or {}
        snap = archive.get("wayback_url")
        if not snap:
            continue
        original_url = link["url"]
        # depth=3: homepage -> frame contents (depth 2) -> any "downloads"
        # subpage they link (depth 1) -> archive hits at the leaf. Old
        # FrontPage-built homepages routinely bury the game download
        # this deep.
        archive_urls = crawl_homepage_for_archives(
            snap, original_url, throttle=throttle, max_depth=3)

        for arc_url in archive_urls:
            if arc_url in seen_urls:
                continue
            seen_urls.add(arc_url)
            entry = {
                "original_url": arc_url,
                "found_via":    snap,
                "filename":     Path(urlparse(arc_url).path).name,
            }
            # Try CDX first (when it's up — gives us the canonical
            # "best" snapshot policy). Otherwise fall back to the
            # same-timestamp Wayback URL — Wayback usually captures
            # linked resources in the same crawl session.
            picked = cdx_pick_snapshot(arc_url)
            if picked:
                entry["archive"] = picked
            else:
                same = same_timestamp_url(snap, arc_url)
                if same:
                    entry["archive"] = {
                        "wayback_url": same,
                        "timestamp":   re.search(r'/web/(\d+)', snap).group(1),
                        "preferred":   "wayback_same_session",
                    }
            record["downloads"].append(entry)
            n_added += 1
        time.sleep(throttle)
    return n_added


def detect_archive_kind(data: bytes) -> str:
    """Return a short label for the file's true type based on magic
    bytes. Used to gate downloads — we routinely receive HTML "click
    to download" landing pages from getuploader / mediafire / etc.
    that masquerade as the requested archive filename. Without this
    check we'd happily save a 25 KB HTML page as `Game.zip` and let
    crossref try to extract an exe from it."""
    if len(data) < 4:
        return "empty"
    # Container archives
    if data[:4] in (b"PK\x03\x04", b"PK\x05\x06", b"PK\x07\x08"):
        return "zip"
    if len(data) >= 7 and data[2:6] == b"-lh" or (len(data) >= 7 and data[2:5] == b"-lh"):
        return "lzh"
    if data[:7] == b"Rar!\x1a\x07\x00" or data[:8] == b"Rar!\x1a\x07\x01\x00":
        return "rar"
    if data[:6] == b"7z\xbc\xaf\x27\x1c":
        return "7z"
    if data[:3] == b"\x1f\x8b\x08":
        return "gzip"
    # PE executable (game might ship a single bare .exe)
    if data[:2] == b"MZ":
        return "exe"
    # Common "I'm an HTML wrapper, not the file you wanted" signatures
    head = data[:512].lstrip()
    low = head[:64].lower()
    if low.startswith(b"<!doctype") or low.startswith(b"<html") \
            or low.startswith(b"<head") or b"<body" in head[:300].lower():
        return "html"
    return "unknown"


def pull_archive(entry: dict, dl_root: Path, max_bytes: int) -> bool:
    """Fetch the archive bytes and stamp the entry with size+sha256.
    Tries the live URL first, falls back to Wayback. Returns True on
    successful download.

    Validates the response with magic-byte sniffing — we refuse to
    save anything that turns out to be an HTML landing page (the
    common JS-walled host pattern: getuploader / mediafire / etc.).
    Such results are recorded with `kind=html` so the caller can show
    "needs Playwright" rather than pretending we have the file."""
    if "size_bytes" in entry and "sha256" in entry:
        return True   # already pulled
    targets = [entry["original_url"]]
    arc = entry.get("archive") or {}
    if arc.get("wayback_url"):
        # Use the raw-content variant (id_) so we don't pull HTML
        # wrappers around the archive.
        wb = arc["wayback_url"]
        if "id_" not in wb:
            wb = re.sub(r'/web/(\d+)/', r'/web/\1id_/', wb)
        targets.append(wb)

    # Track what each attempt actually returned so the operator can
    # tell "no fetch ever succeeded" from "every host gave us HTML".
    attempts: list[dict] = []

    for url in targets:
        data = fetch_url(url, timeout=120.0, max_bytes=max_bytes)
        if not data:
            attempts.append({"url": url, "result": "no_response"})
            continue
        if max_bytes and len(data) > max_bytes:
            attempts.append({"url": url, "result": "oversize",
                             "size": len(data)})
            continue
        kind = detect_archive_kind(data)
        if kind in {"zip", "lzh", "rar", "7z", "gzip", "exe"}:
            sha = hashlib.sha256(data).hexdigest()
            out = dl_root / entry["filename"]
            out.write_bytes(data)
            entry["size_bytes"]   = len(data)
            entry["sha256"]       = sha
            entry["kind"]         = kind
            entry["pulled_from"]  = url
            entry["local_path"]   = str(out.relative_to(REPO))
            return True
        # html / unknown / empty — log and try the next target.
        attempts.append({"url": url, "result": kind, "size": len(data)})

    entry["error"] = "no archive bytes recovered"
    entry["attempts"] = attempts
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--ids", default="",
                    help="comma-separated atwiki IDs (overrides --limit)")
    ap.add_argument("--pull", action="store_true",
                    help="actually download the archives (default: just record URLs)")
    ap.add_argument("--max-mb", type=int, default=500,
                    help="per-archive size cap in MB (default 500)")
    ap.add_argument("--throttle", type=float, default=0.3)
    args = ap.parse_args()

    if args.ids:
        wanted = {s.strip() for s in args.ids.split(",") if s.strip()}
        paths = [GAMES_DIR / f"{i}.json" for i in wanted
                 if (GAMES_DIR / f"{i}.json").exists()]
    else:
        paths = sorted(GAMES_DIR.glob("*.json"))
        if args.limit:
            paths = paths[: args.limit]
    max_bytes = args.max_mb * 1024 * 1024

    n_games = 0
    n_url = n_pulled = n_failed = 0
    for i, p in enumerate(paths, 1):
        record = json.loads(p.read_text(encoding="utf-8"))
        added = gather_downloads_for_game(record, throttle=args.throttle)
        n_url += added
        if args.pull and record.get("downloads"):
            game_dl = DL_DIR / record["atwiki_id"]
            game_dl.mkdir(parents=True, exist_ok=True)
            for entry in record["downloads"]:
                if "size_bytes" in entry:
                    continue
                ok = pull_archive(entry, game_dl, max_bytes)
                if ok:
                    n_pulled += 1
                else:
                    n_failed += 1
                time.sleep(args.throttle)
        p.write_text(json.dumps(record, ensure_ascii=False, indent=2),
                     encoding="utf-8")
        n_games += 1
        print(f"[{i:3d}/{len(paths)}] {p.stem:>5}  +{added} URLs"
              + (f"  pulled={n_pulled} failed={n_failed}" if args.pull else ""),
              flush=True)

    print(f"\nprocessed {n_games} games: {n_url} download URLs"
          + (f", {n_pulled} pulled, {n_failed} failed" if args.pull else ""))
    if FETCH_FAILURES:
        print(f"  WARNING: {len(FETCH_FAILURES)} snapshot fetches FAILED "
              f"(refused / timed out). Those games are UNTESTED, not empty. "
              f"Re-run when Wayback is not throttling us, or the URL count "
              f"above understates what is actually recoverable.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
