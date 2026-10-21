"""The service's core, free of HTTP: the online ticket key, the published KEYSET, accounts, names and #tags, ticket
issuance and the deny list. app.py (routes) and admin.py (CLI) both drive this.
"""
from __future__ import annotations

import glob
import hashlib
import hmac
import json
import os
import secrets
import time
from dataclasses import dataclass
from typing import Callable, Optional

import names
import tickets as T
from config import Config
from db import Database


class ServiceError(Exception):
    """A refusal with a stable code (the response body's `error`) and an HTTP status."""

    def __init__(self, status: int, code: str, detail: str = ""):
        super().__init__(code)
        self.status, self.code, self.detail = status, code, detail


RETIRED_DENY_SECONDS = 72 * 3600  # a retired key generation's deny list is served this long past its last ticket


# ---- the online ticket key ----
@dataclass
class TicketKey:
    key_gen: int
    seed: bytes

    @property
    def pub(self) -> bytes:
        return T.seed_public(self.seed)


def write_ticket_key(path: str, key_gen: int, seed: bytes) -> None:
    """Written 0600 through a temp file. Refuses to overwrite: rotation writes a new file and a new KEYSET."""
    if os.path.exists(path):
        raise FileExistsError(path)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    text = ("# PovertyCaster community ONLINE ticket key. Secret: keep it 0600, never commit it.\n"
            f"key_gen={key_gen}\nseed={seed.hex()}\npublic={T.seed_public(seed).hex()}\n")
    tmp = path + ".tmp"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(text)
    os.replace(tmp, path)


def read_ticket_key(path: str) -> TicketKey:
    if not os.path.isfile(path):
        raise ServiceError(503, "ticket_key_missing",
                           f"no online ticket key at {path}: run `python3 admin.py gen-ticket-key --gen N`")
    vals = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                vals[k.strip()] = v.strip()
    try:
        key = TicketKey(int(vals["key_gen"]), bytes.fromhex(vals["seed"]))
    except (KeyError, ValueError) as e:
        raise ServiceError(503, "ticket_key_bad", f"{path} is not a ticket key file ({e})")
    if len(key.seed) != 32 or ("public" in vals and vals["public"] != key.pub.hex()):
        raise ServiceError(503, "ticket_key_bad", f"{path}: seed and public key disagree")
    return key


class Community:
    def __init__(self, cfg: Config, db: Optional[Database] = None, clock: Callable[[], float] = time.time):
        self.cfg = cfg
        self.db = db or Database(cfg.db_path)
        self.clock = clock
        self._key: Optional[TicketKey] = None
        self._key_mtime = None
        self._ks_cache: tuple = (None, None, None)  # (dir signature, now bucket, (bytes, Keyset))
        self.ensure_public_ids()

    def now(self) -> int:
        return int(self.clock())

    # ---- key + KEYSET ----
    def ticket_key(self) -> TicketKey:
        p = self.cfg.ticket_key_path
        mtime = os.path.getmtime(p) if os.path.isfile(p) else None
        if self._key is None or mtime != self._key_mtime:
            self._key, self._key_mtime = read_ticket_key(p), mtime
        return self._key

    def keyset(self) -> tuple[bytes, T.Keyset]:
        """The newest KEYSET in keyset_dir that verifies against the two roots with the service's 300 s skew, is fresh
        and not below the persisted floor. Pre-signed future KEYSETs wait in the folder until they come due."""
        files = sorted(glob.glob(os.path.join(self.cfg.keyset_dir, "*.pckeys")))
        if not files:
            raise ServiceError(503, "keyset_missing",
                               f"no *.pckeys in {self.cfg.keyset_dir}: the owner signs one offline with sign_keyset.py")
        sig = tuple((f, os.path.getmtime(f), os.path.getsize(f)) for f in files)
        now = self.now()
        if self._ks_cache[0] == sig and self._ks_cache[1] == now // 60:
            return self._ks_cache[2]
        floor = int(self.db.meta_get("keyset_floor", "0"))
        mask = int(self.db.meta_get("revoked_root_mask", "0"))
        best, reasons = None, []
        for f in files:
            with open(f, "rb") as fh:
                b = fh.read()
            r, ks = T.parse_keyset(b, self.cfg.root_pubs, floor, now, T.SERVICE_KEYSET_SKEW, mask)
            if r == T.OK and (best is None or ks.serial > best[1].serial):
                best = (b, ks)
            elif r != T.OK:
                reasons.append(f"{os.path.basename(f)}: {r}")
        if best is None:
            raise ServiceError(503, "keyset_unusable", "no usable KEYSET (" + "; ".join(reasons) + ")")
        b, ks = best
        if ks.serial > floor:
            self.db.meta_set("keyset_floor", ks.serial)
        if ks.revoked_root <= 1:
            self.db.meta_set("revoked_root_mask", mask | (1 << ks.revoked_root))  # sticky
        self._ks_cache = (sig, now // 60, best)
        return best

    def signing_key(self) -> TicketKey:
        """The online key, checked against the published KEYSET: a role-0 entry, not revoked, at least min_key_gen,
        its window covering now. The service never mints a ticket clients would refuse."""
        key = self.ticket_key()
        _, ks = self.keyset()
        e = ks.key(key.key_gen)
        now = self.now()
        if e is None or e.role != T.ROLE_TICKET or e.pub != key.pub:
            raise ServiceError(503, "ticket_key_not_in_keyset",
                               f"the online key (gen {key.key_gen}) is not a ticket key of KEYSET serial {ks.serial}")
        if ks.revoked_gen(key.key_gen) or key.key_gen < ks.min_key_gen:
            raise ServiceError(503, "ticket_key_revoked", f"KEYSET serial {ks.serial} revokes gen {key.key_gen}")
        if not e.not_before <= now <= e.not_after:
            raise ServiceError(503, "ticket_key_outside_window", f"gen {key.key_gen} is outside its KEYSET window")
        return key

    # ---- accounts ----
    def account_for_discord(self, discord_id: str, handle: Optional[str] = None) -> int:
        """The account row for a Discord user, created on first sign-in with a fresh random account_id. The Discord
        username is refreshed at every sign-in (shown back only to that player)."""
        with self.db.tx() as db:
            r = db.one("SELECT id FROM accounts WHERE discord_id = ?", (discord_id,))
            if r:
                if handle:
                    db.x("UPDATE accounts SET discord_handle = ? WHERE id = ?", (handle, r[0]))
                return r[0]
            while True:
                aid = secrets.token_bytes(16)
                if not db.one("SELECT 1 FROM accounts WHERE account_id = ?", (aid,)):
                    break
            cur = db.x("INSERT INTO accounts(account_id, discord_id, discord_handle, public_id, created) "
                       "VALUES(?, ?, ?, ?, ?)", (aid, discord_id, handle, self.stats_public_id(discord_id), self.now()))
            db.audit(self.now(), "account_created", aid.hex())
            return cur.lastrowid

    # ---- the stats join ----
    def stats_public_id(self, discord_id: str) -> Optional[str]:
        """The stats site's public id for a Discord user (stats/privacy.py public_id): HMAC-SHA256 under the shared
        FM2K_STATS_ID_SECRET, 16 hex. None while the secret is not configured."""
        if not self.cfg.stats_id_secret:
            return None
        return hmac.new(self.cfg.stats_id_secret.encode(), discord_id.encode(), hashlib.sha256).hexdigest()[:16]

    def ensure_public_ids(self) -> int:
        """Fill accounts.public_id where missing (accounts made before the secret was set). Returns how many."""
        if not self.cfg.stats_id_secret:
            return 0
        rows = self.db.q("SELECT id, discord_id FROM accounts WHERE public_id IS NULL")
        for r in rows:
            self.db.x("UPDATE accounts SET public_id = ? WHERE id = ?", (self.stats_public_id(r["discord_id"]), r["id"]))
        return len(rows)

    def set_stats_hidden(self, row: int, hidden: bool) -> None:
        """The player's profile opt-out. Republishes export/hidden.json (public ids only) for the stats site."""
        self.db.x("UPDATE accounts SET stats_hidden = ? WHERE id = ?", (1 if hidden else 0, row))
        self.export_hidden()

    def export_hidden(self) -> str:
        ids = sorted(r[0] for r in self.db.q(
            "SELECT public_id FROM accounts WHERE stats_hidden = 1 AND public_id IS NOT NULL"))
        os.makedirs(self.cfg.export_dir, exist_ok=True)
        path = os.path.join(self.cfg.export_dir, "hidden.json")
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"v": 1, "hidden": ids}, f)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
        return path

    def account(self, row: int):
        return self.db.one("SELECT * FROM accounts WHERE id = ?", (row,))

    def find_account(self, who: str):
        """By account id (32 hex) or Name#1234 (admin)."""
        who = who.strip()
        if len(who) == 32 and all(c in "0123456789abcdefABCDEF" for c in who):
            return self.db.one("SELECT * FROM accounts WHERE account_id = ?", (bytes.fromhex(who),))
        if "#" in who:
            name, _, tag = who.rpartition("#")
            if tag.isdigit():
                return self.db.one("SELECT * FROM accounts WHERE name = ? AND tag = ?", (name, int(tag)))
        return None

    def set_name(self, row: int, raw: str) -> tuple[str, int]:
        """Normalize, then pick a #tag unique under the name's skeleton (kept when the skeleton is unchanged)."""
        try:
            name = names.normalize_name(raw)
        except names.BadName as e:
            raise ServiceError(400, "bad_name", str(e))
        skel = names.skeleton(name)
        with self.db.tx() as db:
            acc = db.one("SELECT * FROM accounts WHERE id = ?", (row,))
            if acc["skeleton"] == skel and acc["tag"] is not None:
                tag = acc["tag"]
            else:
                used = {r[0] for r in db.q("SELECT tag FROM accounts WHERE skeleton = ? AND id != ?", (skel, row))}
                if len(used) >= 10000:
                    raise ServiceError(409, "name_full", "every #tag of this name is taken: choose another name")
                tag = secrets.randbelow(10000)
                while tag in used:
                    tag = secrets.randbelow(10000)
            others = db.q("SELECT name FROM accounts WHERE skeleton = ? AND id != ? AND banned = 0", (skel, row))
            flagged = 1 if any(o["name"] != name for o in others) else 0  # confusable, not identical: review
            db.x("UPDATE accounts SET name = ?, skeleton = ?, tag = ?, flagged = ? WHERE id = ?",
                 (name, skel, tag, flagged, row))
        return name, tag

    def reskeleton(self) -> list[tuple[int, int, int]]:
        """Recompute every account's skeleton after the look-alike table changed (the oldest account keeps its #tag
        when two now collide; the later one gets a new one) and re-flag look-alikes. Returns (row, old tag, new tag)
        for every #tag that had to change; each is audited."""
        changed = []
        with self.db.tx() as db:
            rows = db.q("SELECT id, name, tag FROM accounts WHERE name IS NOT NULL ORDER BY id")
            db.x("UPDATE accounts SET skeleton = NULL")
            taken: dict[str, set[int]] = {}
            for r in rows:
                skel = names.skeleton(r["name"])
                used = taken.setdefault(skel, set())
                tag = r["tag"]
                if tag is None or tag in used:
                    tag = secrets.randbelow(10000)
                    while tag in used:
                        tag = secrets.randbelow(10000)
                    changed.append((r["id"], r["tag"], tag))
                    db.audit(self.now(), "tag_reassigned", f"{r['id']}:{r['tag']}->{tag}")
                used.add(tag)
                db.x("UPDATE accounts SET skeleton = ?, tag = ? WHERE id = ?", (skel, tag, r["id"]))
            for r in rows:
                skel = names.skeleton(r["name"])
                others = db.q("SELECT name FROM accounts WHERE skeleton = ? AND id != ? AND banned = 0",
                              (skel, r["id"]))
                db.x("UPDATE accounts SET flagged = ? WHERE id = ?",
                     (1 if any(o["name"] != r["name"] for o in others) else 0, r["id"]))
        return changed

    def bind_client(self, row: int, client_pub: bytes) -> None:
        """Link or relink: a new client key denies every unexpired ticket of the old one (section 9)."""
        now = self.now()
        with self.db.tx() as db:
            acc = db.one("SELECT client_pub FROM accounts WHERE id = ?", (row,))
            if acc["client_pub"] is not None and acc["client_pub"] != client_pub:
                db.x("UPDATE tickets SET revoked = 1 WHERE account = ? AND expires + ? >= ?", (row, T.SKEW, now))
                db.audit(now, "relink", f"account row {row}: old client key's tickets denied")
            db.x("UPDATE accounts SET client_pub = ?, linked = ? WHERE id = ?", (client_pub, now, row))

    # ---- tickets ----
    def issue_ticket(self, row: int) -> bytes:
        acc = self.account(row)
        if acc is None:
            raise ServiceError(401, "relink_required", "unknown account")
        if acc["banned"]:
            raise ServiceError(403, "banned", "this account is banned")
        if not acc["name"]:
            raise ServiceError(409, "name_required", "choose a name first")
        key = self.signing_key()
        now = self.now()
        with self.db.tx() as db:
            r = db.one("SELECT next FROM serials WHERE key_gen = ?", (key.key_gen,))
            serial = r[0] if r else 1
            db.x("INSERT INTO serials(key_gen, next) VALUES(?, ?) ON CONFLICT(key_gen) DO UPDATE SET next = ?",
                 (key.key_gen, serial + 1, serial + 1))
            t = T.Ticket(key.key_gen, acc["account_id"], acc["client_pub"], now, now + T.TICKET_LIFETIME, serial,
                         acc["tag"], acc["name"])
            b = T.sign_ticket(t, key.seed)
            db.x("INSERT INTO tickets(key_gen, serial, account, ticket_id, issued, expires) VALUES(?,?,?,?,?,?)",
                 (key.key_gen, serial, row, T.ticket_id(b), now, t.expires))
            db.x("UPDATE accounts SET last_refresh = ? WHERE id = ?", (now, row))
        return b

    def account_for_refresh(self, ticket: bytes) -> tuple[int, T.Ticket]:
        """A ticket the service issued itself (any age: the link never expires), not denied, still bound to the
        account's current client key. The service's own record is the authority, so a rotated-out key_gen still
        refreshes."""
        r, t = T.decode_ticket(ticket)
        if r != T.OK:
            raise ServiceError(401, "bad_ticket", r)
        row = self.db.one("SELECT * FROM tickets WHERE key_gen = ? AND serial = ?", (t.key_gen, t.serial))
        if row is None or row["ticket_id"] != T.ticket_id(ticket):
            raise ServiceError(401, "relink_required", "this ticket was not issued here")
        acc = self.account(row["account"])
        if acc["banned"]:
            raise ServiceError(403, "banned", "this account is banned")
        if row["revoked"] or acc["client_pub"] != t.client_pub:
            raise ServiceError(401, "relink_required", "this ticket was replaced by a newer link")
        return row["account"], t

    # ---- nonces (section 6: 11 minutes, stored only after the signature verified) ----
    def nonce_seen(self, nonce: bytes) -> bool:
        return self.db.one("SELECT 1 FROM nonces WHERE nonce = ? AND seen >= ?", (nonce, self.now() - 660)) is not None

    def nonce_store(self, nonce: bytes) -> None:
        now = self.now()
        self.db.x("DELETE FROM nonces WHERE seen < ?", (now - 660,))
        self.db.x("INSERT OR REPLACE INTO nonces(nonce, seen) VALUES(?, ?)", (nonce, now))

    # ---- moderation ----
    def ban(self, row: int, reason: str, ban: bool = True) -> None:
        self.db.x("UPDATE accounts SET banned = ?, ban_reason = ? WHERE id = ?", (1 if ban else 0,
                                                                                 reason if ban else None, row))
        self.db.audit(self.now(), "ban" if ban else "unban", f"account row {row}: {reason}")

    def revoke_serial(self, key_gen: int, serial: int) -> bool:
        cur = self.db.x("UPDATE tickets SET revoked = 1 WHERE key_gen = ? AND serial = ?", (key_gen, serial))
        self.db.audit(self.now(), "revoke_ticket", f"gen {key_gen} serial {serial}")
        return cur.rowcount > 0

    # ---- the deny list (section 8) ----
    def retired_keys(self) -> dict[int, TicketKey]:
        """Earlier generations' key files (COMMUNITY_RETIRED_TICKET_KEYS), kept after a rotation only to sign their
        deny lists while that generation's tickets can still be shown by peers."""
        out = {}
        for p in (x.strip() for x in self.cfg.retired_ticket_keys.split(",")):
            if p:
                k = read_ticket_key(p)
                out[k.key_gen] = k
        return out

    def deny_gens(self) -> list[int]:
        """Every generation /v1/deny serves now: the online key's, and each retired one until RETIRED_DENY_SECONDS
        after its last ticket expired."""
        gens = [self.signing_key().key_gen]
        now = self.now()
        for gen in sorted(self.retired_keys()):
            last = self.db.one("SELECT MAX(expires) FROM tickets WHERE key_gen = ?", (gen,))[0]
            if gen not in gens and last is not None and now <= last + RETIRED_DENY_SECONDS:
                gens.append(gen)
        return gens

    def deny_list(self, gen: Optional[int] = None) -> bytes:
        """The signed PCDENY1 for one key generation (default: the online key's). Rebuilt (list_serial + 1) when its
        content changes or every deny_refresh_seconds, so a ban is live at the next fetch. A retired generation's list
        is served for RETIRED_DENY_SECONDS past its last ticket: peers may still show those tickets."""
        key = self.signing_key()
        if gen is not None and gen != key.key_gen:
            if gen not in self.deny_gens():
                raise ServiceError(404, "deny_gen_unknown", f"no deny list is served for key generation {gen}")
            key = self.retired_keys()[gen]
        now = self.now()
        serials = [r[0] for r in self.db.q(
            "SELECT serial FROM tickets WHERE key_gen = ? AND revoked = 1 AND expires + ? >= ?",
            (key.key_gen, T.SKEW, now))]
        accounts = [r[0] for r in self.db.q("SELECT account_id FROM accounts WHERE banned = 1")]
        content = hashlib.sha256(repr((sorted(serials), sorted(accounts))).encode()).digest()
        with self.db.tx() as db:
            st = db.one("SELECT * FROM deny_state WHERE key_gen = ?", (key.key_gen,))
            if st and st["content"] == content and now - st["built"] < self.cfg.deny_refresh_seconds:
                return st["signed"]
            serial = (st["list_serial"] + 1) if st else 1
            body = T.deny_body(key.key_gen, serial, now, now + self.cfg.deny_valid_seconds, serials, accounts)
            signed = T.sign_deny(body, key.seed)
            db.x("INSERT OR REPLACE INTO deny_state(key_gen, list_serial, content, built, signed) VALUES(?,?,?,?,?)",
                 (key.key_gen, serial, content, now, signed))
            return signed

    def stats(self) -> dict:
        one = lambda sql, a=(): self.db.one(sql, a)[0]  # noqa: E731
        now = self.now()
        return {
            "accounts": one("SELECT COUNT(*) FROM accounts"),
            "named": one("SELECT COUNT(*) FROM accounts WHERE name IS NOT NULL"),
            "banned": one("SELECT COUNT(*) FROM accounts WHERE banned = 1"),
            "flagged_names": one("SELECT COUNT(*) FROM accounts WHERE flagged = 1"),
            "refreshed_7d": one("SELECT COUNT(*) FROM accounts WHERE last_refresh >= ?", (now - 7 * 86400,)),
            "tickets_issued": one("SELECT COUNT(*) FROM tickets"),
            "tickets_live": one("SELECT COUNT(*) FROM tickets WHERE expires >= ? AND revoked = 0", (now,)),
            "tickets_revoked_live": one("SELECT COUNT(*) FROM tickets WHERE expires + 300 >= ? AND revoked = 1",
                                        (now,)),
            "pending_links": one("SELECT COUNT(*) FROM links WHERE status IN ('pending', 'authorized')"),
        }
