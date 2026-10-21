"""Match log -> per-game Glicko-2 ratings + records for the JSON API.

Reads the hub's matches.json (format: hub/match_records.py commit_match)
and builds one immutable snapshot, cached by the file's (mtime_ns, size)
so a request only pays for a recompute after the hub commits a match.

What counts (owner decision 2026-10-20: only Discord-linked matches):
  - Both p1_id and p2_id must be Discord snowflakes (15-21 digits). Guest
    ids ("guest:..."), pre-auth nick ids and empty ids are excluded from
    ratings, records and every API result. The hub already strips the dev
    suffix and refuses guest / self-vs-self commits; we re-check both.
  - Self-vs-self (same id both sides) is dropped.
  - Both-disconnect records (report_p1 == report_p2 == "disconnect") are
    written by the hub as winner_id "" but are a no-contest, not a draw
    (match_records.reconcile_outcomes says "there was no actual match").
    They are dropped.
  - A one-sided disconnect is already a win for the survivor in winner_id
    (reconcile_outcomes maps "disconnect" to the other side). We follow it
    and mark the match `disconnect: true`.
  - winner_id "" (a real draw) scores 0.5 for both.

Timeout repair: before the netplay_battle_end.cpp fix the FM2K hook
decided the match winner from final HP alone, so a match whose last round
ended on time-over (both HP > 0) was reported as a DRAW even though the
engine had credited the rounds. Schema-2 records carry rounds[], whose
per-round winner comes from the engine's own result_kind (round_events.cpp)
and is not affected by that bug. A draw whose rounds[] tally is unequal is
therefore relabeled as a win for the side with more round wins and marked
`repaired: true`. Draws without rounds[] (schema 1) cannot be repaired and
stay draws.

Community results (the PovertyCaster service's export/results.jsonl, at FM2K_RESULTS_PATH): one line per match state
change, latest line per match_id wins. Rated: status pending, final and left_early (owner ruling 2026-10-21: the leaver
loses, the stayer wins), unless flagged pair_cap. Players there carry a public id, never a Discord id: a public id that
equals public_id(<legacy Discord id>) joins that legacy player, any other is keyed "pid:<public id>" (privacy.PID_KEY).

Game families: one log holds every engine. Ids look like "fm2k:<id>",
"fm95:<id>" or a bare family name like "mbaacc"; legacy FM2K entries keep
their bare exe-stem ids and map to "fm2k". See game_family().
"""

from __future__ import annotations

import json
import re
import threading
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

import glicko2
import privacy

DISCORD_ID_RE = re.compile(r"^[0-9]{15,21}$")
PUBLIC_ID_RE = re.compile(r"^[0-9a-f]{16}$")
RESULT_RATED = ("pending", "final", "left_early")
KNOWN_FAMILIES = ("fm2k", "fm95", "mbaacc")


def game_family(game_id: str, engine_hint: Optional[str] = None) -> str:
    """Family of a game id. "fam:<id>" -> fam; a bare known family name ->
    itself; anything else is a legacy FM2K exe-stem id unless the registry
    says its engine is fm95."""
    if not game_id:
        return "unknown"
    if ":" in game_id:
        return game_id.split(":", 1)[0].lower() or "unknown"
    low = game_id.lower()
    if low in KNOWN_FAMILIES:
        return low
    if engine_hint and engine_hint.lower() in ("fm95", "2dfm95", "cpw"):
        return "fm95"
    return "fm2k"


# Legacy hub ids (exe stems) -> the launcher's catalog ids ("fm2k:<catalog id>"), so the launcher's Statistics tab
# finds the hub history and builds of one game share one rating. game_aliases.json, or FM2K_GAME_ALIASES.
ALIASES_PATH = Path(__import__("os").environ.get("FM2K_GAME_ALIASES",
                                                  str(Path(__file__).resolve().parent / "game_aliases.json")))
_alias_cache: dict[str, Any] = {"key": None, "aliases": {}, "names": {}}


def _aliases() -> dict[str, Any]:
    try:
        st = ALIASES_PATH.stat()
        key = (st.st_mtime_ns, st.st_size)
    except OSError:
        return _alias_cache
    if _alias_cache["key"] != key:
        try:
            d = json.loads(ALIASES_PATH.read_text(encoding="utf-8"))
            _alias_cache.update(key=key, aliases=dict(d.get("aliases") or {}), names=dict(d.get("names") or {}))
        except (OSError, ValueError):
            pass
    return _alias_cache


def canonical_game_id(game_id: str) -> str:
    """The id a match is rated under: the alias target when the raw id has one, else the raw id."""
    return _aliases()["aliases"].get(game_id, game_id)


def alias_name(game_id: str) -> Optional[str]:
    return _aliases()["names"].get(game_id)


def raw_ids_of(game_id: str) -> list[str]:
    """Every raw id that is rated under this canonical id (for the HTML pages, which work on raw ids)."""
    return [raw for raw, canon in _aliases()["aliases"].items() if canon == game_id]


def is_linked(uid: Any) -> bool:
    return isinstance(uid, str) and bool(DISCORD_ID_RE.match(uid))


def _round_tally(rounds: Any) -> Optional[tuple[int, int]]:
    if not isinstance(rounds, list) or not rounds:
        return None
    p1 = p2 = 0
    for r in rounds[:16]:
        if not isinstance(r, dict):
            continue
        if r.get("winner") == "p1":
            p1 += 1
        elif r.get("winner") == "p2":
            p2 += 1
    return (p1, p2)


def _char(name: Any, cid: Any) -> Optional[str]:
    if isinstance(name, str) and name:
        return name
    if isinstance(cid, int) and not isinstance(cid, bool):
        return f"id {cid}"
    return None


@dataclass
class Match:
    id: str
    game_id: str
    p1: str                 # Discord id (internal only, never serialized)
    p2: str
    p1_nick: str
    p2_nick: str
    p1_char: Optional[str]
    p2_char: Optional[str]
    winner: str             # "p1" | "p2" | "draw"
    score: Optional[tuple[int, int]]
    finished_at: float
    disconnect: bool = False
    repaired: bool = False
    replay_url: Optional[str] = None
    stage: Optional[str] = None
    source_game_id: str = ""  # the id as logged (before game_aliases.json)
    status: str = "final"     # community results: pending | final | left_early
    left_early: Optional[str] = None  # "p1" | "p2": who left (community results)
    p1_tag: Optional[str] = None      # the community numeric tag (results rows)
    p2_tag: Optional[str] = None
    p1_hidden: bool = False
    p2_hidden: bool = False


@dataclass
class GameRecord:
    rating: glicko2.Rating = field(default_factory=glicko2.Rating)
    wins: int = 0
    losses: int = 0
    draws: int = 0
    last_played: float = 0.0
    chars: Counter = field(default_factory=Counter)

    @property
    def matches(self) -> int:
        return self.wins + self.losses + self.draws


@dataclass
class Snapshot:
    matches: list[Match]                          # oldest first
    games: dict[str, dict[str, GameRecord]]       # game -> dc id -> record
    nicks: dict[str, str]                         # dc id -> latest nick
    by_pid: dict[str, str]                        # public id -> dc id (or "pid:<public id>")
    excluded: dict[str, int]
    tags: dict[str, str] = field(default_factory=dict)  # player key -> community numeric tag


def _result_player(p: Any) -> Optional[tuple[str, str, Optional[str], bool]]:
    if not isinstance(p, dict):
        return None
    pid = p.get("public_id")
    if not isinstance(pid, str) or not PUBLIC_ID_RE.match(pid):
        return None
    tag = p.get("tag")
    return (privacy.PID_KEY + pid, str(p.get("name") or "anon"), tag if isinstance(tag, str) and tag else None,
            bool(p.get("hidden")))


def parse_result(r: dict) -> tuple[Optional[Match], str]:
    """A community results.jsonl row (v1). Players come back keyed "pid:<public id>"; build_snapshot joins them."""
    status = r.get("status")
    if status not in RESULT_RATED:
        return None, f"status_{status}" if isinstance(status, str) else "malformed"
    if "pair_cap" in (r.get("flags") or []):
        return None, "pair_cap"
    a, b = _result_player(r.get("p1")), _result_player(r.get("p2"))
    if a is None or b is None:
        return None, "unlinked"
    if a[0] == b[0]:
        return None, "self"
    winner = r.get("winner")
    ts = r.get("finished_at") or r.get("started_at")
    gid = r.get("game_id")
    if winner not in ("p1", "p2", "draw") or not isinstance(ts, (int, float)) or not isinstance(gid, str) or not gid:
        return None, "malformed"
    score = r.get("score")
    score = (int(score[0]), int(score[1])) if isinstance(score, list) and len(score) == 2 else None
    replay = r.get("replay_url")
    left = r.get("left_early") if r.get("left_early") in ("p1", "p2") else None
    return Match(
        id=str(r["match_id"]), game_id=canonical_game_id(gid), source_game_id=gid,
        p1=a[0], p2=b[0], p1_nick=a[1], p2_nick=b[1],
        p1_char=r["p1"].get("char") or None, p2_char=r["p2"].get("char") or None,
        winner=winner, score=score, finished_at=float(ts), disconnect=left is not None,
        replay_url=replay if isinstance(replay, str) and replay else None,
        status=status, left_early=left, p1_tag=a[2], p2_tag=b[2], p1_hidden=a[3], p2_hidden=b[3],
    ), ""


def parse_record(r: Any) -> tuple[Optional[Match], str]:
    """One raw log entry -> (Match, "") or (None, reason). A community results row (v 1, match_id) is parse_result."""
    if isinstance(r, dict) and r.get("v") == 1 and r.get("match_id"):
        return parse_result(r)
    if not isinstance(r, dict) or not r.get("id"):
        return None, "malformed"
    p1, p2 = r.get("p1_id"), r.get("p2_id")
    if not (is_linked(p1) and is_linked(p2)):
        return None, "unlinked"
    if p1 == p2:
        return None, "self"
    rep1, rep2 = r.get("report_p1"), r.get("report_p2")
    if rep1 == "disconnect" and rep2 == "disconnect":
        return None, "no_contest"
    ts = r.get("finished_at", r.get("started_at"))
    if not isinstance(ts, (int, float)):
        return None, "malformed"
    w = r.get("winner_id", "") or ""
    if w == p1:
        winner = "p1"
    elif w == p2:
        winner = "p2"
    elif w == "":
        winner = "draw"
    else:
        return None, "malformed"
    score = _round_tally(r.get("rounds")) if r.get("schema") == 2 else None
    repaired = False
    if winner == "draw" and score and score[0] != score[1]:
        winner = "p1" if score[0] > score[1] else "p2"
        repaired = True
    replay = r.get("replay_url")
    stage = r.get("stage_name")
    return Match(
        id=str(r["id"]), game_id=canonical_game_id(str(r.get("game_id") or "")),
        source_game_id=str(r.get("game_id") or ""),
        p1=p1, p2=p2,
        p1_nick=str(r.get("p1_nick") or "anon"),
        p2_nick=str(r.get("p2_nick") or "anon"),
        p1_char=_char(r.get("p1_char_name"), r.get("p1_char_id")),
        p2_char=_char(r.get("p2_char_name"), r.get("p2_char_id")),
        winner=winner, score=score, finished_at=float(ts),
        disconnect="disconnect" in (rep1, rep2),
        repaired=repaired,
        replay_url=replay if isinstance(replay, str) and replay else None,
        stage=stage if isinstance(stage, str) and stage else None,
    ), ""


def build_snapshot(raw: list[Any], results: Optional[list[Any]] = None) -> Snapshot:
    """`raw`: the legacy hub log; `results`: community results rows (latest per match_id, read_results)."""
    excluded: Counter = Counter()
    matches: list[Match] = []
    for r in list(raw) + list(results or []):
        m, why = parse_record(r)
        if m is None:
            excluded[why] += 1
        else:
            matches.append(m)
    # the join: a results player whose public id is a legacy player's is that player
    legacy = {privacy.public_id(k): k for m in matches if not m.p1.startswith(privacy.PID_KEY) for k in (m.p1, m.p2)}
    hidden: dict[str, bool] = {}
    tags: dict[str, str] = {}
    for m in matches:
        if m.p1.startswith(privacy.PID_KEY):
            m.p1 = legacy.get(privacy.public_id(m.p1), m.p1)
            m.p2 = legacy.get(privacy.public_id(m.p2), m.p2)
            hidden[privacy.public_id(m.p1)] = m.p1_hidden  # file order: the latest row wins
            hidden[privacy.public_id(m.p2)] = m.p2_hidden
    privacy.set_row_hidden(pid for pid, h in hidden.items() if h)
    matches.sort(key=lambda m: m.finished_at)  # stable: log order breaks ties
    excluded["repaired"] = sum(1 for m in matches if m.repaired)

    games: dict[str, dict[str, GameRecord]] = {}
    nicks: dict[str, str] = {}
    for m in matches:
        g = games.setdefault(m.game_id, {})
        a = g.setdefault(m.p1, GameRecord())
        b = g.setdefault(m.p2, GameRecord())
        s = 1.0 if m.winner == "p1" else 0.0 if m.winner == "p2" else 0.5
        a.rating, b.rating = glicko2.rate_match(a.rating, b.rating, s)
        if s == 1.0:
            a.wins += 1
            b.losses += 1
        elif s == 0.0:
            a.losses += 1
            b.wins += 1
        else:
            a.draws += 1
            b.draws += 1
        a.last_played = b.last_played = m.finished_at
        if m.p1_char:
            a.chars[m.p1_char] += 1
        if m.p2_char:
            b.chars[m.p2_char] += 1
        nicks[m.p1] = m.p1_nick
        nicks[m.p2] = m.p2_nick
        for k, t in ((m.p1, m.p1_tag), (m.p2, m.p2_tag)):
            if t:
                tags[k] = t  # the community #tag belongs to the account: a later legacy row keeps it
    by_pid = {privacy.public_id(dc): dc for dc in nicks}
    return Snapshot(matches, games, nicks, by_pid, dict(excluded), tags)


def read_log(path: Path) -> list[Any]:
    try:
        with path.open("r", encoding="utf-8") as f:
            payload = json.load(f)
    except (OSError, json.JSONDecodeError):
        return []
    if isinstance(payload, dict):
        return payload.get("matches", []) or []
    return payload if isinstance(payload, list) else []


def results_path() -> Optional[Path]:
    """The community service's export/results.jsonl (FM2K_RESULTS_PATH), read beside matches.json. None when unset."""
    p = __import__("os").environ.get("FM2K_RESULTS_PATH", "")
    return Path(p) if p else None


def read_results(path: Optional[Path]) -> list[Any]:
    """Every v1 line, the latest per match_id (in file order). A torn last line (a write in progress) is skipped."""
    if path is None:
        return []
    latest: dict[str, Any] = {}
    try:
        with path.open("r", encoding="utf-8") as f:
            for line in f:
                try:
                    r = json.loads(line)
                except ValueError:
                    continue
                if isinstance(r, dict) and r.get("v") == 1 and isinstance(r.get("match_id"), str):
                    latest.pop(r["match_id"], None)
                    latest[r["match_id"]] = r  # re-insert: the dict order is the latest line's order
    except OSError:
        return []
    return list(latest.values())


def _stat_key(path: Optional[Path]) -> tuple:
    if path is None:
        return (None,)
    try:
        st = path.stat()
        return (str(path), st.st_mtime_ns, st.st_size)
    except OSError:
        return (str(path), -1, -1)


_cache_lock = threading.Lock()
_cache: dict[str, Any] = {"key": None, "snap": None}


def snapshot(path: Path, results: Optional[Path] = None) -> Snapshot:
    """Cached snapshot of the log at `path` merged with the community results (`results`, default FM2K_RESULTS_PATH),
    keyed by both files' (mtime, size)."""
    results = results if results is not None else results_path()
    key = (_stat_key(path), _stat_key(results), _aliases()["key"])
    with _cache_lock:
        if _cache["key"] == key and _cache["snap"] is not None:
            return _cache["snap"]
    snap = build_snapshot(read_log(path), read_results(results))
    with _cache_lock:
        _cache["key"] = key
        _cache["snap"] = snap
    return snap


def clear_cache() -> None:
    with _cache_lock:
        _cache["key"] = None
        _cache["snap"] = None
