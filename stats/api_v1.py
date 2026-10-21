"""JSON API v1 for the launcher's Statistics tab. Shapes are documented in
stats/API.md; change them only together with that file.

Privacy rules (see privacy.py): no Discord id or IP is ever serialized;
players are {public_id, name, tag}; an opted-out player's profile is a
{"private": true, ...} stub and they are left off leaderboards, but their
name + tag still show in match rows (names are never private).
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Callable, Optional

from fastapi import APIRouter, Query, Request
from fastapi.responses import JSONResponse

import privacy
import ratings
from ratings import GameRecord, Match, Snapshot

# Leaderboard eligibility: a player is listed once their rating is
# reasonably settled -- RD at or under LB_MAX_RD, or at least
# LB_MIN_MATCHES rated matches in that game. Everyone else is provisional.
LB_MAX_RD = 150.0
LB_MIN_MATCHES = 10
LB_DEFAULT_LIMIT = 50
LB_HARD_LIMIT = 200
RECENT_DEFAULT_LIMIT = 50
RECENT_HARD_LIMIT = 200
PROFILE_RECENT = 20
PROFILE_TOP_CHARS = 5


# Registry engine lookup ("FM2K" / "FM95") for bare legacy game ids; the
# app points this at its game registry. Prefixed ids never need it.
engine_of: Callable[[str], Optional[str]] = lambda gid: None


def family(game_id: str) -> str:
    return ratings.game_family(game_id, engine_of(game_id))


def _r1(x: float) -> float:
    return round(x, 1)


def player_obj(snap: Snapshot, dc: str,
               viewer: Optional[str] = None) -> dict[str, Any]:
    pid = privacy.public_id(dc)
    out: dict[str, Any] = {"public_id": pid,
                           "name": snap.nicks.get(dc, "anon"),
                           "tag": snap.tags.get(dc) or privacy.tag_for(pid)}
    if privacy.is_hidden(dc, viewer):
        out["private"] = True
    return out


def match_obj(snap: Snapshot, m: Match,
              viewer: Optional[str] = None) -> dict[str, Any]:
    p1 = player_obj(snap, m.p1, viewer)
    p2 = player_obj(snap, m.p2, viewer)
    # Names are never private: an opted-out player keeps name + tag (and
    # the character they played) in match rows; only their profile/stats
    # and leaderboard rows are hidden.
    p1["char"] = m.p1_char
    p2["char"] = m.p2_char
    return {
        "id": m.id,
        "game_id": m.game_id,
        "family": family(m.game_id),
        "finished_at": m.finished_at,
        "p1": p1,
        "p2": p2,
        "winner": m.winner,
        "score": list(m.score) if m.score else None,
        "disconnect": m.disconnect,
        "repaired": m.repaired,
        "stage": m.stage,
        "replay_url": m.replay_url,
        "status": m.status,
        "left_early": m.left_early,
    }


def eligible(rec: GameRecord) -> bool:
    return rec.rating.rd <= LB_MAX_RD or rec.matches >= LB_MIN_MATCHES


def ranked(snap: Snapshot, game_id: str,
           viewer: Optional[str] = None) -> list[tuple[str, GameRecord]]:
    """Visible, eligible players of a game, best first. Ties: lower RD,
    then more matches, then public id (stable across requests)."""
    rows = [(dc, rec) for dc, rec in snap.games.get(game_id, {}).items()
            if eligible(rec) and not privacy.is_hidden(dc, viewer)]
    rows.sort(key=lambda t: (-t[1].rating.rating, t[1].rating.rd,
                             -t[1].matches, privacy.public_id(t[0])))
    return rows


def leaderboard(snap: Snapshot, game_id: str, limit: int,
                viewer: Optional[str] = None) -> list[dict[str, Any]]:
    out = []
    for i, (dc, rec) in enumerate(ranked(snap, game_id, viewer)[:limit]):
        row = {"rank": i + 1}
        row.update(player_obj(snap, dc, viewer))
        row.update({
            "rating": _r1(rec.rating.rating),
            "rd": _r1(rec.rating.rd),
            "wins": rec.wins, "losses": rec.losses, "draws": rec.draws,
            "matches": rec.matches,
            "last_played": rec.last_played,
        })
        out.append(row)
    return out


def profile(snap: Snapshot, dc: str,
            viewer: Optional[str] = None) -> dict[str, Any]:
    base = player_obj(snap, dc, viewer)
    if base.get("private"):
        return base
    base["private"] = False
    games = []
    tw = tl = td = 0
    for gid, recs in snap.games.items():
        rec = recs.get(dc)
        if rec is None:
            continue
        rank = None
        for i, (other, _) in enumerate(ranked(snap, gid, viewer)):
            if other == dc:
                rank = i + 1
                break
        games.append({
            "game_id": gid,
            "family": family(gid),
            "rating": _r1(rec.rating.rating),
            "rd": _r1(rec.rating.rd),
            "provisional": not eligible(rec),
            "rank": rank,
            "wins": rec.wins, "losses": rec.losses, "draws": rec.draws,
            "matches": rec.matches,
            "last_played": rec.last_played,
            "top_characters": [{"char": c, "matches": n} for c, n in
                               rec.chars.most_common(PROFILE_TOP_CHARS)],
        })
        tw += rec.wins
        tl += rec.losses
        td += rec.draws
    games.sort(key=lambda g: (-g["matches"], g["game_id"]))
    recent = [match_obj(snap, m, viewer) for m in reversed(snap.matches)
              if dc in (m.p1, m.p2)][:PROFILE_RECENT]
    base.update({"totals": {"wins": tw, "losses": tl, "draws": td,
                            "matches": tw + tl + td},
                 "games": games, "recent": recent})
    return base


def games_list(snap: Snapshot,
               name_of: Callable[[str], str]) -> list[dict[str, Any]]:
    counts: dict[str, int] = {}
    last: dict[str, float] = {}
    for m in snap.matches:
        counts[m.game_id] = counts.get(m.game_id, 0) + 1
        last[m.game_id] = m.finished_at
    out = [{"game_id": gid, "family": family(gid),
            "name": name_of(gid) or gid, "matches": n,
            "players": len(snap.games.get(gid, {})),
            "last_played": last[gid]} for gid, n in counts.items()]
    out.sort(key=lambda g: (-g["matches"], g["game_id"]))
    return out


def recent(snap: Snapshot, game: Optional[str], limit: int,
           viewer: Optional[str] = None) -> list[dict[str, Any]]:
    out = []
    game = ratings.canonical_game_id(game) if game else game
    for m in reversed(snap.matches):
        if game and m.game_id != game:
            continue
        out.append(match_obj(snap, m, viewer))
        if len(out) >= limit:
            break
    return out


def legacy_matches(snap: Snapshot, limit: int, user_id: Optional[str],
                   game_id: Optional[str]) -> list[dict[str, Any]]:
    """/api/v1/matches rows: the pre-v1 field names, but ids are public ids
    (opted-out players still appear; names are never private)."""
    out = []
    for m in reversed(snap.matches):
        if game_id and game_id not in (m.game_id, m.source_game_id):
            continue
        p1, p2 = privacy.public_id(m.p1), privacy.public_id(m.p2)
        if user_id and user_id not in (p1, p2):
            continue
        out.append({
            "id": m.id, "game_id": m.source_game_id or m.game_id,  # the pre-v1 shape: the id as logged
            "p1_id": p1, "p1_nick": m.p1_nick,
            "p2_id": p2, "p2_nick": m.p2_nick,
            "p1_char_name": m.p1_char, "p2_char_name": m.p2_char,
            "stage_name": m.stage,
            "winner_id": {"p1": p1, "p2": p2}.get(m.winner, ""),
            "finished_at": m.finished_at,
        })
        if len(out) >= limit:
            break
    return out


def _not_found(what: str) -> JSONResponse:
    return JSONResponse({"error": "not_found", "what": what}, status_code=404)


def _with_alias_names(name_of: Callable[[str], str]) -> Callable[[str], str]:
    """Catalog ids from game_aliases.json name themselves; the registry still names raw ids."""
    return lambda gid: ratings.alias_name(gid) or name_of(gid)


def build_router(limiter: Any, matches_path: Callable[[], Path],
                 name_of: Callable[[str], str]) -> APIRouter:
    """Routes under /api/v1. `matches_path` is a callable so tests and the
    app can repoint the log without rebuilding the router."""
    name_of = _with_alias_names(name_of)
    router = APIRouter(prefix="/api/v1")

    def snap() -> Snapshot:
        return ratings.snapshot(matches_path())

    @router.get("/matches")
    @limiter.limit("60/minute")
    def api_matches(request: Request,
                    limit: int = Query(50, ge=1, le=500),
                    user_id: Optional[str] = Query(None, max_length=64),
                    game_id: Optional[str] = Query(None, max_length=128)):
        return {"matches": legacy_matches(snap(), limit, user_id, game_id)}

    @router.get("/games")
    @limiter.limit("60/minute")
    def api_games(request: Request):
        return {"games": games_list(snap(), name_of)}

    @router.get("/g/{game_id}/leaderboard")
    @limiter.limit("60/minute")
    def api_leaderboard(request: Request, game_id: str,
                        limit: int = Query(LB_DEFAULT_LIMIT, ge=1,
                                           le=LB_HARD_LIMIT)):
        s = snap()
        game_id = ratings.canonical_game_id(game_id)  # a legacy exe-stem id finds its catalog game
        if game_id not in s.games:
            return _not_found("game")
        return {"game_id": game_id, "family": family(game_id),
                "name": name_of(game_id) or game_id,
                "eligibility": {"max_rd": LB_MAX_RD,
                                "min_matches": LB_MIN_MATCHES},
                "players": leaderboard(s, game_id, limit)}

    @router.get("/u/{public_id}")
    @limiter.limit("60/minute")
    def api_user(request: Request, public_id: str):
        s = snap()
        dc = s.by_pid.get(public_id)
        if dc is None:
            return _not_found("user")
        return profile(s, dc)

    @router.get("/recent")
    @limiter.limit("60/minute")
    def api_recent(request: Request,
                   game: Optional[str] = Query(None, max_length=128),
                   limit: int = Query(RECENT_DEFAULT_LIMIT, ge=1,
                                      le=RECENT_HARD_LIMIT)):
        return {"matches": recent(snap(), game, limit)}

    return router
