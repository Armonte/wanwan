"""SQLite storage for the community service, with migrations-lite: PRAGMA user_version holds the schema version and
MIGRATIONS[i] takes the database from version i to i + 1. Add a migration at the end; never edit a shipped one.

Discord ids live only in accounts.discord_id; accounts.discord_handle goes back only to its own player. Nothing here stores IP addresses.
"""
from __future__ import annotations

import sqlite3
import threading

MIGRATIONS = [
    # 0 -> 1: accounts, issued tickets, pending links, call nonces, deny list state, admin audit log
    """
    CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
    CREATE TABLE accounts (
        id            INTEGER PRIMARY KEY,
        account_id    BLOB NOT NULL UNIQUE,          -- 16 random bytes, never derived from Discord
        discord_id    TEXT NOT NULL UNIQUE,          -- server-side only, never in a response
        name          TEXT,                          -- NFC, section 10 rules; NULL until chosen
        skeleton      TEXT,
        tag           INTEGER,
        flagged       INTEGER NOT NULL DEFAULT 0,    -- confusable with another account's name (moderator review)
        client_pub    BLOB,                          -- the client key the account is bound to
        banned        INTEGER NOT NULL DEFAULT 0,
        ban_reason    TEXT,
        created       INTEGER NOT NULL,
        linked        INTEGER,
        last_refresh  INTEGER,
        UNIQUE (skeleton, tag)
    );
    CREATE TABLE tickets (
        key_gen     INTEGER NOT NULL,
        serial      INTEGER NOT NULL,
        account     INTEGER NOT NULL REFERENCES accounts(id),
        ticket_id   BLOB NOT NULL,
        issued      INTEGER NOT NULL,
        expires     INTEGER NOT NULL,
        revoked     INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (key_gen, serial)
    );
    CREATE INDEX tickets_account ON tickets(account);
    CREATE TABLE serials (key_gen INTEGER PRIMARY KEY, next INTEGER NOT NULL);
    CREATE TABLE links (
        link_id     TEXT PRIMARY KEY,                -- the launcher's poll secret
        state       TEXT NOT NULL UNIQUE,            -- the OAuth state (travels through the browser)
        challenge   BLOB NOT NULL,
        client_pub  BLOB NOT NULL,
        created     INTEGER NOT NULL,
        account     INTEGER REFERENCES accounts(id),
        status      TEXT NOT NULL                    -- pending, authorized, consumed, failed
    );
    CREATE TABLE nonces (nonce BLOB PRIMARY KEY, seen INTEGER NOT NULL);
    CREATE TABLE deny_state (
        key_gen     INTEGER PRIMARY KEY,
        list_serial INTEGER NOT NULL,
        content     BLOB NOT NULL,                   -- sha256 of (serials, accounts) at the last publish
        built       INTEGER NOT NULL,
        signed      BLOB NOT NULL
    );
    CREATE TABLE audit (id INTEGER PRIMARY KEY, ts INTEGER NOT NULL, action TEXT NOT NULL, detail TEXT NOT NULL);
    """,
    # 1 -> 2: the single Discord login (launcher Social SDK token -> /v1/link/discord). The player's own Discord
    # username, shown back only to that player (finish / refresh), never to anyone else.
    """
    ALTER TABLE accounts ADD COLUMN discord_handle TEXT;
    """,
    # 2 -> 3: rate limits that survive a restart. key = HMAC(daily salt, client address): no address is stored, and
    # a row lives only for its window (pruned on every check).
    """
    CREATE TABLE rate_hits (bucket TEXT NOT NULL, key BLOB NOT NULL, ts REAL NOT NULL);
    CREATE INDEX rate_hits_key ON rate_hits(bucket, key, ts);
    """,
    # 3 -> 4: the stats join. public_id = HMAC-SHA256(FM2K_STATS_ID_SECRET, discord_id)[:16 hex], the stats site's own
    # derivation, so a linked player's legacy hub history is theirs with no claim step. stats_hidden = profile opt-out.
    """
    ALTER TABLE accounts ADD COLUMN public_id TEXT;
    ALTER TABLE accounts ADD COLUMN stats_hidden INTEGER NOT NULL DEFAULT 0;
    CREATE UNIQUE INDEX accounts_public_id ON accounts(public_id);
    """,
    # 4 -> 5: match results (spec section 7, STATS_DESIGN.md 4.2). games = the generated per-game table (admin
    # import-games); matches = one row per match key (session_key, match_index) with its counting state; receipts =
    # every verified receipt (opponents' signatures: never exported); marks = strikes / left early / disputes; reports =
    # the bodies already processed per account (a re-sent report is a no-op) and the per-account daily cap.
    """
    CREATE TABLE games (
        game_id             TEXT NOT NULL,           -- the catalog id, as receipts carry it
        build_id            TEXT NOT NULL,           -- sha256 of the .kgt (hex), as receipts carry it
        family              TEXT NOT NULL,           -- fm2k, fm95, ...: the stats key is family:game_id
        name                TEXT,
        rounds_to_win_legal TEXT NOT NULL,
        fps                 INTEGER NOT NULL,
        min_round_frames    INTEGER NOT NULL,
        double_ko           INTEGER NOT NULL,
        input_width         INTEGER NOT NULL,
        max_match_frames    INTEGER NOT NULL,
        worker              INTEGER NOT NULL DEFAULT 0,
        setup_layout        TEXT NOT NULL DEFAULT '',
        confirmed           INTEGER NOT NULL DEFAULT 0,
        imported            INTEGER NOT NULL,
        PRIMARY KEY (game_id, build_id)
    );
    CREATE TABLE matches (
        id            INTEGER PRIMARY KEY,
        match_id      TEXT NOT NULL UNIQUE,          -- public, 12 random hex, never derived
        session_key   BLOB NOT NULL,
        match_index   INTEGER NOT NULL,
        game_id       TEXT NOT NULL,
        build_id      TEXT NOT NULL,
        acct0         INTEGER NOT NULL REFERENCES accounts(id),
        acct1         INTEGER NOT NULL REFERENCES accounts(id),
        pub0          BLOB NOT NULL,                 -- the client keys that signed (a later relink does not matter)
        pub1          BLOB NOT NULL,
        started_unix  INTEGER NOT NULL,
        rounds_to_win INTEGER NOT NULL,
        status        TEXT NOT NULL,                 -- open pending final disputed void left_early dropped
        winner_slot   INTEGER,                       -- 0, 1, 255 draw
        rounds0       INTEGER,
        rounds1       INTEGER,
        decided_frame INTEGER,
        flags         INTEGER NOT NULL DEFAULT 0,    -- 1 desync, 4 late claim, 8 pair cap (not rated)
        left_slot     INTEGER,
        first_report  INTEGER NOT NULL,
        counted_at    INTEGER,                       -- when it first counted (the 24 h pending window, the pair cap day)
        settled_at    INTEGER,
        updated       INTEGER NOT NULL,
        exported      TEXT,                          -- the last results.jsonl state written (no duplicate lines)
        UNIQUE (session_key, match_index)
    );
    CREATE INDEX matches_status ON matches(status, counted_at);
    CREATE INDEX matches_pair ON matches(acct0, acct1, counted_at);
    CREATE TABLE receipts (
        match       INTEGER NOT NULL REFERENCES matches(id),
        sha         BLOB NOT NULL,
        signer_slot INTEGER NOT NULL,
        kind        INTEGER NOT NULL,
        frame       INTEGER NOT NULL,
        bytes       BLOB NOT NULL,
        received    INTEGER NOT NULL,
        PRIMARY KEY (match, sha)
    );
    CREATE TABLE marks (
        account INTEGER NOT NULL REFERENCES accounts(id),
        match   INTEGER NOT NULL REFERENCES matches(id),
        kind    TEXT NOT NULL,                       -- strike left_early left_early_behind disputed late_claim
        ts      INTEGER NOT NULL,
        PRIMARY KEY (account, match, kind)
    );
    CREATE TABLE reports (
        account  INTEGER NOT NULL REFERENCES accounts(id),
        body_sha BLOB NOT NULL,
        match    INTEGER REFERENCES matches(id),
        ts       INTEGER NOT NULL,
        PRIMARY KEY (account, body_sha)
    );
    CREATE INDEX reports_ts ON reports(account, ts);
    CREATE INDEX tickets_window ON tickets(expires);
    """,
]

SCHEMA_VERSION = len(MIGRATIONS)


class Database:
    """One connection, serialized by a lock (the service is a single small process)."""

    def __init__(self, path: str):
        self.path = path
        self.conn = sqlite3.connect(path, check_same_thread=False, isolation_level=None)
        self.conn.row_factory = sqlite3.Row
        self.conn.execute("PRAGMA foreign_keys = ON")
        if path != ":memory:":
            self.conn.execute("PRAGMA journal_mode = WAL")
        self.lock = threading.RLock()
        self.migrate()

    def migrate(self) -> int:
        with self.lock:
            v = self.conn.execute("PRAGMA user_version").fetchone()[0]
            if v > SCHEMA_VERSION:
                raise RuntimeError(f"{self.path}: schema version {v} is newer than this code ({SCHEMA_VERSION})")
            while v < SCHEMA_VERSION:
                self.conn.execute("BEGIN")
                try:
                    for stmt in _split(MIGRATIONS[v]):
                        self.conn.execute(stmt)
                    self.conn.execute(f"PRAGMA user_version = {v + 1}")
                    self.conn.execute("COMMIT")
                except Exception:
                    self.conn.execute("ROLLBACK")
                    raise
                v += 1
            return v

    def q(self, sql: str, args=()):
        with self.lock:
            return self.conn.execute(sql, args).fetchall()

    def one(self, sql: str, args=()):
        with self.lock:
            return self.conn.execute(sql, args).fetchone()

    def x(self, sql: str, args=()):
        with self.lock:
            return self.conn.execute(sql, args)

    def tx(self):
        return _Tx(self)

    def meta_get(self, key: str, default=None):
        r = self.one("SELECT value FROM meta WHERE key = ?", (key,))
        return r[0] if r else default

    def meta_set(self, key: str, value) -> None:
        self.x("INSERT INTO meta(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value",
               (key, str(value)))

    def audit(self, ts: int, action: str, detail: str) -> None:
        self.x("INSERT INTO audit(ts, action, detail) VALUES(?, ?, ?)", (ts, action, detail))


class _Tx:
    def __init__(self, db: Database):
        self.db = db

    def __enter__(self):
        self.db.lock.acquire()
        self.db.conn.execute("BEGIN IMMEDIATE")
        return self.db

    def __exit__(self, et, ev, tb):
        try:
            self.db.conn.execute("ROLLBACK" if et else "COMMIT")
        finally:
            self.db.lock.release()
        return False


def _split(script: str):
    """Statements of a migration (no '--' or ';' inside the schema's string literals)."""
    code = "\n".join(ln.split("--", 1)[0] for ln in script.splitlines())  # comments first: they may hold ';'
    return [s.strip() for s in code.split(";") if s.strip()]
