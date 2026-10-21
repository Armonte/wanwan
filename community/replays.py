"""POST /v1/replay: shared replays (STATS_DESIGN.md 4.6 with the owner's 2026-10-21 rulings). Inputs-only, compressed,
attached to a RECORDED match, public to anyone, kept forever, player names kept as-is. Stored in Cloudflare R2 through
the S3 API; the database keeps the object key, never the URL.

The body is one PCRZ1 file (README "Shared replays"):

    0   4  magic "PCRZ"
    4   u16 version (1)
    6   u16 header_len (bytes before the payload: 26 + len(game_id) + len(build_id) in v1)
    8   u32 match_index        must equal the match's (the receipts' match_index)
    12  u32 frames             battle frames of the match, 1..max_match_frames of the game row
    16  u32 raw_len            the decompressed size, 1..RAW_CAP
    20  u32 z_len              header_len + z_len == the body length exactly
    24  u8  game_id_len, game_id (ASCII, the catalog id the receipts carry)
        u8  build_id_len, build_id (ASCII, the receipts' build_id)
    header_len: one zstd frame of `raw_len` bytes: an inputs-only .pcrep (pc-replay ReplayFormat.hpp, version 14)

The inner .pcrep must be complete and CRC-clean, hold exactly one CH_SESSION before exactly one CH_MATCH_START of this
match index (at most one CH_MATCH_END of the same index whose frames equal the wrapper's), use the game row's input
width, and carry only allowed chunk types: no CH_GEN_STATE / CH_GEN_STATE_Z (snapshots) and no CH_CHAT.

Money guard (the owner: nobody can drain it): every limit below is checked before a byte goes to R2, and a stored
object is the only thing that costs. 1 MiB per body (Content-Length before reading, and while streaming); one object
per match (first valid upload wins, a re-upload stores nothing); only a participant of a recorded match; a per-account
daily cap (429 cap_daily); a global daily breaker on count and bytes (503 replays_paused); the per-client rate limit.
Decompression is bounded (RAW_CAP, an 8 MiB zstd window) and never parses past the cap.
"""
from __future__ import annotations

import re
import secrets
import struct
import time
import zlib
import tickets as T
from service import ServiceError

MAGIC = b"PCRZ"
VERSION = 1
FIXED = 24  # magic .. z_len
RAW_CAP = 16 << 20
ZSTD_WINDOW_MAX = 8 << 20
RECORDED = ("pending", "final", "left_early")
STALE_RESERVATION = 300  # an `uploading` row older than this (a crash mid-PutObject) is reclaimed
DAY = 86400
MATCH_ID_RE = re.compile(r"[0-9a-f]{12}")
SAFE_ID = re.compile(r"[A-Za-z0-9._-]{1,64}")

# pc-replay ReplayFormat.hpp (kVersion 14, kMinReadableVersion 14)
PCREP_MAGIC = b"PCRP"
PCREP_VERSIONS = (14,)
PCREP_HEADER = struct.Struct("<4sHH16sBBHBBBBQ32sQIHH")  # FileHeader, 88 bytes
CHUNK = struct.Struct("<BBHI")  # type, pad, len, crc32(payload)
CH_GEN, CH_INPUTS, CH_CHECK, CH_ROUND_END, CH_RETRY, CH_END, CH_GEN_STATE = 1, 2, 3, 4, 5, 6, 7
CH_TAPE, CH_TAPE_MARK, CH_TAPE_CHECK, CH_TAPE_END = 8, 9, 10, 11
CH_SESSION, CH_MATCH_START, CH_MATCH_END, CH_GEN_STATE_Z = 12, 13, 14, 15
CH_SETUP_EXT, CH_CSS_LIVE, CH_CHAT, CH_SESSION_NAMES, CH_MATCH_CHARS = 16, 17, 18, 19, 20
FORBIDDEN = {CH_GEN_STATE: "CH_GEN_STATE", CH_GEN_STATE_Z: "CH_GEN_STATE_Z", CH_CHAT: "CH_CHAT"}
ALLOWED = {CH_GEN, CH_INPUTS, CH_CHECK, CH_ROUND_END, CH_RETRY, CH_END, CH_TAPE, CH_TAPE_MARK, CH_TAPE_CHECK,
           CH_TAPE_END, CH_SESSION, CH_MATCH_START, CH_MATCH_END, CH_SETUP_EXT, CH_CSS_LIVE, CH_SESSION_NAMES,
           CH_MATCH_CHARS}
SIZES = {CH_SESSION: 16, CH_MATCH_START: 36, CH_MATCH_END: 12, CH_SESSION_NAMES: 132}


class BadReplay(ValueError):
    pass


def _bad(why: str):
    raise BadReplay(why)


def encode_pcrz(game_id: str, build_id: str, match_index: int, frames: int, raw: bytes, level: int = 19) -> bytes:
    """The launcher's side, for tests and tools: wrap an inputs-only .pcrep."""
    import zstandard
    z = zstandard.ZstdCompressor(level=level).compress(raw)
    g, b = game_id.encode("ascii"), build_id.encode("ascii")
    head_len = FIXED + 2 + len(g) + len(b)
    return (MAGIC + struct.pack("<HHIIII", VERSION, head_len, match_index, frames, len(raw), len(z)) +
            bytes([len(g)]) + g + bytes([len(b)]) + b + z)


def parse_header(body: bytes) -> dict:
    """The wrapper fields; BadReplay on anything off. Reads nothing past the declared sizes."""
    if len(body) < FIXED + 2 or body[:4] != MAGIC:
        _bad("not a PCRZ file")
    version, head_len, match_index, frames, raw_len, z_len = struct.unpack_from("<HHIIII", body, 4)
    if version != VERSION:
        _bad(f"PCRZ version {version} is not supported")
    pos = FIXED
    ids = []
    for _ in range(2):
        if pos >= len(body):
            _bad("truncated header")
        n = body[pos]
        s = body[pos + 1:pos + 1 + n]
        if not 1 <= n <= 64 or len(s) != n:
            _bad("bad game_id / build_id length")
        try:
            ids.append(s.decode("ascii"))
        except UnicodeDecodeError:
            _bad("game_id / build_id are not ASCII")
        pos += 1 + n
    if head_len != pos:
        _bad("header_len does not match the header")
    if z_len == 0 or head_len + z_len != len(body):
        _bad("z_len does not match the body length")
    if not 1 <= raw_len <= RAW_CAP:
        _bad("raw_len out of range")
    return {"game_id": ids[0], "build_id": ids[1], "match_index": match_index, "frames": frames,
            "raw_len": raw_len, "z": body[head_len:]}


def decompress(z: bytes, raw_len: int) -> bytes:
    """Exactly `raw_len` bytes from one zstd frame, never more than raw_len + 1 produced (decompression bomb)."""
    import zstandard
    try:
        dctx = zstandard.ZstdDecompressor(max_window_size=ZSTD_WINDOW_MAX)
        out = bytearray()
        with dctx.stream_reader(z, read_across_frames=False) as rd:
            while len(out) <= raw_len:
                chunk = rd.read(min(1 << 16, raw_len + 1 - len(out)))
                if not chunk:
                    break
                out += chunk
    except zstandard.ZstdError as e:
        _bad(f"zstd: {e}")
    if len(out) != raw_len:
        _bad("the decompressed size is not raw_len")
    return bytes(out)


def check_pcrep(raw: bytes, match_index: int, frames: int, input_width: int) -> dict:
    """Walk the inner .pcrep chunk by chunk (CRC'd). Returns counts for the record; BadReplay on anything refused."""
    if len(raw) < PCREP_HEADER.size:
        _bad("pcrep header truncated")
    (magic, version, head_bytes, _gid, num_players, input_size, rng_bytes, *_rest) = PCREP_HEADER.unpack_from(raw)
    if magic != PCREP_MAGIC:
        _bad("inner file is not a .pcrep")
    if version not in PCREP_VERSIONS:
        _bad(f"pcrep version {version} is not supported")
    if not PCREP_HEADER.size <= head_bytes <= 4096 or head_bytes > len(raw):
        _bad("pcrep headerBytes out of range")
    if not 2 <= num_players <= 4 or input_size != input_width:
        _bad("pcrep geometry does not match the game")
    pos, seen = head_bytes, {}
    order = []
    while pos < len(raw):
        if pos + CHUNK.size > len(raw):
            _bad("truncated chunk header")
        ctype, _pad, n, crc = CHUNK.unpack_from(raw, pos)
        pay = raw[pos + CHUNK.size:pos + CHUNK.size + n]
        if len(pay) != n:
            _bad("truncated chunk")
        if ctype in FORBIDDEN:
            _bad(f"{FORBIDDEN[ctype]} is not allowed in a shared replay")
        if ctype not in ALLOWED:
            _bad(f"chunk type {ctype} is not allowed")
        if zlib.crc32(pay) & 0xFFFFFFFF != crc:
            _bad("chunk CRC mismatch")
        if ctype in SIZES and n != SIZES[ctype]:
            _bad(f"chunk type {ctype} has the wrong size")
        seen[ctype] = seen.get(ctype, 0) + 1
        if ctype in (CH_SESSION, CH_MATCH_START, CH_MATCH_END, CH_SESSION_NAMES):
            order.append((ctype, pay))
        pos += CHUNK.size + n
    for ctype, name in ((CH_SESSION, "CH_SESSION"), (CH_MATCH_START, "CH_MATCH_START")):
        if seen.get(ctype, 0) != 1:
            _bad(f"exactly one {name} is required")
    for ctype, name in ((CH_MATCH_END, "CH_MATCH_END"), (CH_SESSION_NAMES, "CH_SESSION_NAMES")):
        if seen.get(ctype, 0) > 1:
            _bad(f"more than one {name}")
    kinds = [c for c, _ in order]
    if kinds.index(CH_SESSION) > kinds.index(CH_MATCH_START):
        _bad("CH_SESSION must come before CH_MATCH_START")
    start = dict(order)[CH_MATCH_START]
    if struct.unpack_from("<H", start)[0] != match_index:
        _bad("CH_MATCH_START is another match")
    if CH_MATCH_END in kinds:
        if kinds.index(CH_MATCH_END) < kinds.index(CH_MATCH_START):
            _bad("CH_MATCH_END before CH_MATCH_START")
        end = dict(order)[CH_MATCH_END]
        if struct.unpack_from("<H", end)[0] != match_index or struct.unpack_from("<I", end, 8)[0] != frames:
            _bad("CH_MATCH_END does not match the header")
    return {"chunks": sum(seen.values())}


def validate(body: bytes, match, game) -> dict:
    """The whole file against its match row and game row. Returns the header fields; BadReplay otherwise."""
    h = parse_header(body)
    if h["game_id"] != match["game_id"] or h["build_id"] != match["build_id"]:
        _bad("game_id / build_id differ from the match")
    if h["match_index"] != match["match_index"] or (h["match_index"] >> 16):
        _bad("match_index differs from the match")
    if not 1 <= h["frames"] <= int(game["max_match_frames"]):
        _bad("frames out of range for this game")
    raw = decompress(h["z"], h["raw_len"])
    check_pcrep(raw, h["match_index"], h["frames"], int(game["input_width"]))
    return h


def object_key(family: str, game_id: str, started_unix: int, replay_id: str) -> str:
    gid = game_id.split(":", 1)[-1]
    gid = gid if SAFE_ID.fullmatch(gid) else re.sub(r"[^A-Za-z0-9._-]", "_", gid)[:64]
    fam = family if SAFE_ID.fullmatch(family or "") else "unknown"
    return f"replays/v1/{fam}/{gid}/{time.strftime('%Y-%m', time.gmtime(started_unix))}/{replay_id}.pcrz"


class R2Store:
    """PutObject into the R2 bucket (S3 API). Tests replace Replays.store with a fake; nothing here runs without the
    four COMMUNITY_R2_* values."""

    def __init__(self, cfg):
        self.cfg = cfg
        self._client = None

    def client(self):
        if self._client is None:
            import boto3
            from botocore.config import Config as BotoConfig
            kw = dict(signature_version="s3v4", connect_timeout=5, read_timeout=30, retries={"max_attempts": 2})
            try:  # newer botocore sends default CRC checksums; send them only when required
                bc = BotoConfig(request_checksum_calculation="when_required",
                                response_checksum_validation="when_required", **kw)
            except TypeError:
                bc = BotoConfig(**kw)
            self._client = boto3.client("s3", endpoint_url=self.cfg.r2_endpoint, region_name="auto",
                                        aws_access_key_id=self.cfg.r2_access_key_id,
                                        aws_secret_access_key=self.cfg.r2_secret_access_key, config=bc)
        return self._client

    def put(self, key: str, body: bytes) -> None:
        self.client().put_object(Bucket=self.cfg.r2_bucket, Key=key, Body=body,
                                 ContentType="application/octet-stream",
                                 CacheControl="public, max-age=31536000, immutable")


def _day_start(now: int) -> int:
    return now - now % DAY


def _retry_tomorrow(e: ServiceError, now: int) -> ServiceError:
    e.headers = {"Retry-After": str(max(60, _day_start(now) + DAY - now))}
    return e


class Replays:
    def __init__(self, community, results):
        self.c = community
        self.db = community.db
        self.results = results
        self.store = R2Store(community.cfg)

    @property
    def cfg(self):
        return self.c.cfg

    def url(self, key: str) -> str:
        return self.cfg.replay_public_base.rstrip("/") + "/" + key

    def _answer(self, r) -> dict:
        return {"replay_id": r["replay_id"], "replay_url": self.url(r["key"])}

    def enabled(self) -> None:
        if self.cfg.replays_missing():
            raise ServiceError(503, "replays_disabled", "shared replays are not configured on this server")

    def _match(self, match_id: str):
        if not isinstance(match_id, str) or not MATCH_ID_RE.fullmatch(match_id):
            raise ServiceError(400, "bad_match_id", "match_id must be the 12-hex id /v1/report answered")
        return self.db.one("SELECT * FROM matches WHERE match_id = ?", (match_id,))

    def _gate(self, d, acct: int, m, now: int, size: int = 0):
        """Participant, recorded, and every cap. Returns the existing replay row (a re-upload) or None. `d` is the
        database (inside the reservation transaction on the real pass)."""
        if m is None:
            raise ServiceError(409, "match_unknown", "no recorded match with this id yet (retry after the report)")
        if acct not in (m["acct0"], m["acct1"]):
            raise ServiceError(403, "not_participant", "not a player of this match")
        r = d.one("SELECT * FROM replays WHERE match = ?", (m["id"],))
        if r is not None:
            if r["status"] == "stored":
                return r
            if r["created"] > now - STALE_RESERVATION:
                e = ServiceError(409, "replay_busy", "an upload for this match is in progress, retry shortly")
                e.headers = {"Retry-After": "30"}
                raise e
            d.x("DELETE FROM replays WHERE match = ?", (m["id"],))  # a crashed upload: reclaim
        if m["status"] not in RECORDED:
            raise ServiceError(409, "match_unknown", f"the match is {m['status']}, not recorded (yet)")
        day = _day_start(now)
        n, total = d.one("SELECT COUNT(*), COALESCE(SUM(bytes), 0) FROM replays WHERE created >= ?", (day,))
        if n >= self.cfg.replay_daily_uploads or total + size > self.cfg.replay_daily_bytes:
            raise _retry_tomorrow(ServiceError(503, "replays_paused", "replay uploads are paused until tomorrow"), now)
        mine = d.one("SELECT COUNT(*) FROM replays WHERE uploader = ? AND created >= ?", (acct, day))[0]
        if mine >= self.cfg.replay_account_daily:
            raise _retry_tomorrow(ServiceError(429, "cap_daily", "too many replay uploads today"), now)
        return None

    def precheck(self, acct: int, match_id: str) -> None:
        """Before the body is read: configuration, the match, the caps. The upload repeats every check atomically."""
        self.enabled()
        self._gate(self.db, acct, self._match(match_id), self.c.now())

    def upload(self, acct: int, match_id: str, body: bytes) -> dict:
        self.enabled()
        now = self.c.now()
        m = self._match(match_id)
        existing = self._gate(self.db, acct, m, now)
        if existing is not None:
            return self._answer(existing)  # first upload wins; nothing is stored again
        if len(body) > self.cfg.replay_max_bytes:
            raise ServiceError(413, "body_too_large", f"the body is above {self.cfg.replay_max_bytes} bytes")
        game = self.db.one("SELECT * FROM games WHERE game_id = ? AND build_id = ?", (m["game_id"], m["build_id"]))
        if game is None:
            raise ServiceError(409, "match_unknown", "this match's game row is gone")
        try:
            h = validate(body, m, game)
        except BadReplay as e:
            raise ServiceError(400, "bad_replay", str(e))
        replay_id = secrets.token_hex(8)
        key = object_key(game["family"], m["game_id"], m["started_unix"], replay_id)
        with self.db.tx() as d:  # reserve: the caps and first-wins hold under concurrent uploads
            existing = self._gate(d, acct, d.one("SELECT * FROM matches WHERE id = ?", (m["id"],)), now, len(body))
            if existing is not None:
                return self._answer(existing)
            d.x("INSERT INTO replays(match, replay_id, key, uploader, bytes, raw_bytes, frames, sha, status, created) "
                "VALUES(?,?,?,?,?,?,?,?, 'uploading', ?)",
                (m["id"], replay_id, key, acct, len(body), h["raw_len"], h["frames"], T.sha256(body), now))
        try:
            self.store.put(key, body)
        except Exception:
            self.db.x("DELETE FROM replays WHERE match = ? AND replay_id = ?", (m["id"], replay_id))
            raise ServiceError(502, "replay_store_failed", "could not store the replay, retry later")
        self.db.x("UPDATE replays SET status = 'stored' WHERE match = ?", (m["id"],))
        self.db.audit(now, "replay", f"match {m['match_id']} {key} {len(body)}B")
        self.results.export_match(m["id"])
        return {"replay_id": replay_id, "replay_url": self.url(key)}
