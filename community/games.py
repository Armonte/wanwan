"""The per-game table (COMMUNITY_TICKET_SPEC.md section 7 "Per-game table"), imported from the TSV that fm2k-port
generates (`python3 tools/community/gen_game_table.py --format tsv > game_table.tsv`). A (game_id, build_id) with no row
never records a result. Columns by header name, so an added column (setup_layout) needs no change here.
"""
from __future__ import annotations

from typing import Optional

import receipts as R
from db import Database

REQUIRED = ("game_id", "build_id", "rounds_to_win_legal", "fps", "min_round_frames", "double_ko", "input_width",
            "max_match_frames")
ENGINE_FAMILY = {"fm2k": "fm2k", "fm95": "fm95", "2dfm95": "fm95", "cpw": "fm95"}


def _yes(v: str) -> int:
    return 1 if str(v).strip().lower() in ("1", "yes", "true", "y") else 0


def parse_tsv(text: str) -> list[dict]:
    """Rows of the generator's TSV. ValueError names the first bad line; a placeholder build id (`*`) is refused."""
    lines = [ln for ln in text.splitlines() if ln.strip() and not ln.startswith("#")]
    if not lines:
        raise ValueError("empty table")
    head = [h.strip() for h in lines[0].split("\t")]
    missing = [c for c in REQUIRED if c not in head]
    if missing:
        raise ValueError("missing columns: " + ", ".join(missing))
    out = []
    for n, ln in enumerate(lines[1:], 2):
        f = dict(zip(head, (x.strip() for x in ln.split("\t"))))
        try:
            if not f["game_id"] or not f["build_id"] or f["build_id"] == "*":
                raise ValueError("game_id and a real build_id are required")
            fam = f.get("family") or ENGINE_FAMILY.get(f.get("engine", "").lower(), "")
            if not fam:
                raise ValueError(f"unknown engine {f.get('engine')!r} (no family)")
            row = dict(game_id=f["game_id"], build_id=f["build_id"], family=fam, name=f.get("name") or None,
                       rounds_to_win_legal=f["rounds_to_win_legal"], fps=int(f["fps"]),
                       min_round_frames=int(f["min_round_frames"]), double_ko=_yes(f["double_ko"]),
                       input_width=int(f["input_width"]), max_match_frames=int(f["max_match_frames"]),
                       worker=_yes(f.get("worker", "no")), setup_layout=f.get("setup_layout", ""),
                       confirmed=_yes(f.get("confirmed", "no")))
            if not R.rules_valid(rules_of(row)):
                raise ValueError("an unusable row")
        except (KeyError, ValueError) as e:
            raise ValueError(f"line {n}: {e}")
        out.append(row)
    return out


def import_rows(db: Database, rows: list[dict], now: int, replace_all: bool = False) -> int:
    """Upsert the rows (one transaction). `replace_all` first drops rows absent from this table (a retired build stops
    recording; its recorded matches stay)."""
    cols = ("game_id", "build_id", "family", "name", "rounds_to_win_legal", "fps", "min_round_frames", "double_ko",
            "input_width", "max_match_frames", "worker", "setup_layout", "confirmed")
    with db.tx() as d:
        if replace_all:
            d.x("DELETE FROM games")
        for r in rows:
            d.x(f"INSERT OR REPLACE INTO games({', '.join(cols)}, imported) VALUES({', '.join('?' * len(cols))}, ?)",
                tuple(r[c] for c in cols) + (now,))
        d.audit(now, "import_games", f"{len(rows)} rows{' (replace)' if replace_all else ''}")
    return len(rows)


def rules_of(row) -> R.GameRules:
    try:
        mask = R.legal_mask(row["rounds_to_win_legal"])
    except ValueError:
        mask = 0
    return R.GameRules(rounds_to_win_legal=mask, fps=int(row["fps"]), min_round_frames=int(row["min_round_frames"]),
                       double_ko=bool(row["double_ko"]), input_width=int(row["input_width"]),
                       max_match_frames=int(row["max_match_frames"]))


def lookup(db: Database, game_id: str, build_id: str) -> Optional[dict]:
    r = db.one("SELECT * FROM games WHERE game_id = ? AND build_id = ?", (game_id, build_id))
    return dict(r) if r else None


def max_report_body(db: Database) -> int:
    """The largest report body any row allows (checked from Content-Length before a byte is read); the row's own cap is
    checked again once the body names its game. The default row's cap while the table is empty."""
    best = 0
    for r in db.q("SELECT * FROM games"):
        best = max(best, R.report_body_cap(rules_of(r)))
    return best or R.report_body_cap(R.GameRules())
