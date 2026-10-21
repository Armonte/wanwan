"""POST /v1/report behind HTTP: verify a signed receipt list (COMMUNITY_TICKET_SPEC.md sections 7 and 7c), store the
receipts per match key, apply the counting decision (counting.py) with its timing, and append the state to the
results export the stats site reads (STATS_DESIGN.md 4.4).

Timing (spec section 7 and the owner's 2026-10-21 rulings):
- a counted match is `pending` for 24 h (rated at once), then `final` (settle(): lazily on every report, and from
  `admin.py settle` on a timer);
- after the pending window a contradicting report never flips the W/L: on a game without a replay worker it adds a
  `late_claim` mark and the late-claim flag; on a game with one the match becomes `disputed`;
- a dispute with no verdict closes `void` 7 days after the match; any report later than 7 days is refused;
- at most 20 counted matches per pair per UTC day are rated (the rest carry the pair-cap flag);
- at most 3 left-early outcomes filed per reporter per UTC day (429 cap_daily, retried the next day);
- at most REPORTS_PER_DAY new report bodies per account per UTC day (429 cap_daily). A re-sent body is a no-op.

The export never carries a Discord id, an account id, a session key, a signature or an IP: public id, name, numeric
tag and the hidden flag only.
"""
from __future__ import annotations

import json
import os
import secrets
from typing import Optional

import counting as K
import games as G
import receipts as R
import tickets as T
from service import Community, ServiceError

DAY = 86400
PENDING_SECONDS = DAY
CLOSE_SECONDS = 7 * DAY
PAIR_CAP = 20
LEFT_EARLY_CAP = 3
REPORTS_PER_DAY = 300
MARK_KINDS = ("strike", "left_early", "left_early_behind", "disputed")
FLAG_NAMES = ((K.FLAG_DESYNC, "desync"), (K.FLAG_LATE, "late_claim"), (K.FLAG_PAIR_CAP, "pair_cap"))


def _day_start(now: int) -> int:
    return now - now % DAY


def _cap(detail: str, now: int):
    e = ServiceError(429, "cap_daily", detail)
    e.headers = {"Retry-After": str(max(60, _day_start(now) + DAY - now))}
    return e


class Results:
    def __init__(self, community: Community):
        self.c = community
        self.db = community.db

    # ---- the report ----
    def report(self, acct: int, client_pub: bytes, body: bytes) -> dict:
        now = self.c.now()
        self.settle(now)
        sha = T.sha256(body)
        seen = self.db.one("SELECT match FROM reports WHERE account = ? AND body_sha = ?", (acct, sha))
        if seen is not None:  # F11: a re-sent report touches nothing
            m = self.db.one("SELECT match_id, status FROM matches WHERE id = ?", (seen[0],))
            return {"status": m["status"], "match_id": m["match_id"]}
        n_today = self.db.one("SELECT COUNT(*) FROM reports WHERE account = ? AND ts >= ?", (acct, _day_start(now)))[0]
        if n_today >= REPORTS_PER_DAY:
            raise _cap("too many reports today", now)

        why, items = R.parse_list(body, R.LIST_STRUCTURAL_MAX)
        if why != R.OK:
            raise ServiceError(400, "bad_receipt", f"the receipt list: {why}")
        first = items[0].receipt
        if not all(R.same_match(first, x.receipt) for x in items):
            raise ServiceError(400, "bad_receipt", "receipts of more than one match, game or build")
        try:
            game_id, build_id = first.game_id.decode("ascii"), first.build_id.decode("ascii")
        except UnicodeDecodeError:
            raise ServiceError(422, "unknown_game", "game_id / build_id are not ASCII")
        row = G.lookup(self.db, game_id, build_id)
        if row is None:
            raise ServiceError(422, "unknown_game", "no game table row for this game and build")
        rules = G.rules_of(row)
        if len(body) > R.report_body_cap(rules):
            raise ServiceError(413, "body_too_large", "the report is above this game's cap")
        why, items = R.parse_report_body(body, rules)
        if why != R.OK:
            raise ServiceError(400, "bad_receipt", f"the receipt list: {why}")
        if any(not x.receipt.has_evidence for x in items):
            raise ServiceError(422, "no_evidence", "a zero state_hash never counts")
        if now > first.started_unix + CLOSE_SECONDS:
            raise ServiceError(409, "too_late", "reports for this match closed 7 days after it started")
        for x in items:
            if not R.frame_fits_wall_time(x.receipt, now, rules):
                raise ServiceError(400, "bad_receipt", "a receipt's frame does not fit the time since started_unix")

        match = self.db.one("SELECT * FROM matches WHERE session_key = ? AND match_index = ?",
                            (first.session_key, first.match_index))
        if match is not None:
            if acct not in (match["acct0"], match["acct1"]):
                raise ServiceError(403, "not_participant", "not a player of this match")
            if match["status"] == "void" and match["settled_at"] is not None:
                raise ServiceError(409, "closed", "this match closed")
            pubs = (match["pub0"], match["pub1"])
            accts = (match["acct0"], match["acct1"])
        else:
            accts, pubs = self._participants(acct, client_pub, items)
        for x in items:
            why, _ = R.verify_receipt(x.bytes, first.session_key, pubs, rules)
            if why != R.OK:
                raise ServiceError(400, "bad_receipt", f"a receipt does not verify: {why}")
        mid = self._apply(acct, accts, pubs, first, game_id, build_id, row, rules, items, sha, now)
        self.export_match(mid)
        m = self.db.one("SELECT match_id, status FROM matches WHERE id = ?", (mid,))
        return {"status": m["status"], "match_id": m["match_id"]}

    def _participants(self, acct: int, client_pub: bytes, items: list) -> tuple[tuple, tuple]:
        """A new match key: the reporter's slot from its own key, the opponent from the accounts that held a ticket at
        started_unix (receipts carry no ticket, so the key that verifies the opponent's started receipt names it)."""
        by = {s: [x for x in items if x.receipt.signer_slot == s] for s in (0, 1)}
        mine = [s for s in (0, 1) if by[s] and all(R.signature_ok(x.bytes, client_pub) for x in by[s])]
        if len(mine) == 2:
            raise ServiceError(403, "self_play", "both slots are signed by one player")
        if not mine:
            raise ServiceError(403, "not_participant", "no receipt of this report is signed by the caller")
        if not all(any(x.receipt.kind == R.STARTED for x in by[s]) for s in (0, 1)):
            raise ServiceError(422, "no_started_pair", "without both started receipts nothing is recorded")
        me, opp = mine[0], 1 - mine[0]
        started = next(x for x in by[opp] if x.receipt.kind == R.STARTED)
        t0 = started.receipt.started_unix
        cands = self.db.q(
            "SELECT DISTINCT a.id, a.client_pub, a.banned FROM tickets t JOIN accounts a ON a.id = t.account "
            "WHERE t.expires + ? >= ? AND t.issued <= ? AND a.client_pub IS NOT NULL",
            (T.SKEW, t0, t0 + T.SKEW))
        found = next((c for c in cands if R.signature_ok(started.bytes, c["client_pub"])), None)
        if found is None or found["banned"]:
            raise ServiceError(403, "anonymous_peer", "the opponent is not a linked player")
        if found["id"] == acct:
            raise ServiceError(403, "self_play", "both slots are one account")
        accts = [0, 0]
        pubs = [b"", b""]
        accts[me], pubs[me] = acct, client_pub
        accts[opp], pubs[opp] = found["id"], found["client_pub"]
        return tuple(accts), tuple(pubs)

    def _apply(self, acct, accts, pubs, first, game_id, build_id, row, rules, items, sha, now) -> int:
        worker = bool(row["worker"])
        with self.db.tx() as d:
            m = d.one("SELECT * FROM matches WHERE session_key = ? AND match_index = ?",
                      (first.session_key, first.match_index))
            if m is None:
                cur = d.x("INSERT INTO matches(match_id, session_key, match_index, game_id, build_id, acct0, acct1, pub0, "
                          "pub1, started_unix, rounds_to_win, status, first_report, updated) "
                          "VALUES(?,?,?,?,?,?,?,?,?,?,?, 'open', ?, ?)",
                          (secrets.token_hex(6), first.session_key, first.match_index, game_id, build_id, accts[0],
                           accts[1], pubs[0], pubs[1], first.started_unix, first.rounds_to_win, now, now))
                m = d.one("SELECT * FROM matches WHERE id = ?", (cur.lastrowid,))
            mid = m["id"]
            for x in items:
                r = x.receipt
                d.x("INSERT OR IGNORE INTO receipts(match, sha, signer_slot, kind, frame, bytes, received) "
                    "VALUES(?,?,?,?,?,?,?)", (mid, T.sha256(x.bytes), r.signer_slot, r.kind, r.frame, x.bytes, now))
            stored = [R.parse_receipt(b[0])[1] for b in d.q("SELECT bytes FROM receipts WHERE match = ?", (mid,))]
            out = K.evaluate(stored, rules, worker)
            prev = K.Outcome(m["status"], m["winner_slot"],
                             None if m["rounds0"] is None else (m["rounds0"], m["rounds1"]), m["decided_frame"],
                             m["flags"], m["left_slot"])
            late = m["status"] in K.COUNTED and m["counted_at"] is not None and now >= m["counted_at"] + PENDING_SECONDS
            if late and out.key() != prev.key():
                if worker:
                    out = K.Outcome("disputed", flags=m["flags"], marks=[(0, "disputed"), (1, "disputed")])
                    self._set(d, m, out, now)
                    self._marks(d, mid, accts, out.marks, now, replace=False)
                else:
                    d.x("UPDATE matches SET flags = flags | ?, updated = ? WHERE id = ?", (K.FLAG_LATE, now, mid))
                    d.x("INSERT OR IGNORE INTO marks(account, match, kind, ts) VALUES(?,?,?,?)",
                        (acct, mid, "late_claim", now))
                    self._marks(d, mid, accts, [mk for mk in out.marks if mk[1] == "strike"], now, replace=False)
                    d.audit(now, "late_claim", f"match {m['match_id']}")
            else:
                if out.status == "left_early" and m["status"] != "left_early":
                    stayer = accts[out.winner_slot]
                    n = d.one("SELECT COUNT(*) FROM matches WHERE status = 'left_early' AND counted_at >= ? AND "
                              "((acct0 = ? AND winner_slot = 0) OR (acct1 = ? AND winner_slot = 1))",
                              (_day_start(now), stayer, stayer))[0]
                    if n >= LEFT_EARLY_CAP:
                        raise _cap("at most 3 left-early results per day", now)
                out.flags |= m["flags"] & (K.FLAG_LATE | K.FLAG_PAIR_CAP)
                if out.status in K.COUNTED and m["counted_at"] is None:
                    a, b = accts
                    n = d.one("SELECT COUNT(*) FROM matches WHERE counted_at >= ? AND flags & ? = 0 AND "
                              "status IN ('pending', 'final', 'left_early') AND "
                              "((acct0 = ? AND acct1 = ?) OR (acct0 = ? AND acct1 = ?))",
                              (_day_start(now), K.FLAG_PAIR_CAP, a, b, b, a))[0]
                    if n >= PAIR_CAP:
                        out.flags |= K.FLAG_PAIR_CAP
                self._set(d, m, out, now)
                self._marks(d, mid, accts, out.marks, now, replace=True)
            d.x("INSERT OR IGNORE INTO reports(account, body_sha, match, ts) VALUES(?,?,?,?)", (acct, sha, mid, now))
        return mid

    @staticmethod
    def _set(d, m, out: K.Outcome, now: int) -> None:
        counted_at = m["counted_at"] or (now if out.status in K.COUNTED else None)
        status = out.status
        if status == "pending" and m["status"] == "final" and out.key() == K.Outcome(
                "final", m["winner_slot"], (m["rounds0"], m["rounds1"]), left_slot=m["left_slot"]).key():
            status = "final"  # the same result again: it stays settled
        rounds = out.rounds or (None, None)
        d.x("UPDATE matches SET status = ?, winner_slot = ?, rounds0 = ?, rounds1 = ?, decided_frame = ?, flags = ?, "
            "left_slot = ?, counted_at = ?, updated = ? WHERE id = ?",
            (status, out.winner_slot, rounds[0], rounds[1], out.frame, out.flags, out.left_slot, counted_at, now,
             m["id"]))

    @staticmethod
    def _marks(d, mid: int, accts, marks: list, now: int, replace: bool) -> None:
        if replace:
            d.x(f"DELETE FROM marks WHERE match = ? AND kind IN ({','.join('?' * len(MARK_KINDS))})",
                (mid,) + MARK_KINDS)
        for slot, kind in marks:
            d.x("INSERT OR IGNORE INTO marks(account, match, kind, ts) VALUES(?,?,?,?)", (accts[slot], mid, kind, now))

    # ---- timing ----
    def settle(self, now: Optional[int] = None) -> int:
        """pending -> final after 24 h; an open dispute closes void 7 days after the match. Returns rows changed."""
        now = self.c.now() if now is None else now
        ids = []
        with self.db.tx() as d:
            for r in d.q("SELECT id FROM matches WHERE status = 'pending' AND counted_at <= ?", (now - PENDING_SECONDS,)):
                d.x("UPDATE matches SET status = 'final', settled_at = ?, updated = ? WHERE id = ?", (now, now, r[0]))
                ids.append(r[0])
            for r in d.q("SELECT id FROM matches WHERE status = 'disputed' AND started_unix <= ?",
                         (now - CLOSE_SECONDS,)):
                d.x("UPDATE matches SET status = 'void', settled_at = ?, updated = ? WHERE id = ?", (now, now, r[0]))
                ids.append(r[0])
        for i in ids:
            self.export_match(i)
        return len(ids)

    # ---- the export (STATS_DESIGN.md 4.4) ----
    def export_path(self) -> str:
        return os.path.join(self.c.cfg.export_dir, "results.jsonl")

    def export_row(self, mid: int) -> Optional[dict]:
        m = self.db.one("SELECT * FROM matches WHERE id = ?", (mid,))
        if m is None or m["status"] == "open":
            return None
        g = self.db.one("SELECT family, fps FROM games WHERE game_id = ? AND build_id = ?", (m["game_id"], m["build_id"]))
        family = g["family"] if g else "unknown"
        fps = g["fps"] if g else 100
        gid = m["game_id"] if ":" in m["game_id"] else f"{family}:{m['game_id']}"

        def player(a):
            acc = self.db.one("SELECT public_id, name, tag, stats_hidden FROM accounts WHERE id = ?", (a,))
            return {"public_id": acc["public_id"], "name": acc["name"],
                    "tag": None if acc["tag"] is None else f"{acc['tag']:04d}", "hidden": bool(acc["stats_hidden"]),
                    "char": None}
        winner = {0: "p1", 1: "p2", R.WINNER_DRAW: "draw"}.get(m["winner_slot"])
        finished = None if m["decided_frame"] is None else m["started_unix"] + -(-m["decided_frame"] // fps)
        return {"v": 1, "match_id": m["match_id"], "game_id": gid, "family": family, "build": m["build_id"],
                "status": m["status"], "started_at": m["started_unix"], "finished_at": finished,
                "rounds_to_win": m["rounds_to_win"], "p1": player(m["acct0"]), "p2": player(m["acct1"]),
                "winner": winner, "score": None if m["rounds0"] is None else [m["rounds0"], m["rounds1"]],
                "left_early": {0: "p1", 1: "p2"}.get(m["left_slot"]),
                "flags": [name for bit, name in FLAG_NAMES if m["flags"] & bit], "replay_url": self.replay_url(mid)}

    def replay_url(self, mid: int) -> Optional[str]:
        """The stored replay's public URL, composed from the key and COMMUNITY_REPLAY_PUBLIC_BASE now (never stored)."""
        base = self.c.cfg.replay_public_base
        r = self.db.one("SELECT key FROM replays WHERE match = ? AND status = 'stored'", (mid,))
        return base.rstrip("/") + "/" + r[0] if r and base else None

    def export_match(self, mid: int, force: bool = False) -> bool:
        """Append the match's state when it changed since the last line (fsync per line)."""
        row = self.export_row(mid)
        if row is None:
            return False
        line = json.dumps(row, sort_keys=True, separators=(",", ":"))
        with self.db.lock:
            prev = self.db.one("SELECT exported FROM matches WHERE id = ?", (mid,))[0]
            if prev == line and not force:
                return False
            os.makedirs(self.c.cfg.export_dir, exist_ok=True)
            with open(self.export_path(), "a", encoding="utf-8") as f:
                f.write(line + "\n")
                f.flush()
                os.fsync(f.fileno())
            self.db.x("UPDATE matches SET exported = ? WHERE id = ?", (line, mid))
        return True

    def reexport(self) -> int:
        """One line per recorded match with its current state (after a secret or name change)."""
        return sum(self.export_match(r[0], force=True) for r in self.db.q("SELECT id FROM matches ORDER BY id"))

    def stats(self) -> dict:
        out = {r[0]: r[1] for r in self.db.q("SELECT status, COUNT(*) FROM matches GROUP BY status")}
        out["games"] = self.db.one("SELECT COUNT(*) FROM games")[0]
        out["replays"], out["replay_bytes"] = self.db.one(
            "SELECT COUNT(*), COALESCE(SUM(bytes), 0) FROM replays WHERE status = 'stored'")
        return out
