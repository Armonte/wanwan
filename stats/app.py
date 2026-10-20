"""FM2K Public Stats Site — read-only views over the hub's match log.

Architecture and security model documented at
docs/dev/stats_site_design.md (in the parent repo). Single-file FastAPI
app for v1. Reads `matches.json` from disk on each request — no DB, no
cache invalidation hooks needed; the file is tiny by web-app standards
and we re-parse fast enough that an extra 10ms per request beats the
complexity of a watchdog observer.

Security posture:
  - Read-only file access.
  - Jinja2 autoescape enabled by default for *.html templates; no `safe`
    filter or `Markup()` calls.
  - Discord IDs never rendered. load_matches() swaps p1_id / p2_id /
    winner_id for opaque public ids (privacy.py) and drops p1_dc_id /
    p2_dc_id; players show as name#TAG and link to /u/<public id>.
  - Strict CSP: default-src 'self'; img-src 'self' data:; no inline JS.
  - Per-IP rate limit on /api/v1 endpoints via slowapi.
  - Process should run as an unprivileged user with read-only access to
    the hub's data dir (deploy-time concern, not enforced in code).
"""

from __future__ import annotations

import json
import os
import re
import time
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional

from fastapi import FastAPI, HTTPException, Query, Request
from fastapi.responses import HTMLResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from slowapi import Limiter, _rate_limit_exceeded_handler
from slowapi.errors import RateLimitExceeded
from slowapi.util import get_remote_address

import privacy
import ratings


# ─── Config ──────────────────────────────────────────────────────────────

# Path to the hub's matches.json. Defaults to ../hub/matches.json relative
# to this file so a standard `cd stats && uvicorn app:app` works without
# fiddling. Override via FM2K_MATCHES_PATH for production deploys where
# the hub data dir is elsewhere.
DEFAULT_MATCHES_PATH = Path(__file__).resolve().parent.parent / "hub" / "matches.json"
MATCHES_PATH = Path(os.environ.get("FM2K_MATCHES_PATH", str(DEFAULT_MATCHES_PATH)))

# Game registry — pretty names + (eventually) banners + character art.
# Built by tools/build_registry.py from MyAbandonware + archive.org
# scrapes. Optional: if absent, the site falls back to rendering the
# raw game_id string.
DEFAULT_REGISTRY_PATH = Path(__file__).resolve().parent.parent / "games" / "registry.json"
REGISTRY_PATH = Path(os.environ.get("FM2K_REGISTRY_PATH",
                                    str(DEFAULT_REGISTRY_PATH)))

# Mounted under nginx at /stats by default; the path prefix is consumed by
# the reverse proxy so we serve from "/" internally. If you're running
# uvicorn directly behind no proxy, the urls in templates still work
# because url_for emits relative paths.
ROOT_PATH = os.environ.get("FM2K_STATS_ROOT_PATH", "")

# Maximum matches the JSON API will return in one request — caps the
# bandwidth a single client can hammer out of us.
API_MATCHES_HARD_LIMIT = 500


# ─── App + middleware ────────────────────────────────────────────────────

app = FastAPI(
    title="FM2K Stats",
    description="Public read-only stats for FM2K Rollback hub matches.",
    version="0.1.0",
    root_path=ROOT_PATH,
    docs_url=None,           # hide Swagger UI on the public site
    redoc_url=None,
    openapi_url=None,
)

limiter = Limiter(key_func=get_remote_address)
app.state.limiter = limiter
app.add_exception_handler(RateLimitExceeded, _rate_limit_exceeded_handler)


@app.middleware("http")
async def security_headers(request: Request, call_next):
    """Apply a strict CSP + a few belt-and-suspenders headers to every
    response. Inline scripts are disallowed by default-src + script-src,
    so any future XSS attempt fails to execute."""
    response = await call_next(request)
    # Public pages still ban inline scripts (script-src 'self' allows
    # only same-origin .js files — no inline tags, no eval). The admin
    # editor at /admin/* needs JS for live editing; we ship that as a
    # static file (/static/admin.js), which 'self' permits.
    response.headers["Content-Security-Policy"] = (
        "default-src 'self'; "
        "img-src 'self' data:; "
        "style-src 'self' 'unsafe-inline'; "
        "script-src 'self'; "
        "frame-ancestors 'none'"
    )
    response.headers["X-Content-Type-Options"] = "nosniff"
    response.headers["Referrer-Policy"] = "no-referrer"
    response.headers["X-Frame-Options"] = "DENY"
    return response


_static_dir = Path(__file__).resolve().parent / "static"
_templates_dir = Path(__file__).resolve().parent / "templates"

app.mount("/static", StaticFiles(directory=str(_static_dir)), name="static")
templates = Jinja2Templates(directory=str(_templates_dir))


# ─── Match log loader ────────────────────────────────────────────────────

def load_matches() -> list[dict[str, Any]]:
    """Read matches.json off disk. Returns a defensive copy with only the
    fields the public site is allowed to render. Drops any record missing
    structural keys instead of crashing the renderer."""
    try:
        with MATCHES_PATH.open("r", encoding="utf-8") as f:
            payload = json.load(f)
    except (OSError, json.JSONDecodeError):
        return []
    raw = payload.get("matches", []) if isinstance(payload, dict) else []
    out: list[dict[str, Any]] = []
    for r in raw:
        if not isinstance(r, dict):
            continue
        # Required keys for any aggregation. Missing → skip.
        if not r.get("id") or not r.get("p1_id") or not r.get("p2_id"):
            continue
        if not isinstance(r.get("started_at"), (int, float)):
            continue
        # Whitelist fields. Notably excludes p1_dc_id / p2_dc_id.
        # p1_id / p2_id / winner_id on disk are Discord ids (or guest
        # ids); they are replaced HERE by opaque public ids (privacy.py)
        # so no raw id ever reaches a template. Every HTML aggregation
        # below therefore keys by public id.
        raw1, raw2 = str(r["p1_id"]), str(r["p2_id"])
        p1, p2 = privacy.public_id(raw1), privacy.public_id(raw2)
        w = r.get("winner_id") or ""
        winner = (p1 if w == raw1 else p2 if w == raw2
                  else privacy.public_id(str(w)) if w else "")
        rec: dict[str, Any] = {
            "id":            r.get("id"),
            "p1_id":         p1,
            "p1_nick":       r.get("p1_nick", "anon"),
            "p2_id":         p2,
            "p2_nick":       r.get("p2_nick", "anon"),
            "p1_char_id":    r.get("p1_char_id"),
            "p2_char_id":    r.get("p2_char_id"),
            "p1_char_name":  r.get("p1_char_name"),
            "p2_char_name":  r.get("p2_char_name"),
            "stage_id":      r.get("stage_id"),
            "stage_name":    r.get("stage_name"),
            "game_id":       r.get("game_id", ""),
            "winner_id":     winner,
            "started_at":    r.get("started_at"),
            "finished_at":   r.get("finished_at", r.get("started_at")),
        }
        # Schema 2 (C10): session_id correlates matches into a single
        # session, match_index_in_session is 1-based ordering, rounds[]
        # is per-round mini-records. Default schema 1 = these fields
        # absent. Whitelist + sanitize the rounds[] payload so a
        # malformed entry can't crash the renderer.
        if r.get("schema") == 2:
            rec["schema"]                = 2
            rec["session_id"]            = r.get("session_id") or ""
            rec["match_index_in_session"] = (
                r.get("match_index_in_session") or 0)
            raw_rounds = r.get("rounds")
            clean_rounds: list[dict[str, Any]] = []
            if isinstance(raw_rounds, list):
                for rr in raw_rounds[:8]:
                    if not isinstance(rr, dict):
                        continue
                    w = rr.get("winner")
                    if w not in ("p1", "p2", "draw"):
                        continue
                    clean_rounds.append({
                        "winner":     w,
                        "frames":     int(rr.get("frames") or 0),
                        "p1_hp_left": int(rr.get("p1_hp_left") or 0),
                        "p2_hp_left": int(rr.get("p2_hp_left") or 0),
                    })
            rec["rounds"] = clean_rounds
        out.append(rec)
    return out


# ─── Game registry loader ────────────────────────────────────────────────

# In-memory cache keyed by mtime. The registry is small (~140 records,
# <100KB) and reads are fast; we just want to skip re-parsing on every
# request without locking ourselves out of edits during dev.
_registry_cache: dict[str, Any] = {"mtime": -1.0, "by_id": {}}


def load_registry() -> dict[str, dict[str, Any]]:
    """Return {game_id: record} for the current registry. Empty dict
    when the file is absent or corrupt — callers fall back to raw
    game_id rendering in that case.

    Both the primary game_id AND any aliased_ids[] resolve to the same
    record. This lets old game_id values (preserved during a rename)
    keep finding their renamed game so match history doesn't orphan.
    """
    try:
        st = REGISTRY_PATH.stat()
    except OSError:
        return {}
    if st.st_mtime == _registry_cache["mtime"]:
        return _registry_cache["by_id"]
    try:
        with REGISTRY_PATH.open("r", encoding="utf-8") as f:
            recs = json.load(f)
    except (OSError, json.JSONDecodeError):
        return _registry_cache["by_id"]
    by_id: dict[str, dict[str, Any]] = {}
    for r in recs:
        if not isinstance(r, dict):
            continue
        gid = r.get("game_id")
        if not gid:
            continue
        by_id[gid] = r
        # Aliased ids (left over from rename or merge) point at the
        # same record. Collisions across two records would have been
        # rejected by the rename/merge endpoints; if one slips through
        # anyway, the latest entry wins.
        for alias in (r.get("aliased_ids") or []):
            if alias and alias != gid:
                by_id[alias] = r
    _registry_cache["mtime"] = st.st_mtime
    _registry_cache["by_id"] = by_id
    return by_id


def primary_game_id(any_id: str) -> str:
    """Resolve an id (which may be the primary or an alias) to the
    record's current primary game_id. Returns the input untouched if
    no registry entry exists (e.g. matches recorded for a game that
    never made it into the registry)."""
    rec = load_registry().get(any_id)
    return rec.get("game_id", any_id) if rec else any_id


def game_friendly_name(game_id: str) -> str:
    """Pretty name from the registry. Falls back to the raw game_id so
    a missing entry still renders as something readable."""
    if not game_id:
        return ""
    rec = load_registry().get(game_id)
    if rec and rec.get("name"):
        return rec["name"]
    return game_id


def game_meta(game_id: str) -> dict[str, Any]:
    """Full registry record for a game_id. Empty dict if not in the
    registry. Templates use this to render banner / year / source
    links when available, with .get() defaults so missing fields
    don't crash the render."""
    return load_registry().get(game_id, {})


def banner_color(seed: str) -> str:
    """Generate a deterministic two-stop linear-gradient string for a
    placeholder banner. Each game_id → fixed accent pair so cards
    don't look identical, and a refresh keeps the same visual.

    No JS / external CSS lib — output is "linear-gradient(...)" usable
    inline as a CSS `background:` value. Saturation and lightness
    pinned to fit the existing dark theme."""
    if not seed:
        seed = "fm2k"
    # FNV-1a hash → two hue values 90° apart so the gradient looks
    # purposeful instead of muddy.
    h = 2166136261
    for c in seed.encode("utf-8"):
        h ^= c
        h = (h * 16777619) & 0xFFFFFFFF
    hue1 = h % 360
    hue2 = (hue1 + 90) % 360
    return (f"linear-gradient(135deg, "
            f"hsl({hue1} 60% 28%) 0%, "
            f"hsl({hue2} 55% 18%) 100%)")


def banner_monogram(name: str) -> str:
    """First letter of each significant word, max 3 chars. Renders
    over the gradient when no real banner image exists.
    'Pokemon: Close Combat' → 'PCC'; '2D Fighter Maker 95' → '2FM'."""
    if not name:
        return "?"
    parts = re.findall(r"[A-Za-z0-9]+", name)
    if not parts:
        return name[:1].upper() or "?"
    out = ""
    for p in parts[:3]:
        out += p[0].upper()
    return out


# Make the registry helpers available to every template render
# without threading them through each handler explicitly. Templates
# can call {{ friendly_name(m.game_id) }} or {{ game_meta(gid).year }}.
templates.env.globals["friendly_name"] = game_friendly_name
templates.env.globals["game_meta"] = game_meta
templates.env.globals["banner_color"] = banner_color
templates.env.globals["banner_monogram"] = banner_monogram


def who(nick: Optional[str], pid: str) -> str:
    """Public display identity: name#TAG. Names are never private."""
    return f"{nick or 'anon'}#{privacy.tag_for(pid or '')}"


templates.env.globals["who"] = who


def hidden_pids() -> set[str]:
    """Public ids of opted-out players (their profile renders private)."""
    return {privacy.public_id(dc) for dc in privacy.hidden_ids()}


def user_ratings(pid: str) -> list[dict[str, Any]]:
    """Per-game Glicko-2 rows (rating, rd, rank, provisional) for a
    profile, from the same snapshot the JSON API serves. Empty for
    players with no Discord-linked rated matches."""
    snap = ratings.snapshot(MATCHES_PATH)
    dc = snap.by_pid.get(pid)
    if dc is None:
        return []
    return _api_v1.profile(snap, dc).get("games", [])


def char_label(name: Optional[str], cid: Optional[int]) -> str:
    """Render a friendly character label. Filename (already UTF-8) when
    the hook resolved one; "id N" fallback otherwise; "?" for fully
    unknown."""
    if isinstance(name, str) and name:
        return name
    if isinstance(cid, int):
        return f"id {cid}"
    return "?"


def format_iso(ts: float) -> str:
    """Sortable, human-readable timestamp. UTC for consistency across
    deploys."""
    try:
        return datetime.fromtimestamp(ts, tz=timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    except (OSError, ValueError, OverflowError):
        return "?"


def aggregate_user(user_id: str) -> dict[str, Any]:
    """Build the per-user rollups consumed by /u/<id>. One full pass over
    the match log; everything derives from the same view of the data."""
    matches = [m for m in load_matches() if m["p1_id"] == user_id or m["p2_id"] == user_id]
    if not matches:
        return {
            "user_id": user_id, "nick": user_id, "aliases": [],
            "total": 0, "wins": 0, "losses": 0, "draws": 0,
            "by_game": [], "by_char": [], "vs_opponents": [],
            "recent": [],
        }
    nick = matches[-1]["p1_nick"] if matches[-1]["p1_id"] == user_id else matches[-1]["p2_nick"]

    # Nick history — Steam-style "also known as" tracking. We key by
    # the Discord user_id (= persistent identity) so a player can
    # change launcher nicks freely; their stats stay attributed and
    # the profile shows every alias they've used.
    #
    # Self-vs-self note: dev sessions where the same Discord user runs
    # two launchers produce matches where p1_id == p2_id; we count
    # BOTH nicks in that case so a nick used only on the p2 side of
    # such a match still shows up. Counting only one side would drop
    # alias rows that exist nowhere else in the log.
    nick_counts: Counter = Counter()
    nick_last_seen: dict[str, float] = {}
    def _bump(n: str, ts: float):
        if not n: return
        nick_counts[n] += 1
        if ts > nick_last_seen.get(n, 0.0):
            nick_last_seen[n] = ts
    for m in matches:
        ts = m["finished_at"]
        if m["p1_id"] == user_id:
            _bump(m.get("p1_nick", ""), ts)
        if m["p2_id"] == user_id:
            _bump(m.get("p2_nick", ""), ts)
    # Current nick = most recently used (matches the live launcher).
    if nick_last_seen:
        nick = max(nick_last_seen, key=lambda k: nick_last_seen[k])
    # Aliases = all OTHER nicks, ordered most-used first.
    aliases = [
        {"nick": n, "matches": nick_counts[n], "last_seen": nick_last_seen[n]}
        for n in nick_counts if n != nick
    ]
    aliases.sort(key=lambda a: -a["matches"])

    wins = losses = draws = 0
    by_game: dict[str, dict[str, int]] = defaultdict(lambda: {"wins": 0, "losses": 0, "draws": 0})
    by_char: dict[str, dict[str, int]] = defaultdict(lambda: {"wins": 0, "losses": 0, "draws": 0})
    vs_opp:  dict[str, dict[str, Any]] = defaultdict(
        lambda: {"opp_id": "", "opp_nick": "", "wins": 0, "losses": 0, "draws": 0})

    for m in matches:
        i_am_p1   = (m["p1_id"] == user_id)
        opp_id    = m["p2_id"]   if i_am_p1 else m["p1_id"]
        opp_nick  = m["p2_nick"] if i_am_p1 else m["p1_nick"]
        my_char   = char_label(m["p1_char_name"] if i_am_p1 else m["p2_char_name"],
                                m["p1_char_id"]   if i_am_p1 else m["p2_char_id"])
        gkey      = m["game_id"] or "(unknown)"
        winner    = m["winner_id"]

        if not winner:
            r = "draws"
            draws += 1
        elif winner == user_id:
            r = "wins"
            wins += 1
        else:
            r = "losses"
            losses += 1
        by_game[gkey][r] += 1
        by_char[my_char][r] += 1
        cell = vs_opp[opp_id]
        cell["opp_id"]   = opp_id
        cell["opp_nick"] = opp_nick
        cell[r] += 1

    # Sort: most-played first within each grouping.
    by_game_rows = sorted(
        ({"game_id": g, **rec} for g, rec in by_game.items()),
        key=lambda r: -(r["wins"] + r["losses"] + r["draws"]))
    by_char_rows = sorted(
        ({"char": c, **rec} for c, rec in by_char.items()),
        key=lambda r: -(r["wins"] + r["losses"] + r["draws"]))
    vs_rows = sorted(
        vs_opp.values(),
        key=lambda r: -(r["wins"] + r["losses"] + r["draws"]))[:32]

    recent = sorted(matches, key=lambda m: -m["finished_at"])[:20]
    return {
        "user_id": user_id, "nick": nick, "aliases": aliases,
        "total": len(matches), "wins": wins, "losses": losses, "draws": draws,
        "by_game": by_game_rows[:32],
        "by_char": by_char_rows[:32],
        "vs_opponents": vs_rows,
        "recent": recent,
    }


def aggregate_game(game_id: str) -> dict[str, Any]:
    # Pull matches recorded under either the current game_id or any
    # of the record's previous aliases. matches.json keys by game_id
    # and we never rewrite it, so aliased history would otherwise
    # orphan after a rename.
    rec = load_registry().get(game_id) or {}
    aliases = set(rec.get("aliased_ids") or [])
    aliases.add(rec.get("game_id") or game_id)
    matches = [m for m in load_matches() if m["game_id"] in aliases]
    if not matches:
        meta = game_meta(game_id)
        archive = {
            "atwiki_id":        meta.get("atwiki_id"),
            "atwiki_url":       meta.get("atwiki_url"),
            "homepage":         meta.get("homepage"),
            "wayback_homepage": meta.get("wayback_homepage"),
            "archive_status":   meta.get("archive_status"),
            "resources":        meta.get("resources") or [],
            "versions":         meta.get("versions") or [],
        }
        return {"game_id": game_id, "total": 0, "top_players": [],
                "top_chars": [], "recent": [], "archive": archive}

    by_player: Counter = Counter()
    by_player_nick: dict[str, str] = {}
    # Per-character record keyed by char label. Tracks pick count plus
    # W/L/D so the template can render winrate alongside picks. Draws
    # are split between both characters (count toward neither W nor L,
    # only the picks total). The winner_id field is empty string on
    # draws — that's the sentinel we key off.
    char_stats: dict[str, dict[str, int]] = defaultdict(
        lambda: {"picks": 0, "wins": 0, "losses": 0, "draws": 0})
    for m in matches:
        by_player[m["p1_id"]] += 1
        by_player[m["p2_id"]] += 1
        by_player_nick[m["p1_id"]] = m["p1_nick"]
        by_player_nick[m["p2_id"]] = m["p2_nick"]

        c1 = char_label(m["p1_char_name"], m["p1_char_id"])
        c2 = char_label(m["p2_char_name"], m["p2_char_id"])
        char_stats[c1]["picks"] += 1
        char_stats[c2]["picks"] += 1
        wid = m["winner_id"]
        if not wid:
            char_stats[c1]["draws"] += 1
            char_stats[c2]["draws"] += 1
        elif wid == m["p1_id"]:
            char_stats[c1]["wins"] += 1
            char_stats[c2]["losses"] += 1
        elif wid == m["p2_id"]:
            char_stats[c2]["wins"] += 1
            char_stats[c1]["losses"] += 1

    top_players = [
        {"user_id": uid, "nick": by_player_nick[uid], "matches": n}
        for uid, n in by_player.most_common(20)
    ]

    # Build the full character roster (no cap). Winrate excludes draws
    # from the denominator since "what's the win-rate of this char?"
    # most naturally reads as wins / decisive games. Sort by pick
    # count desc; ties broken by winrate desc so the same-pick chars
    # surface the strongest first.
    #
    # The "?" label is the fallback for player slots where both
    # char_name and char_id resolved to null — see char_label(). It's
    # not a real character; surface only as a data-quality footnote
    # via top_chars_skipped, kept out of the visible leaderboard.
    top_chars: list[dict[str, Any]] = []
    top_chars_skipped: dict[str, int] = {}
    for c, s in char_stats.items():
        if c == "?":
            top_chars_skipped["unknown_picks"] = s["picks"]
            continue
        decisive = s["wins"] + s["losses"]
        winrate = (s["wins"] / decisive) if decisive else 0.0
        top_chars.append({
            "char":    c,
            "matches": s["picks"],
            "wins":    s["wins"],
            "losses":  s["losses"],
            "draws":   s["draws"],
            "winrate": winrate,
        })
    top_chars.sort(key=lambda r: (-r["matches"], -r["winrate"]))

    recent = sorted(matches, key=lambda m: -m["finished_at"])[:20]
    # Archive panel — pulled from the registry (populated by
    # tools/atwiki_merge_to_registry.py). Templates render only when
    # at least one archive field is present, so games not in atwiki
    # gracefully fall through to the original layout.
    meta = game_meta(game_id)
    archive = {
        "atwiki_id":         meta.get("atwiki_id"),
        "atwiki_url":        meta.get("atwiki_url"),
        "homepage":          meta.get("homepage"),
        "wayback_homepage":  meta.get("wayback_homepage"),
        "archive_status":    meta.get("archive_status"),
        "resources":         meta.get("resources") or [],
        "versions":          meta.get("versions") or [],
    }
    return {"game_id": game_id, "total": len(matches),
            "top_players": top_players, "top_chars": top_chars,
            "recent": recent, "archive": archive}


def aggregate_character(game_id: str, char_key: str) -> dict[str, Any]:
    """char_key matches against either char_name (preferred) or `id N`
    so URLs can be human-readable when names are available and still
    work for unmapped slots."""
    matches = [m for m in load_matches() if m["game_id"] == game_id]
    on_p1 = [m for m in matches if char_label(m["p1_char_name"], m["p1_char_id"]) == char_key]
    on_p2 = [m for m in matches if char_label(m["p2_char_name"], m["p2_char_id"]) == char_key]
    plays = on_p1 + on_p2
    if not plays:
        return {"game_id": game_id, "char": char_key, "total": 0,
                "top_pilots": [], "matchups": [], "recent": []}

    by_pilot: dict[str, dict[str, Any]] = defaultdict(
        lambda: {"user_id": "", "nick": "", "wins": 0, "losses": 0, "draws": 0})
    matchups: dict[str, dict[str, int]] = defaultdict(
        lambda: {"wins": 0, "losses": 0, "draws": 0})

    def record(pilot_id: str, pilot_nick: str, opp_char: str, winner_id: str):
        cell = by_pilot[pilot_id]
        cell["user_id"] = pilot_id
        cell["nick"]    = pilot_nick
        if not winner_id:
            cell["draws"] += 1; matchups[opp_char]["draws"] += 1
        elif winner_id == pilot_id:
            cell["wins"] += 1;  matchups[opp_char]["wins"] += 1
        else:
            cell["losses"] += 1; matchups[opp_char]["losses"] += 1

    for m in on_p1:
        record(m["p1_id"], m["p1_nick"],
               char_label(m["p2_char_name"], m["p2_char_id"]), m["winner_id"])
    for m in on_p2:
        record(m["p2_id"], m["p2_nick"],
               char_label(m["p1_char_name"], m["p1_char_id"]), m["winner_id"])

    pilots = sorted(by_pilot.values(),
                    key=lambda r: -(r["wins"] + r["losses"] + r["draws"]))[:20]
    mu_rows = sorted(
        ({"opp_char": c, **rec} for c, rec in matchups.items()),
        key=lambda r: -(r["wins"] + r["losses"] + r["draws"]))[:32]
    recent = sorted(plays, key=lambda m: -m["finished_at"])[:20]
    return {"game_id": game_id, "char": char_key, "total": len(plays),
            "top_pilots": pilots, "matchups": mu_rows, "recent": recent}


# ─── Routes ──────────────────────────────────────────────────────────────

def _games_grid_data(matches: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Aggregate all known games for the /games grid view.

    Combines:
      - registry.json entries (every game we've ever cataloged,
        regardless of whether matches have been played on it)
      - distinct game_id values from matches.json (for any games that
        appeared in match logs but aren't in the registry yet —
        usually means a regular plays a game we haven't curated)

    Returns list of {game_id, name, year, sources, engine,
    has_kgt, banner_url, thumb_url, match_count, last_played_at}.
    Sorted by name, case-insensitive.
    """
    by_id = load_registry()
    # Match counts per game_id (only games that show up in real
    # matches; the grid still shows the full registry but we want to
    # surface "active" games at the top later).
    counts: Counter[str] = Counter()
    last_played: dict[str, float] = {}
    for m in matches:
        gid = m.get("game_id") or ""
        if not gid:
            continue
        counts[gid] += 1
        last_played[gid] = max(last_played.get(gid, 0.0),
                               float(m.get("started_at") or 0.0))

    # Union of registry game_ids + match-only game_ids.
    all_ids = set(by_id.keys()) | set(counts.keys())
    rows: list[dict[str, Any]] = []
    for gid in all_ids:
        rec = by_id.get(gid, {})
        rows.append({
            "game_id":         gid,
            "name":            rec.get("name") or gid,
            "year":            rec.get("year") or "",
            "engine":          rec.get("engine") or "FM2K",
            "sources":         rec.get("sources") or [],
            "has_kgt":         bool(rec.get("kgt_filename")),
            "banner_url":      rec.get("banner_url") or "",
            "thumb_url":       rec.get("thumb_url") or "",
            "homepage":        rec.get("homepage") or "",
            "developer":       rec.get("developer") or "",
            "match_count":     counts.get(gid, 0),
            "last_played_at":  last_played.get(gid, 0.0),
        })
    rows.sort(key=lambda r: (r["name"] or r["game_id"]).lower())
    return rows


@app.get("/games", response_class=HTMLResponse)
def games_grid(request: Request):
    """Fightcade-style game grid — every catalog entry as a card with
    name + year + match count, plus a banner placeholder when the
    registry doesn't (yet) ship a real image."""
    rows = _games_grid_data(load_matches())
    total_games   = len(rows)
    total_active  = sum(1 for r in rows if r["match_count"] > 0)
    return templates.TemplateResponse(
        request, "games_grid.html",
        {"rows": rows,
         "total_games": total_games,
         "total_active": total_active})


@app.get("/", response_class=HTMLResponse)
def landing(request: Request):
    matches = load_matches()
    now = time.time()
    today_cutoff = now - 86400
    week_cutoff = now - 7 * 86400

    by_player: Counter = Counter()
    by_player_nick: dict[str, str] = {}
    by_game: Counter = Counter()
    for m in matches:
        by_player[m["p1_id"]] += 1
        by_player[m["p2_id"]] += 1
        by_player_nick[m["p1_id"]] = m["p1_nick"]
        by_player_nick[m["p2_id"]] = m["p2_nick"]
        if m["game_id"]:
            by_game[m["game_id"]] += 1

    top_players = [
        {"user_id": uid, "nick": by_player_nick[uid], "matches": n}
        for uid, n in by_player.most_common(10)
    ]
    top_games = by_game.most_common(10)
    return templates.TemplateResponse("landing.html", {
        "request": request,
        "total_all":   len(matches),
        "total_day":   sum(1 for m in matches if m["finished_at"] >= today_cutoff),
        "total_week":  sum(1 for m in matches if m["finished_at"] >= week_cutoff),
        "top_players": top_players,
        "top_games":   top_games,
    })


@app.get("/u/{public_id}", response_class=HTMLResponse)
def user_page(request: Request, public_id: str):
    if not public_id or len(public_id) > 64:
        raise HTTPException(status_code=404)
    data = aggregate_user(public_id)
    if data["total"] == 0:
        # Legacy /u/<discord id> links: 301 to the public id. This only
        # confirms the mapping to someone who already holds the raw id.
        alt = privacy.public_id(public_id)
        if alt != public_id and aggregate_user(alt)["total"]:
            from fastapi.responses import RedirectResponse
            return RedirectResponse(url=f"/u/{alt}", status_code=301)
        raise HTTPException(status_code=404, detail="No matches for this user.")
    if public_id in hidden_pids():
        return templates.TemplateResponse("user_private.html",
            {"request": request, "u": {"user_id": public_id,
                                       "nick": data["nick"]}})
    data["ratings"] = user_ratings(public_id)
    return templates.TemplateResponse("user.html",
        {"request": request, "u": data, "char_label": char_label,
         "format_iso": format_iso})


@app.get("/g/{game_id}", response_class=HTMLResponse)
def game_page(request: Request, game_id: str):
    # If someone hits an old aliased URL, 301 to the renamed one so
    # bookmarks / external links keep working but settle on canonical.
    primary = primary_game_id(game_id)
    if primary != game_id:
        from fastapi.responses import RedirectResponse
        return RedirectResponse(url=f"/g/{primary}", status_code=301)
    if not game_id or len(game_id) > 64:
        raise HTTPException(status_code=404)
    data = aggregate_game(game_id)
    # 404 only when we have neither match history nor any registry/
    # archive data to render. The page is also useful as a static
    # info card for games we've cataloged but never played on the hub.
    if data["total"] == 0 and not (data.get("archive") or {}).get("atwiki_id"):
        meta = game_meta(game_id)
        if not meta or not meta.get("name"):
            raise HTTPException(status_code=404, detail="Unknown game.")
    return templates.TemplateResponse("game.html",
        {"request": request, "g": data, "char_label": char_label,
         "format_iso": format_iso})


@app.get("/g/{game_id}/c/{char_key}", response_class=HTMLResponse)
def character_page(request: Request, game_id: str, char_key: str):
    if not game_id or len(game_id) > 64 or not char_key or len(char_key) > 96:
        raise HTTPException(status_code=404)
    data = aggregate_character(game_id, char_key)
    if data["total"] == 0:
        raise HTTPException(status_code=404, detail="No matches for this character.")
    return templates.TemplateResponse("character.html",
        {"request": request, "c": data, "char_label": char_label,
         "format_iso": format_iso})


@app.get("/recent", response_class=HTMLResponse)
def recent_page(request: Request, limit: int = 100):
    limit = max(1, min(limit, 200))
    matches = load_matches()
    matches = sorted(matches, key=lambda m: -m["finished_at"])[:limit]
    return templates.TemplateResponse("recent.html",
        {"request": request, "matches": matches,
         "char_label": char_label, "format_iso": format_iso})


def _sessions_grid_data(matches: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Aggregate schema-2 matches by session_id. Each session row carries
    the participants, game, total matches, score (rounds_won totals across
    matches), and the first/last finished_at for sorting + display.
    Schema-1 records (no session_id) are excluded — they pre-date the
    session-tracking rollout and have nothing to group.

    Sort: most-recent finished_at first."""
    by_session: dict[str, dict[str, Any]] = {}
    for m in matches:
        sid = m.get("session_id")
        if not sid:
            continue  # legacy schema-1 row
        s = by_session.get(sid)
        if s is None:
            s = {
                "session_id":  sid,
                "p1_id":       m["p1_id"],
                "p1_nick":     m["p1_nick"],
                "p2_id":       m["p2_id"],
                "p2_nick":     m["p2_nick"],
                "game_id":     m["game_id"],
                "matches":     [],
                "p1_match_wins": 0,
                "p2_match_wins": 0,
                "draws":       0,
                "started_at":  m["started_at"],
                "finished_at": m["finished_at"],
            }
            by_session[sid] = s
        s["matches"].append(m)
        if m["winner_id"] == m["p1_id"]:
            s["p1_match_wins"] += 1
        elif m["winner_id"] == m["p2_id"]:
            s["p2_match_wins"] += 1
        else:
            s["draws"] += 1
        s["started_at"]  = min(s["started_at"], m["started_at"])
        s["finished_at"] = max(s["finished_at"], m["finished_at"])
    # Per-session match list ordered by match_index_in_session (falls back
    # to finished_at when index is missing/zero) so the rendered tree
    # shows match 1 → match N in temporal order.
    for s in by_session.values():
        s["matches"].sort(key=lambda mm: (
            mm.get("match_index_in_session") or 0,
            mm["finished_at"]))
    return sorted(by_session.values(),
                  key=lambda s: -s["finished_at"])


@app.get("/sessions", response_class=HTMLResponse)
def sessions_page(request: Request, limit: int = 50):
    limit = max(1, min(limit, 200))
    matches = load_matches()
    sessions = _sessions_grid_data(matches)[:limit]
    return templates.TemplateResponse("sessions.html",
        {"request": request, "sessions": sessions,
         "char_label": char_label, "format_iso": format_iso})


# JSON API v1 (launcher Statistics tab): ratings, leaderboards, profiles,
# recent matches, plus the legacy /api/v1/matches feed. See api_v1.py and
# API.md. Ids there are opaque public ids, never Discord ids.
import api_v1 as _api_v1     # noqa: E402
_api_v1.engine_of = lambda gid: game_meta(gid).get("engine")
app.include_router(_api_v1.build_router(
    limiter, lambda: MATCHES_PATH, game_friendly_name))


@app.exception_handler(404)
async def not_found(request: Request, exc: HTTPException):
    return templates.TemplateResponse(
        "404.html",
        {"request": request, "detail": getattr(exc, "detail", None)},
        status_code=404,
    )


# ─── Admin editor (mounted under /admin) ─────────────────────────────────
# Trusted-user write surface. Auth is bearer-token via cookie or
# Authorization header — see stats/admin.py for the model.
import admin as _admin       # noqa: E402  (imported after handlers above)
_admin.set_registry_path(REGISTRY_PATH)
_admin.attach_templates(templates)
app.include_router(_admin.router)
