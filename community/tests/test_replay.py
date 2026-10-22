"""POST /v1/replay: participant-only, first upload wins, the caps (per file, per account, the global breaker), the PCRZ1 /
inputs-only .pcrep validation, the URL composed from config, the results.jsonl replay_url, and nothing private in an
answer. R2 is a fake: no network."""
import base64
import secrets
import struct
import time
import zlib

import pytest

import receipts as R
import replays as RP
import tickets as T
from conftest import DISCORD_ID, NOW, b64
from report_fixture import BUILD, CA, CB, CX, GAME, body, latest, mk, report, setup, started_pair

P, RES = R.PROGRESS, R.RESULT
BASE = "https://replays.example.test"


class FakeStore:
    def __init__(self, fail=False):
        self.puts, self.fail = [], fail

    def put(self, key, data):
        if self.fail:
            raise RuntimeError("R2 down")
        self.puts.append((key, data))


def chunk(ctype, payload):
    return struct.pack("<BBHI", ctype, 0, len(payload), zlib.crc32(payload) & 0xFFFFFFFF) + payload


def pcrep(mi=3, frames=1500, extra=(), end=True, input_size=2, version=14):
    head = RP.PCREP_HEADER.pack(b"PCRP", version, 88, b"FM2K".ljust(16, b"\0"), 2, input_size, 232, 2, 8, 0, 0, NOW,
                                bytes(32), 0, 0, 0, 0)
    names = bytes([2, 0, 0, 0]) + b"Alice".ljust(32, b"\0") + b"Bob".ljust(32, b"\0") + bytes(64)
    out = head + chunk(RP.CH_SESSION, struct.pack("<QBB6x", 77, 2, 0)) + chunk(RP.CH_SESSION_NAMES, names)
    out += chunk(RP.CH_MATCH_START, struct.pack("<HBB", mi, 2, 0) + bytes(32))
    out += chunk(RP.CH_TAPE, struct.pack("<iH", 0, 6) + bytes(24))
    for c in extra:
        out += c
    if end:
        out += chunk(RP.CH_MATCH_END, struct.pack("<HbB4BI", mi, 0, 0, 2, 1, 0, 0, frames))
    return out + chunk(RP.CH_END, b"")


def pcrz(raw=None, mi=3, frames=1500, game=GAME, build=BUILD):
    return RP.encode_pcrz(game, build, mi, frames, pcrep(mi, frames) if raw is None else raw)


def upload(h, seed, ticket, data, match_id, signed_query=None):
    path = f"/v1/replay?match_id={match_id}"
    nonce = secrets.token_bytes(16)
    signed = "/community/v1/replay?" + (signed_query if signed_query is not None else f"match_id={match_id}")
    msg = T.call_message(b"2dfm.org", b"POST", signed.encode(), T.sha256(data), h.clock.t, nonce, T.ticket_id(ticket))
    return h.post(path, content=data, headers={"X-PC-Ticket": b64(ticket), "X-PC-Time": str(h.clock.t),
                                               "X-PC-Nonce": b64(nonce), "X-PC-Sig": b64(T.sign(seed, b"", msg)),
                                               "Content-Type": "application/octet-stream"})


def recorded(h, ta, n=0):
    rs = started_pair(n=n) + [mk(P, 0, 600, n=n), mk(P, 1, 600, n=n), mk(P, 0, 1200, (1, 1), n=n),
                              mk(P, 1, 1200, (1, 1), n=n), mk(RES, 0, 1500, (2, 1), 0, n=n)]
    j = report(h, CA, ta, body(*rs)).json()
    assert j["status"] == "pending", j
    return j["match_id"]


@pytest.fixture
def env(h, tmp_path):
    ta, tb, a, b = setup(h, tmp_path)
    h.cfg.r2_endpoint, h.cfg.r2_bucket = "https://r2.example.test", "bucket"
    h.cfg.r2_access_key_id, h.cfg.r2_secret_access_key = "kid", "secret"
    h.cfg.replay_public_base = BASE + "/"
    h.app.state.replays.store = FakeStore()
    h.clock.t = NOW + 600
    return ta, tb


def err(r):
    return r.status_code, r.json().get("error")


def test_upload_first_wins_and_exports(h, tmp_path, env):
    ta, tb = env
    mid = recorded(h, ta)
    r = upload(h, CB, tb, pcrz(), mid)  # either participant may upload
    assert r.status_code == 200, r.text
    j = r.json()
    assert set(j) == {"replay_id", "replay_url"}
    key = j["replay_url"][len(BASE) + 1:]
    assert j["replay_url"] == f"{BASE}/{key}"
    yyyymm = time.strftime("%Y-%m", time.gmtime(NOW + 60))
    assert key == f"replays/v1/fm2k/{GAME}/{yyyymm}/{j['replay_id']}.pcrz" and len(j["replay_id"]) == 16
    store = h.app.state.replays.store
    assert store.puts == [(key, pcrz())]
    row = h.community.db.one("SELECT key, status FROM replays")
    assert tuple(row) == (key, "stored")  # the key, never the URL
    assert latest(tmp_path)[mid]["replay_url"] == j["replay_url"]
    # a re-upload (other side, or the same body again) answers the same URL and stores nothing
    assert upload(h, CA, ta, pcrz(frames=1499), mid).json() == j
    assert len(store.puts) == 1
    # the URL follows the configured base (composed at answer and export time)
    h.cfg.replay_public_base = "https://cdn.other.test"
    assert upload(h, CA, ta, pcrz(), mid).json()["replay_url"] == "https://cdn.other.test/" + key
    for resp in h.responses:
        assert DISCORD_ID.encode() not in resp.content and b"testclient" not in resp.content
        if resp.request.url.path == "/v1/replay":
            assert b"account" not in resp.content and b"uploader" not in resp.content


def test_participants_and_recorded_only(h, tmp_path, env):
    ta, tb = env
    c = h.link(CX, name="Carol", code="third-code").json()
    tc = base64.b64decode(c["ticket"])
    assert err(upload(h, CA, ta, pcrz(), "0123456789ab")) == (409, "match_unknown")
    assert err(upload(h, CA, ta, pcrz(), "nope")) == (400, "bad_match_id")
    mid = recorded(h, ta)
    assert err(upload(h, CX, tc, pcrz(), mid)) == (403, "not_participant")
    # an open match (no result yet) is not recorded: retryable 409
    open_mid = report(h, CA, ta, body(*started_pair(n=1), mk(P, 0, 600, n=1))).json()["match_id"]
    assert err(upload(h, CA, ta, pcrz(), open_mid)) == (409, "match_unknown")
    # a match settled as not counted is final: 410, so the launcher's queue stops retrying
    h.community.db.x("UPDATE matches SET status = 'void' WHERE match_id = ?", (open_mid,))
    assert err(upload(h, CA, ta, pcrz(), open_mid)) == (410, "match_closed")
    # the query is part of the signed path: a match_id swapped in transit does not verify
    assert err(upload(h, CA, ta, pcrz(), mid, signed_query="match_id=0123456789ab")) == (401, "bad_call_signature")
    assert h.app.state.replays.store.puts == []


def test_disabled_without_r2(h, tmp_path, env):
    ta, _ = env
    mid = recorded(h, ta)
    h.cfg.r2_bucket = ""
    assert err(upload(h, CA, ta, pcrz(), mid)) == (503, "replays_disabled")
    assert h.get("/health").json()["replays_configured"] is False


def test_bad_files(h, tmp_path, env):
    ta, _ = env
    mid = recorded(h, ta)
    good = pcrz()

    def bad(data, why=None):
        r = upload(h, CA, ta, data, mid)
        assert err(r) == (400, "bad_replay"), r.text
        if why:
            assert why in r.json()["detail"]
    bad(b"XXXX" + good[4:], "not a PCRZ")
    bad(good[:4] + struct.pack("<H", 2) + good[6:], "version")
    bad(good[:-1], "z_len")
    bad(good + b"\0", "z_len")
    bad(pcrz(game="othergame"), "differ")
    bad(pcrz(mi=4), "match_index")
    bad(pcrz(frames=180001), "frames")
    bad(pcrz(raw=pcrep(extra=[chunk(RP.CH_CHAT, struct.pack("<ibB", 10, 0, 2) + b"hi")])), "CH_CHAT")
    bad(pcrz(raw=pcrep(extra=[chunk(RP.CH_GEN_STATE, bytes(12))])), "CH_GEN_STATE")
    bad(pcrz(raw=pcrep(extra=[chunk(RP.CH_GEN_STATE_Z, bytes(20))])), "CH_GEN_STATE_Z")
    bad(pcrz(raw=pcrep(extra=[chunk(99, b"x")])), "type 99")
    bad(pcrz(raw=pcrep(version=13)), "pcrep version")
    bad(pcrz(raw=pcrep(input_size=4)), "geometry")
    bad(pcrz(raw=pcrep(frames=1400)), "CH_MATCH_END")
    raw = bytearray(pcrep())
    raw[-12] ^= 1  # a payload byte of CH_MATCH_END
    bad(pcrz(raw=bytes(raw)), "CRC")
    # decompression bomb: raw_len is checked against RAW_CAP, and a stream bigger than raw_len is refused
    bomb_raw = pcrep() + bytes(RP.RAW_CAP)
    bomb = pcrz(raw=bomb_raw)
    assert len(bomb) < (1 << 20)
    bad(bomb, "raw_len")
    lie = bytearray(pcrz(raw=pcrep() + bytes(1 << 20)))
    struct.pack_into("<I", lie, 16, len(pcrep()))  # declares far less than the stream holds
    bad(bytes(lie), "decompressed size")
    assert h.app.state.replays.store.puts == []
    assert h.community.db.one("SELECT COUNT(*) FROM replays")[0] == 0


def test_size_cap_before_reading(h, tmp_path, env):
    ta, _ = env
    mid = recorded(h, ta)
    r = upload(h, CA, ta, b"\0" * ((1 << 20) + 1), mid)
    assert err(r) == (413, "body_too_large")


def test_account_cap_and_global_breaker(h, tmp_path, env):
    ta, tb = env
    h.cfg.replay_account_daily = 2
    mids = [recorded(h, ta, n=40 + i) for i in range(4)]
    assert upload(h, CA, ta, pcrz(), mids[0]).status_code == 200
    assert upload(h, CA, ta, pcrz(), mids[1]).status_code == 200
    r = upload(h, CA, ta, pcrz(), mids[2])
    assert err(r) == (429, "cap_daily") and int(r.headers["Retry-After"]) >= 60
    # the cap is per account: B still uploads; a re-upload past the cap still answers its URL
    assert upload(h, CB, tb, pcrz(), mids[2]).status_code == 200
    assert upload(h, CA, ta, pcrz(), mids[0]).status_code == 200
    # the global breaker on count, then on bytes
    h.cfg.replay_daily_uploads = 3
    r = upload(h, CB, tb, pcrz(), mids[3])
    assert err(r) == (503, "replays_paused") and int(r.headers["Retry-After"]) >= 60
    h.cfg.replay_daily_uploads = 100
    h.cfg.replay_daily_bytes = 3 * len(pcrz()) + 10
    assert err(upload(h, CB, tb, pcrz(), mids[3])) == (503, "replays_paused")
    # the next UTC day the caps reset
    h.cfg.replay_daily_bytes = 2 << 30
    h.clock.t += 86400
    assert upload(h, CB, tb, pcrz(), mids[3]).status_code == 200
    assert len(h.app.state.replays.store.puts) == 4


def test_store_failure_releases_the_reservation(h, tmp_path, env):
    ta, _ = env
    mid = recorded(h, ta)
    h.app.state.replays.store = FakeStore(fail=True)
    assert err(upload(h, CA, ta, pcrz(), mid)) == (502, "replay_store_failed")
    assert h.community.db.one("SELECT COUNT(*) FROM replays")[0] == 0
    assert latest(tmp_path)[mid]["replay_url"] is None
    h.app.state.replays.store = FakeStore()
    assert upload(h, CA, ta, pcrz(), mid).status_code == 200


def test_in_progress_and_stale_reservation(h, tmp_path, env):
    ta, _ = env
    mid = recorded(h, ta)
    m = h.community.db.one("SELECT id FROM matches WHERE match_id = ?", (mid,))[0]
    acct = h.community.db.one("SELECT id FROM accounts WHERE name = 'Alice'")[0]
    h.community.db.x("INSERT INTO replays(match, replay_id, key, uploader, bytes, raw_bytes, frames, sha, status, "
                     "created) VALUES(?, 'ffffffffffffffff', 'k', ?, 1, 1, 1, x'00', 'uploading', ?)",
                     (m, acct, h.clock.t))
    r = upload(h, CA, ta, pcrz(), mid)
    assert err(r) == (409, "replay_busy") and r.headers["Retry-After"] == "30"
    h.clock.t += RP.STALE_RESERVATION + 1
    assert upload(h, CA, ta, pcrz(), mid).status_code == 200


def test_r2_store_put_arguments(monkeypatch):
    calls = {}

    class Client:
        def put_object(self, **kw):
            calls.update(kw)

    import boto3

    def fake_client(service, **kw):
        calls["service"], calls["endpoint"] = service, kw["endpoint_url"]
        return Client()
    monkeypatch.setattr(boto3, "client", fake_client)
    from conftest import make_cfg
    cfg = make_cfg("/nonexistent", r2_endpoint="https://acct.r2.cloudflarestorage.com", r2_bucket="b",
                   r2_access_key_id="k", r2_secret_access_key="s")
    RP.R2Store(cfg).put("replays/v1/x.pcrz", b"data")
    assert calls["service"] == "s3" and calls["endpoint"] == "https://acct.r2.cloudflarestorage.com"
    assert calls["Bucket"] == "b" and calls["Key"] == "replays/v1/x.pcrz" and calls["Body"] == b"data"
    assert calls["ContentType"] == "application/octet-stream"
    assert calls["CacheControl"] == "public, max-age=31536000, immutable"


def test_local_store_dev_mode(h, tmp_path, env):
    """COMMUNITY_REPLAY_LOCAL_DIR: the upload lands as a file under the directory (no R2 values needed), keyed exactly
    like R2; anything that is not an object key, or leaves the root, is refused."""
    ta, _ = env
    h.cfg.r2_endpoint = h.cfg.r2_bucket = h.cfg.r2_access_key_id = h.cfg.r2_secret_access_key = ""
    h.cfg.replay_local_dir = str(tmp_path / "store")
    assert h.cfg.replays_missing() == []
    h.app.state.replays.store = RP.LocalStore(h.cfg.replay_local_dir)
    mid = recorded(h, ta)
    r = upload(h, CA, ta, pcrz(), mid)
    assert r.status_code == 200, r.text
    key = r.json()["replay_url"][len(BASE) + 1:]
    with open(tmp_path / "store" / key, "rb") as f:
        assert f.read() == pcrz()
    st = h.app.state.replays.store
    for bad in ("../x.pcrz", "replays/v1/../../" + key.split("/", 2)[2], "replays/v1/x/y/2026-01/zz.pcrz", ""):
        with pytest.raises(ValueError):
            st.path(bad)
    with pytest.raises(ValueError):
        st.path("replays/v1/../g/2026-01/0123456789abcdef.pcrz")
    h.cfg.replay_public_base = ""
    assert h.cfg.replays_missing() == ["COMMUNITY_REPLAY_PUBLIC_BASE"]
