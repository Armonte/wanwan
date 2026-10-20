"""PCTKT1 tickets, PCKEYS1 KEYSETs and the signed messages around them (COMMUNITY_TICKET_SPEC.md v9.1 sections 1-3, 6,
8, 9). A byte-for-byte port of PovertyCaster's pc-core `pc/community/Ticket.cpp` / `Session.cpp`: the pinned vectors in
community/tests/test_vectors.py hold this module to the C++ bytes.

The service only ever holds the ONLINE ticket key (a role-0 KEYSET entry). The two offline root keys sign KEYSETs on the
owner's machine (sign_keyset.py); here they appear as public keys only, to verify the KEYSET the service publishes.

Every signature is Ed25519(key, M) where M = domain tag || body. Integers are little-endian, padding is zero.
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass, field
from typing import Optional

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey

from names import name_ok  # noqa: F401  (re-exported: the section 10 verifier rule)

# ---- domain tags (section 1) ----
TAG_KEYSET = b"PC-COMMUNITY-KEYSET-V1\n"
TAG_TICKET = b"PC-COMMUNITY-TICKET-V1\n"
TAG_PERKS = b"PC-COMMUNITY-PERKS-V1\n"
TAG_DENY = b"PC-COMMUNITY-DENY-V1\n"
TAG_LINK = b"PC-COMMUNITY-LINK-V1\n"
TAG_PROOF = b"PC-COMMUNITY-PROOF-V1\n"
TAG_CALL = b"PC-COMMUNITY-CALL-V1\n"
TAG_RESULT = b"PC-COMMUNITY-RESULT-V1\n"
TAG_REPLAY = b"PC-COMMUNITY-REPLAY-V1\n"
ALL_TAGS = (TAG_KEYSET, TAG_TICKET, TAG_PERKS, TAG_DENY, TAG_LINK, TAG_PROOF, TAG_CALL, TAG_RESULT, TAG_REPLAY)

# The two offline roots (COMMUNITY_SERVICE.md section 7, generated 2026-10-20). Public halves only.
ROOT_A_PUB = bytes.fromhex("c9e1bf09a428508b860cd21cb3b49dcadc2c8e3e6abe17cd51e8155bfe8797de")
ROOT_B_PUB = bytes.fromhex("b597400beba4adb355936c998653c6b2bbc621047446b79fef412100b7d1995b")

KEYSET_MAGIC = b"PCKEYS1\0"
TICKET_MAGIC = b"PCTKT1\0\0"
DENY_MAGIC = b"PCDENY1\0"
TICKET_BODY, TICKET_SIZE = 188, 252
NAME_MAX_BYTES, NAME_MAX_SCALARS = 96, 24
TICKET_MAX_LIFETIME, SKEW = 72 * 3600, 300
TICKET_LIFETIME = 48 * 3600  # what the service issues (section 3)
KEYSET_MAX_KEYS, KEYSET_MAX_REVOKED, KEYSET_MAX_SCOPE, GAME_ID_MAX = 16, 32, 8, 64
CLIENT_KEYSET_SKEW, SERVICE_KEYSET_SKEW = 86400, 300
ROLE_TICKET, ROLE_WORKER = 0, 1
# the PCTKT1 v1 layout (Ticket.hpp namespace off)
OFF = dict(magic=0, version=8, flags=10, key_gen=12, account=16, client_pub=32, issued=64, expires=72, serial=80,
           tag=88, name_len=90, name=91, zero=187, sig=188)

# reason codes, the C++ reasonName() strings
OK = "ok"


class Refused(Exception):
    def __init__(self, reason: str):
        super().__init__(reason)
        self.reason = reason


# ---- little helpers ----
def le(v: int, n: int) -> bytes:
    return int(v).to_bytes(n, "little")


def get_le(b: bytes, at: int, n: int) -> int:
    return int.from_bytes(b[at:at + n], "little")


def lp(s: bytes) -> bytes:
    if len(s) > 0xFFFF:
        raise ValueError("lp field past 65535 bytes")
    return le(len(s), 2) + s


def sha256(b: bytes) -> bytes:
    return hashlib.sha256(b).digest()


# ---- Ed25519 ----
def seed_public(seed: bytes) -> bytes:
    return Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def sign(seed: bytes, tag: bytes, body: bytes) -> bytes:
    return Ed25519PrivateKey.from_private_bytes(seed).sign(tag + body)


def verify(pub: bytes, tag: bytes, body: bytes, sig: bytes) -> bool:
    if len(pub) != 32 or len(sig) != 64:
        return False
    # S >= L is refused (OpenSSL checks it too; kept explicit so a backend change can't loosen it)
    if int.from_bytes(sig[32:], "little") >= _L:
        return False
    try:
        Ed25519PublicKey.from_public_bytes(pub).verify(sig, tag + body)
        return True
    except (InvalidSignature, ValueError):
        return False


_P = 2 ** 255 - 19
_L = 2 ** 252 + 27742317777372353535851937790883648493
_D = (-121665 * pow(121666, _P - 2, _P)) % _P
_SQRT_M1 = pow(2, (_P - 1) // 4, _P)


def _add(a, b):
    (x1, y1), (x2, y2) = a, b
    t = _D * x1 * x2 * y1 * y2 % _P
    x3 = (x1 * y2 + x2 * y1) * pow(1 + t, _P - 2, _P) % _P
    y3 = (y1 * y2 + x1 * x2) * pow(1 - t, _P - 2, _P) % _P
    return x3, y3


def pubkey_strong(pub: bytes) -> bool:
    """ed25519PublicKeyStrong: canonical y < p, on the curve, no x = 0 with the sign bit, not of small order."""
    if len(pub) != 32:
        return False
    y = int.from_bytes(pub, "little") & ((1 << 255) - 1)
    sign_bit = pub[31] >> 7
    if y >= _P:
        return False
    x2 = (y * y - 1) * pow(_D * y * y + 1, _P - 2, _P) % _P
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P:
        x = x * _SQRT_M1 % _P
    if (x * x - x2) % _P:
        return False  # not on the curve
    if x == 0 and sign_bit:
        return False  # a second encoding of x = 0
    if (x & 1) != sign_bit:
        x = _P - x
    q = (x, y)
    for _ in range(3):
        q = _add(q, q)
    return q != (0, 1)  # 8P is the identity: one of the 8 torsion points


# ---- KEYSET (section 2) ----
@dataclass
class KeysetEntry:
    key_gen: int
    pub: bytes
    not_before: int
    not_after: int
    role: int = ROLE_TICKET
    scope: list[str] = field(default_factory=list)


@dataclass
class Keyset:
    serial: int
    issued: int
    valid_until: int
    min_key_gen: int
    root_id: int
    keys: list[KeysetEntry]
    revoked: list[int] = field(default_factory=list)
    revoked_root: int = 0xFF

    def key(self, gen: int) -> Optional[KeysetEntry]:
        return next((k for k in self.keys if k.key_gen == gen), None)

    def revoked_gen(self, gen: int) -> bool:
        return gen in self.revoked


def _game_id_printable(g: str) -> bool:
    b = g.encode("utf-8")
    return len(b) <= GAME_ID_MAX and all(0x20 <= c <= 0x7E for c in b)


def keyset_body(k: Keyset) -> bytes:
    """The unsigned KEYSET bytes (what the root signs, after the KEYSET tag). Raises ValueError on what the C++ signer
    refuses to encode (a scope past 255 entries, a game_id past 64 bytes or outside printable ASCII)."""
    if len(k.keys) > 0xFF or len(k.revoked) > 0xFF:
        raise ValueError("too many keys or revoked generations to encode")
    b = bytearray(KEYSET_MAGIC + le(1, 2) + le(0, 2))
    b += le(k.serial, 8) + le(k.issued, 8) + le(k.valid_until, 8) + le(k.min_key_gen, 4)
    b += bytes([k.root_id, len(k.keys)])
    for e in k.keys:
        b += le(e.key_gen, 4) + bytes([e.role]) + e.pub + le(e.not_before, 8) + le(e.not_after, 8)
        if len(e.scope) > 0xFF:
            raise ValueError("scope past 255 game_ids")
        b.append(len(e.scope))
        for g in e.scope:
            if not _game_id_printable(g):
                raise ValueError(f"game_id {g!r} is past 64 bytes or not printable ASCII")
            b += lp(g.encode("ascii"))
    b.append(len(k.revoked))
    for g in k.revoked:
        b += le(g, 4)
    b.append(k.revoked_root)
    return bytes(b)


def sign_keyset(k: Keyset, root_seed: bytes) -> bytes:
    """Tests and the owner's offline tool only: the service never holds a root seed."""
    body = keyset_body(k)
    return body + sign(root_seed, TAG_KEYSET, body)


def _role_scope_ok(e: KeysetEntry) -> bool:
    if e.role == ROLE_TICKET:
        return not e.scope
    if e.role != ROLE_WORKER or not e.scope or len(e.scope) > KEYSET_MAX_SCOPE:
        return False
    for i, g in enumerate(e.scope):
        if not g or not _game_id_printable(g) or (i and not e.scope[i - 1] < g):
            return False
    return True


class _Reader:
    def __init__(self, b: bytes, at: int):
        self.b, self.at, self.ok = b, at, True

    def take(self, n: int) -> bytes:
        if not self.ok or self.at + n > len(self.b):
            self.ok = False
            return b"\0" * n
        out = self.b[self.at:self.at + n]
        self.at += n
        return out

    def le(self, n: int) -> int:
        return int.from_bytes(self.take(n), "little")

    def u8(self) -> int:
        return self.take(1)[0]


def parse_keyset(b: bytes, roots: tuple[bytes, bytes], min_serial: int = 0, now: int = 0,
                 future_skew: int = SERVICE_KEYSET_SKEW, revoked_mask: int = 0) -> tuple[str, Optional[Keyset]]:
    """parseKeyset, in the C++ order. Returns (reason, keyset or None). The service passes SERVICE_KEYSET_SKEW."""
    head = 8 + 2 + 2 + 8 + 8 + 8 + 4 + 1 + 1
    if len(b) < head + 1 + 1 + 64 or b[:8] != KEYSET_MAGIC:
        return "unknown_format", None
    if get_le(b, 8, 2) != 1:
        return "newer_format", None
    if get_le(b, 10, 2) != 0:
        return "unknown_flags", None
    rd = _Reader(b, 12)
    serial, issued, valid_until = rd.le(8), rd.le(8), rd.le(8)
    min_key_gen, root_id, n_keys = rd.le(4), rd.u8(), rd.u8()
    if root_id > 1 or n_keys < 1 or n_keys > KEYSET_MAX_KEYS:
        return "noncanonical", None
    keys: list[KeysetEntry] = []
    for _ in range(n_keys):
        gen, role, pub = rd.le(4), rd.u8(), rd.take(32)
        nb, na, n_scope = rd.le(8), rd.le(8), rd.u8()
        scope = []
        for _ in range(n_scope):
            if not rd.ok:
                break
            scope.append(rd.take(rd.le(2)).decode("latin-1"))
        if not rd.ok:
            return "unknown_format", None
        if role > ROLE_WORKER:
            return "noncanonical", None
        e = KeysetEntry(gen, pub, nb, na, role, scope)
        if ((keys and gen <= keys[-1].key_gen) or na < nb or not _role_scope_ok(e) or not pubkey_strong(pub)
                or pub in roots):
            return "noncanonical", None
        if any(p.pub == pub for p in keys):
            return "noncanonical", None
        keys.append(e)
    n_rev = rd.u8()
    if not rd.ok or len(b) - rd.at < n_rev * 4 + 1 + 64:
        return "unknown_format", None
    if n_rev > KEYSET_MAX_REVOKED or len(b) - rd.at != n_rev * 4 + 1 + 64:
        return "noncanonical", None
    revoked: list[int] = []
    for _ in range(n_rev):
        g = rd.le(4)
        if revoked and g <= revoked[-1]:
            return "noncanonical", None
        revoked.append(g)
    revoked_root = rd.u8()
    at = rd.at
    if revoked_root != 0xFF and revoked_root > 1:
        return "noncanonical", None
    if revoked_root == root_id:
        return "noncanonical", None
    if revoked_mask & (1 << root_id):
        return "keyset_revoked_root", None
    if not verify(roots[root_id], TAG_KEYSET, b[:at], b[at:at + 64]):
        return "bad_signature", None
    if issued > valid_until:
        return "noncanonical", None
    if serial < min_serial:
        return "keyset_rollback", None
    if now and issued > now + future_skew:
        return "not_yet_valid", None
    if now and now > valid_until:
        return "keyset_stale", None
    return OK, Keyset(serial, issued, valid_until, min_key_gen, root_id, keys, revoked, revoked_root)


# ---- TICKET (section 3) ----
@dataclass
class Ticket:
    key_gen: int
    account: bytes  # 16 random bytes, never derived from Discord
    client_pub: bytes
    issued: int
    expires: int
    serial: int
    tag: int  # 0..9999
    name: str  # UTF-8, NFC

    @property
    def display(self) -> str:
        return f"{self.name}#{self.tag:04d}"


def ticket_body(t: Ticket) -> bytes:
    name = t.name.encode("utf-8")
    if (not name or len(name) > NAME_MAX_BYTES or not 0 <= t.tag <= 9999 or t.expires < t.issued
            or t.expires - t.issued > TICKET_MAX_LIFETIME or len(t.account) != 16 or len(t.client_pub) != 32):
        raise ValueError("ticket breaks a rule a verifier refuses")
    b = bytearray(TICKET_BODY)
    b[0:8] = TICKET_MAGIC
    b[8:16] = le(1, 2) + le(0, 2) + le(t.key_gen, 4)
    b[16:32] = t.account
    b[32:64] = t.client_pub
    b[64:90] = le(t.issued, 8) + le(t.expires, 8) + le(t.serial, 8) + le(t.tag, 2)
    b[90] = len(name)
    b[91:91 + len(name)] = name
    return bytes(b)


def sign_ticket(t: Ticket, ticket_seed: bytes) -> bytes:
    """The 252 bytes. Raises ValueError when the ticket breaks a client rule: the service never mints what clients
    reject."""
    if not name_ok(t.name.encode("utf-8")) or not pubkey_strong(t.client_pub):
        raise ValueError("bad name or client key")
    body = ticket_body(t)
    return body + sign(ticket_seed, TAG_TICKET, body)


def ticket_id(ticket: bytes) -> bytes:
    return sha256(ticket[:TICKET_BODY])


def decode_ticket(p: bytes) -> tuple[str, Optional[Ticket]]:
    """Steps 1-2 only (layout, flags, padding, name_len): the fields, unverified."""
    if len(p) != TICKET_SIZE or p[:8] != TICKET_MAGIC:
        return "unknown_format", None
    if get_le(p, 8, 2) != 1:
        return "newer_format", None
    if get_le(p, 10, 2) != 0:
        return "unknown_flags", None
    n = p[90]
    if n < 1 or n > NAME_MAX_BYTES:
        return "bad_name", None
    if any(p[91 + n:187]) or p[187]:
        return "bad_padding", None
    tag = get_le(p, 88, 2)
    if tag > 9999:
        return "unknown_format", None
    raw = p[91:91 + n]
    try:
        name = raw.decode("utf-8")
    except UnicodeDecodeError:
        name = raw.decode("utf-8", "replace")
    return OK, Ticket(get_le(p, 12, 4), p[16:32], p[32:64], get_le(p, 64, 8), get_le(p, 72, 8), get_le(p, 80, 8),
                      tag, name)


def verify_ticket(p: bytes, ks: Keyset, now: int, check_times: bool = True) -> tuple[str, Optional[Ticket]]:
    """verifyTicket (steps 1-7; the deny list is the caller's). `check_times` False skips step 5's now window (the
    refresh endpoint renews an expired ticket: the link never expires)."""
    reason, t = decode_ticket(p)
    if reason != OK:
        return reason, None
    k = ks.key(t.key_gen)
    if not k or k.role != ROLE_TICKET:
        return "unknown_key", None
    if ks.revoked_gen(t.key_gen) or t.key_gen < ks.min_key_gen:
        return "revoked_key", None
    if t.issued < k.not_before or t.issued > k.not_after:
        return "unknown_key", None
    if not verify(k.pub, TAG_TICKET, p[:TICKET_BODY], p[TICKET_BODY:]):
        return "bad_signature", None
    if t.expires < t.issued or t.expires - t.issued > TICKET_MAX_LIFETIME:
        return "lifetime", None
    if check_times and now + SKEW < t.issued:
        return "not_yet_valid", None
    if check_times and now > t.expires + SKEW:
        return "expired", None
    if not pubkey_strong(t.client_pub):
        return "bad_client_key", None
    if not name_ok(p[91:91 + p[90]]):
        return "bad_name", None
    return OK, t


# ---- section 9: the link proof of possession ----
def link_message(challenge: bytes, client_pub: bytes) -> bytes:
    """The bytes after the LINK tag: challenge[32] || client_pub[32]."""
    if len(challenge) != 32 or len(client_pub) != 32:
        raise ValueError("challenge and client_pub are 32 bytes")
    return challenge + client_pub


# ---- sections 5 and 6: the proof and call messages (M = tag || fields) ----
def proof_message(game_id: bytes, build_id: bytes, session_id: bytes, nonce_mine: bytes, nonce_peer: bytes,
                  my_ticket_id: bytes, peer_ticket_id: bytes, my_slot: int) -> bytes:
    return (TAG_PROOF + lp(game_id) + lp(build_id) + lp(session_id) + nonce_mine + nonce_peer + my_ticket_id
            + peer_ticket_id + bytes([my_slot]))


def call_message(host: bytes, method: bytes, path: bytes, body_sha: bytes, ts: int, nonce: bytes,
                 tid: bytes) -> bytes:
    return TAG_CALL + lp(host) + lp(method) + lp(path) + body_sha + le(ts, 8) + nonce + tid


def verify_call(client_pub: bytes, msg: bytes, sig: bytes) -> bool:
    """msg from call_message (it already begins with the CALL tag)."""
    return pubkey_strong(client_pub) and verify(client_pub, b"", msg, sig)


# ---- section 8: the deny list (PCDENY1 v1) ----
DENY_MAX = 65536


def deny_body(key_gen: int, list_serial: int, issued: int, valid_until: int, serials: list[int],
              accounts: list[bytes]) -> bytes:
    serials, accounts = sorted(set(serials)), sorted(set(accounts))
    if len(serials) > DENY_MAX or len(accounts) > DENY_MAX:
        raise ValueError("deny list over its caps")
    b = bytearray(DENY_MAGIC + le(1, 2) + le(0, 2) + le(key_gen, 4) + le(list_serial, 8) + le(issued, 8)
                  + le(valid_until, 8))
    b += le(len(serials), 4) + b"".join(le(s, 8) for s in serials)
    b += le(len(accounts), 4) + b"".join(accounts)
    return bytes(b)


def sign_deny(body: bytes, ticket_seed: bytes) -> bytes:
    return body + sign(ticket_seed, TAG_DENY, body)


def parse_deny(b: bytes, pub: bytes) -> tuple[str, Optional[dict]]:
    """For tests and admin checks: the layout, caps, sort order, no trailing bytes, and the signature."""
    head = 8 + 2 + 2 + 4 + 8 + 8 + 8
    if len(b) < head + 4 + 4 + 64 or b[:8] != DENY_MAGIC:
        return "unknown_format", None
    if get_le(b, 8, 2) != 1:
        return "newer_format", None
    if get_le(b, 10, 2) != 0:
        return "unknown_flags", None
    rd = _Reader(b, 12)
    key_gen, list_serial, issued, valid_until = rd.le(4), rd.le(8), rd.le(8), rd.le(8)
    n = rd.le(4)
    if n > DENY_MAX or len(b) - rd.at < n * 8 + 4 + 64:
        return "unknown_format", None
    serials = [rd.le(8) for _ in range(n)]
    m = rd.le(4)
    if m > DENY_MAX or len(b) - rd.at != m * 16 + 64:
        return "unknown_format", None
    accounts = [rd.take(16) for _ in range(m)]
    if serials != sorted(set(serials)) or accounts != sorted(set(accounts)):
        return "noncanonical", None
    if not verify(pub, TAG_DENY, b[:rd.at], b[rd.at:]):
        return "bad_signature", None
    return OK, dict(key_gen=key_gen, list_serial=list_serial, issued=issued, valid_until=valid_until,
                    serials=serials, accounts=accounts)
