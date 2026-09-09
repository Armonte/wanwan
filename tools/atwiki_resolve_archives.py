#!/usr/bin/env python3
"""Phase 3: resolve dead outbound links to Wayback snapshots.

Most FM2K-era game homepages were on Geocities JP (closed 2019-03)
and other free-host services that have since died. The Internet
Archive has snapshots of most of them — we just need to query CDX,
filter to the pre-shutdown era, and pick the latest available.

Per-link policy:
  1. Always query the CDX index (cheap, ~100ms each).
  2. Prefer snapshots with statuscode=200 and timestamp <= 2018-12-31
     (Geocities JP shut March 2019; pages got LAST-CHANCE notices in
     late 2018, so the freshest pre-shutdown snapshot is the most
     complete article AND most likely to still be reachable in Wayback).
  3. Fall back to the latest 2xx of any era if no <=2018 snapshot exists
     (some sites lived past 2018 — newer doujin homepages, etc).
  4. Skip if zero 2xx snapshots — record as 'no_archive' so the
     downstream download phase can warn.

Output is merged into the existing per-game JSON under an "outbound"
field — each link gains an "archive" sub-record:

  "outbound": [
    {
      "url": "http://www.geocities.jp/foo/...",
      "label": "Homepage",
      "archive": {
        "wayback_url":  "https://web.archive.org/web/20181201123456/...",
        "timestamp":    "20181201123456",
        "live_status":  "dead" | "live" | "unknown",
        "preferred":    "wayback_pre2019" | "wayback_latest" | "live"
      }
    }
  ]
"""

import argparse
import http.client
import json
import re
import ssl
import sys
import time
from pathlib import Path
from urllib.parse import quote, urlparse
from urllib.request import Request, urlopen
from urllib.error import URLError, HTTPError


REPO = Path(__file__).resolve().parents[1]
GAMES_DIR = REPO / "data" / "fm2k_wiki" / "games"

# Geocities JP shutdown was 2019-03-31. We prefer snapshots BEFORE this
# (using end-of-2018 as a clean cutoff) since post-shutdown snapshots
# tend to be 404-redirects or shutdown placeholders.
PRE_SHUTDOWN_CUTOFF = "20181231"

CDX_BASE = "https://web.archive.org/cdx/search/cdx"
USER_AGENT = ("Mozilla/5.0 (compatible; fm2k-archive-crawler/1.0; "
              "+https://github.com/Armonte/wanwan)")

# Circuit breaker for CDX outages. CDX has been on/off in early 2026;
# when it's down, every query stalls until timeout.
#
# THE BREAKER USED TO BE PERMANENT, AND THAT COST US THE WHOLE RUN. On
# 2026-09-09 it tripped at line 5 of a 336-game resolve -- five slow queries at
# the very start -- and every game after that was resolved by the weaker
# probe-only fallback. The run reported "302 unarchivable"; spot-checking those
# against CDX by hand immediately found sites with 50 snapshots. A transient
# stall at the start must not decide the outcome for the remaining 99% of the
# work, so the breaker now COOLS DOWN and re-arms instead of latching off. The
# 5s timeout that produced those failures was also far too tight for CDX, which
# routinely takes 10s+.
CDX_FAILURE_THRESHOLD = 5
CDX_COOLDOWN_S = 60.0
_cdx_consec_failures = 0
_cdx_disabled_until = 0.0


def cdx_query(url: str, timeout: float = 20.0) -> list[tuple[str, str]]:
    """Query the Wayback CDX index for `url`, returning a list of
    (timestamp, archived_url) for 2xx snapshots, sorted oldest -> newest.

    Returns [] on any error (network, parse, no snapshots).

    Includes a process-level circuit breaker: after CDX_FAILURE_THRESHOLD
    consecutive failures we mark CDX as disabled and short-circuit
    subsequent calls. Lets the run fall through to the probe fallback
    immediately during outages instead of paying ~5s per query."""
    global _cdx_consec_failures, _cdx_disabled_until
    now = time.time()
    if now < _cdx_disabled_until:
        return []
    # No statuscode filter. Requiring 200 threw away pages archived as a 3xx,
    # which is a common shape for the Geocities-era hosts in this corpus; the
    # 2xx selection happens below, where a redirect can still be followed.
    q = (f"{CDX_BASE}?url={quote(url, safe=':/?&=')}"
         f"&output=json&limit=500")
    req = Request(q, headers={"User-Agent": USER_AGENT})
    try:
        with urlopen(req, timeout=timeout) as r:
            raw = r.read().decode("utf-8", errors="replace")
    except (URLError, HTTPError, TimeoutError):
        _cdx_consec_failures += 1
        if _cdx_consec_failures >= CDX_FAILURE_THRESHOLD:
            _cdx_disabled_until = time.time() + CDX_COOLDOWN_S
            _cdx_consec_failures = 0
            print(f"  [cdx] cooling down {CDX_COOLDOWN_S:.0f}s after "
                  f"{CDX_FAILURE_THRESHOLD} failures (will re-arm)", flush=True)
        return []
    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        # CDX returns 503 + HTML during outages — counts as failure.
        _cdx_consec_failures += 1
        if _cdx_consec_failures >= CDX_FAILURE_THRESHOLD:
            _cdx_disabled_until = time.time() + CDX_COOLDOWN_S
            _cdx_consec_failures = 0
            print(f"  [cdx] cooling down {CDX_COOLDOWN_S:.0f}s after "
                  f"{CDX_FAILURE_THRESHOLD} failures (will re-arm)", flush=True)
        return []
    # Success — reset counter.
    _cdx_consec_failures = 0
    if not data or len(data) < 2:
        return []
    # Header row first; remaining rows are snapshots.
    rows = data[1:]
    # CDX columns: urlkey, timestamp, original, mimetype, statuscode, digest,
    # length. The query no longer filters statuscode, so filter HERE and keep
    # the preference explicit: 2xx is a real capture, 3xx is a redirect whose
    # target is usually also archived and is worth following, 4xx/5xx are
    # captures of an error page and are worthless. Taking every row (which is
    # what dropping the query filter naively would do) would record 404 pages
    # as successful resolutions -- worse than the false negatives it fixes.
    ok, redirs = [], []
    for row in rows:
        if len(row) < 3:
            continue
        ts, original = row[1], row[2]
        status = row[4] if len(row) > 4 else ""
        wb_url = f"https://web.archive.org/web/{ts}/{original}"
        if status.startswith("2") or status == "-":
            ok.append((ts, wb_url))
        elif status.startswith("3"):
            redirs.append((ts, wb_url))
    snaps = ok or redirs
    snaps.sort()  # ascending by timestamp
    return snaps


def wayback_probe(url: str, year_hint: str = "20181231",
                  timeout: float = 15.0) -> dict | None:
    """CDX-free fallback: use Wayback's `/web/<date>id_/<url>` redirect
    to discover the closest snapshot to a given date. Works even when
    CDX is 503'd (the May-2026 outage we hit mid-crawl).

    Implementation note: we deliberately use http.client rather than
    urllib here. urllib auto-follows 302s, which means it actually
    fetches the entire archived HTML body just to throw it away —
    plus on this WSL host urllib's redirect handler returns a fast
    "connection refused" for reasons that don't repro with curl or
    raw http.client. http.client lets us read the Location header
    directly and skip the redirect entirely.
    """
    path = f"/web/{year_hint}id_/{url}"
    # Two attempts — Wayback throws transient 5xx fairly often during
    # outage windows (the May-2026 episode caused a ~80% miss rate on
    # the first corpus pass). One retry with a short backoff reclaims
    # most of those without dragging out the run.
    for attempt in (0, 1):
        if attempt:
            time.sleep(1.0)
        conn = None
        try:
            conn = http.client.HTTPSConnection("web.archive.org",
                                               timeout=timeout)
            conn.request("HEAD", path,
                         headers={"User-Agent": USER_AGENT})
            r = conn.getresponse()
        except (OSError, http.client.HTTPException):
            if conn:
                try: conn.close()
                except Exception: pass
            continue
        # 302 = found a capture, 404 = no capture exists, 5xx = retry.
        if r.status == 302:
            loc = r.getheader("Location")
            try: conn.close()
            except Exception: pass
            if loc:
                m = re.search(r'/web/(\d{8,14})', loc)
                if m:
                    return {"wayback_url": loc,
                            "timestamp":   m.group(1),
                            "preferred":   "wayback_probe"}
            return None
        if r.status >= 500:
            try: conn.close()
            except Exception: pass
            continue   # retry once
        try: conn.close()
        except Exception: pass
        return None    # 404 / 4xx = genuinely no capture
    return None


def pick_snapshot(snaps: list[tuple[str, str]]) -> dict | None:
    """Pick the best Wayback snapshot per the policy in the module
    docstring. Returns None if no usable snapshot."""
    if not snaps:
        return None
    pre = [s for s in snaps if s[0] <= PRE_SHUTDOWN_CUTOFF + "999999"]
    if pre:
        ts, url = pre[-1]   # latest pre-shutdown
        return {
            "wayback_url": url,
            "timestamp":   ts,
            "preferred":   "wayback_pre2019",
        }
    ts, url = snaps[-1]   # latest of any era
    return {
        "wayback_url": url,
        "timestamp":   ts,
        "preferred":   "wayback_latest",
    }


# Year hints for the probe-only fallback path. Wayback's redirect-to-
# closest behavior means a HEAD against /web/<year>id_/<url> returns the
# closest snapshot to that year. Trying multiple year hints widens the
# search window across the lifespan of old Japanese free-host pages:
#   1998 — early geocities, freett, tripod
#   2003 — first generation peak
#   2009 — geocities US shutdown era
#   2018 — geocities JP shutdown era (our original single-shot value)
PROBE_YEAR_HINTS = ("19981231", "20031231", "20091231", "20181231")


def wayback_probe_multi(url: str) -> dict | None:
    """Try wayback_probe across multiple year hints. First success
    wins. Used as the CDX-down fallback so we don't miss URLs whose
    only snapshot is from outside the 2018 window."""
    for year in PROBE_YEAR_HINTS:
        pick = wayback_probe(url, year_hint=year)
        if pick:
            return pick
    return None


def resolve_one_game(game_path: Path, throttle: float = 0.3,
                     force: bool = False,
                     retry_no_archive: bool = False
                     ) -> tuple[int, int, int]:
    """Returns (n_resolved, n_no_archive, n_recovered_on_retry) for the game."""
    record = json.loads(game_path.read_text(encoding="utf-8"))
    outbound = record.get("outbound", [])
    if not outbound:
        return 0, 0, 0
    n_resolved = n_none = n_recovered = 0
    for link in outbound:
        prev = link.get("archive") or {}
        was_no_archive = prev.get("preferred") == "no_archive"
        if "archive" in link and not force and not (
                retry_no_archive and was_no_archive):
            # Already resolved on a previous run, and not retrying.
            continue
        # Primary path: CDX index (gives full snapshot list, lets us
        # apply the pre-shutdown timestamp policy).
        snaps = cdx_query(link["url"])
        pick = pick_snapshot(snaps)
        # Fallback: Wayback redirect probe across multiple year hints.
        # Used when CDX is down (503 outage) or has no record of this
        # URL but Wayback has a snapshot indexed only via the front-end.
        if not pick:
            pick = wayback_probe_multi(link["url"])
        if pick:
            link["archive"] = pick
            n_resolved += 1
            if was_no_archive:
                n_recovered += 1
        else:
            link["archive"] = {"wayback_url": None,
                               "timestamp": None,
                               "preferred": "no_archive"}
            n_none += 1
        time.sleep(throttle)
    game_path.write_text(json.dumps(record, ensure_ascii=False, indent=2),
                         encoding="utf-8")
    return n_resolved, n_none, n_recovered


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--limit", type=int, default=0,
                    help="stop after N games (smoke test)")
    ap.add_argument("--throttle", type=float, default=0.3,
                    help="seconds between CDX requests (default 0.3)")
    ap.add_argument("--force", action="store_true",
                    help="re-resolve even if previously archived")
    ap.add_argument("--retry-no-archive", action="store_true",
                    help="re-resolve only links currently marked "
                         "preferred=no_archive (uses multi-year probe hints)")
    args = ap.parse_args()

    if not GAMES_DIR.exists():
        print(f"missing {GAMES_DIR} — run atwiki_parse_pages.py first",
              file=sys.stderr)
        return 1

    paths = sorted(GAMES_DIR.glob("*.json"))
    if args.limit:
        paths = paths[: args.limit]
    if not paths:
        print("no parsed game files found", file=sys.stderr)
        return 1

    total_resolved = total_none = total_recovered = 0
    for i, p in enumerate(paths, 1):
        r, n, rec = resolve_one_game(
            p, throttle=args.throttle, force=args.force,
            retry_no_archive=args.retry_no_archive)
        total_resolved += r
        total_none += n
        total_recovered += rec
        if r or rec or args.limit:
            print(f"[{i:3d}/{len(paths)}] {p.stem:>5}  +{r} archived  "
                  f"-{n} no-snap  ↻{rec} recovered",
                  flush=True)

    print(f"\nresolved {total_resolved} archived links, "
          f"{total_none} unarchivable, "
          f"{total_recovered} recovered on retry")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
