"""Silent renewal (section 6 signed calls), the deny list (bans and revoked serials), and the privacy scan: no
response anywhere carries a Discord id, a Discord name or a client IP."""
import base64
import json

import admin
import tickets as T
from conftest import DISCORD_ID, DISCORD_NAME, NOW, b64

C1 = bytes([0x61]) * 32


def linked(h):
    r = h.link(C1, name="Skeleton")
    assert r.status_code == 200, r.text
    return base64.b64decode(r.json()["ticket"])


def test_refresh_returns_a_new_ticket(h):
    old = linked(h)
    h.clock.t += 24 * 3600
    r = h.refresh(C1, old)
    assert r.status_code == 200, r.text
    new = base64.b64decode(r.json()["ticket"])
    rr, t = T.verify_ticket(new, h.keyset(), h.clock.t)
    o = T.decode_ticket(old)[1]
    assert rr == T.OK and t.account == o.account and t.client_pub == o.client_pub and t.serial > o.serial
    assert t.issued == h.clock.t and r.json()["refresh_after"] == 24 * 3600


def test_refresh_of_a_long_expired_ticket_still_works(h):
    old = linked(h)
    h.clock.t += 30 * 86400  # offline a month: the link never expires
    r = h.refresh(C1, old)
    assert r.status_code == 200, r.text


def test_refresh_refusals(h):
    t = linked(h)
    assert h.refresh(C1, t, nonce=b"\x01" * 16).status_code == 200
    r = h.refresh(C1, t, nonce=b"\x01" * 16)
    assert r.status_code == 401 and r.json()["error"] == "nonce_reused"
    assert "X-PC-Server-Time" in r.headers
    assert h.refresh(C1, t, nonce=b"\x02" * 16, ts=NOW + 301).json()["error"] == "clock_skew"
    assert h.refresh(C1, t, nonce=b"\x03" * 16, ts=NOW - 300).status_code == 200
    assert h.refresh(C1, t, nonce=b"\x04" * 16, host="staging.2dfm.org").json()["error"] == "bad_call_signature"
    assert h.refresh(C1, t, nonce=b"\x05" * 16, signed_path="/v1/ticket/refresh").json()["error"] == \
        "bad_call_signature"  # the public path includes /community
    assert h.refresh(bytes([0x62]) * 32, t, nonce=b"\x06" * 16).json()["error"] == "bad_call_signature"
    forged = bytearray(t)
    forged[91] ^= 1
    assert h.refresh(C1, bytes(forged), nonce=b"\x07" * 16).json()["error"] == "relink_required"
    r = h.post("/v1/ticket/refresh", headers={"X-PC-Ticket": b64(t), "X-PC-Time": "x", "X-PC-Nonce": b64(b"1" * 16),
                                              "X-PC-Sig": b64(b"1" * 64)})
    assert r.json()["error"] == "bad_time"


def test_refresh_body_is_signed(h):
    t = linked(h)
    ts, nonce = NOW, b"\x08" * 16
    msg = T.call_message(b"2dfm.org", b"POST", b"/community/v1/ticket/refresh", T.sha256(b"{}"), ts, nonce,
                         T.ticket_id(t))
    sig = T.sign(C1, b"", msg)
    hdr = {"X-PC-Ticket": b64(t), "X-PC-Time": str(ts), "X-PC-Nonce": b64(nonce), "X-PC-Sig": b64(sig)}
    assert h.post("/v1/ticket/refresh", content=b"{ }", headers=hdr).json()["error"] == "bad_call_signature"
    assert h.post("/v1/ticket/refresh", content=b"{}", headers=hdr).status_code == 200


def test_ban_refuses_refresh_and_lists_the_account(h, monkeypatch, capsys):
    t = linked(h)
    acct = T.decode_ticket(t)[1]
    monkeypatch.setattr(admin, "Community", lambda cfg: h.community)
    monkeypatch.setattr(admin.Config, "from_env", classmethod(lambda cls: h.cfg))
    assert admin.main(["ban", acct.display, "--reason", "test"]) == 0
    r = h.refresh(C1, t)
    assert r.status_code == 403 and r.json()["error"] == "banned"
    rr, d = T.parse_deny(h.get("/v1/deny").content, T.seed_public(h.key_seed))
    assert rr == T.OK and acct.account in d["accounts"]
    assert admin.main(["unban", acct.account.hex()]) == 0
    rr, d2 = T.parse_deny(h.get("/v1/deny").content, T.seed_public(h.key_seed))
    assert acct.account not in d2["accounts"] and d2["list_serial"] > d["list_serial"]
    assert h.refresh(C1, t, nonce=b"\x0a" * 16).status_code == 200
    assert admin.main(["stats"]) == 0
    assert '"banned": 0' in capsys.readouterr().out


def test_revoked_serial_rides_the_deny_list_until_it_expires(h):
    t = linked(h)
    tk = T.decode_ticket(t)[1]
    assert h.community.revoke_serial(1, tk.serial)
    pub = T.seed_public(h.key_seed)
    rr, d = T.parse_deny(h.get("/v1/deny").content, pub)
    assert rr == T.OK and d["serials"] == [tk.serial] and d["key_gen"] == 1
    assert d["valid_until"] - d["issued"] == 86400
    assert h.refresh(C1, t).json()["error"] == "relink_required"
    h.clock.t = tk.expires + 301
    rr, d = T.parse_deny(h.get("/v1/deny").content, pub)
    assert d["serials"] == []  # only unexpired serials are listed


def test_deny_list_republishes_on_schedule_with_monotone_serial(h):
    pub = T.seed_public(h.key_seed)
    a = T.parse_deny(h.get("/v1/deny").content, pub)[1]
    b = T.parse_deny(h.get("/v1/deny").content, pub)[1]
    assert a == b  # unchanged content within 15 minutes: the same signed list
    h.clock.t += 901
    c = T.parse_deny(h.get("/v1/deny").content, pub)[1]
    assert c["list_serial"] == a["list_serial"] + 1 and c["issued"] == h.clock.t


def test_deny_list_tamper_detected(h):
    linked(h)
    b = bytearray(h.get("/v1/deny").content)
    b[20] ^= 1
    assert T.parse_deny(bytes(b), T.seed_public(h.key_seed))[0] == "bad_signature"


def test_no_discord_id_name_or_ip_in_any_response(h):
    h.app.state.discord.users["good-code"] = DISCORD_ID
    t = linked(h)
    h.refresh(C1, t, nonce=b"\x0b" * 16)
    h.post("/v1/link/start", json={"client_pub": b64(T.seed_public(C1))}, headers={"X-Forwarded-For": "203.0.113.77"})
    h.get("/health")
    h.get("/v1/keyset")
    h.get("/v1/deny")
    h.refresh(C1, t, nonce=b"\x0b" * 16)  # an error response too
    needles = [DISCORD_ID.encode(), DISCORD_NAME.encode(), b"203.0.113.77", b"testclient",
               DISCORD_ID.encode().hex().encode(), int(DISCORD_ID).to_bytes(8, "little")]
    assert len(h.responses) > 8
    for r in h.responses:
        blob = r.content + json.dumps(dict(r.headers)).encode()
        for n in needles:
            assert n not in blob, (r.request.url, n)
    # and the server-side record holds it only in accounts.discord_id
    assert h.community.db.one("SELECT COUNT(*) FROM accounts WHERE discord_id = ?", (DISCORD_ID,))[0] == 1
