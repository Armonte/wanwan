"""LOCAL DEV MODE (COMMUNITY_FAKE_DISCORD=1): refused off loopback; a full link with a fake:<id> token, then a signed
refresh against a loopback public host with no root path (what a client with community.url http://127.0.0.1:8790
signs)."""
import base64
import os

import pytest
from fastapi.testclient import TestClient

import app as app_mod
import tickets as T
from conftest import NOW, ROOTS, Clock, b64, make_cfg, make_keyset
from service import Community, write_ticket_key

LOCAL = ("127.0.0.1", 50123)
BASE = "http://127.0.0.1:8790"


def fake_cfg(tmp, **over):
    vals = dict(fake_discord=True, bind_host="127.0.0.1", public_host="127.0.0.1", root_path="",
                discord_client_id="")
    vals.update(over)
    return make_cfg(str(tmp), **vals)


def build(tmp, peer=LOCAL, **over):
    cfg = fake_cfg(tmp, **over)
    seed = bytes([0x44]) * 32
    write_ticket_key(cfg.ticket_key_path, 1, seed)
    os.makedirs(cfg.keyset_dir)
    with open(os.path.join(cfg.keyset_dir, "keyset_000001.pckeys"), "wb") as f:
        f.write(make_keyset(T.seed_public(seed)))
    clock = Clock()
    a = app_mod.create_app(cfg, Community(cfg, clock=clock))
    return TestClient(a, base_url=BASE, client=peer), clock


@pytest.mark.parametrize("over", [dict(bind_host="0.0.0.0"), dict(bind_host=""), dict(bind_host="192.168.1.5"),
                                  dict(public_host="2dfm.org"), dict(public_host="example.com:8790")])
def test_refuses_to_start_off_loopback(tmp_path, over):
    with pytest.raises(RuntimeError, match="COMMUNITY_FAKE_DISCORD"):
        app_mod.create_app(fake_cfg(tmp_path, **over), Community(fake_cfg(tmp_path, **over)))


def test_loopback_forms_accepted(tmp_path):
    from fake_discord import host_is_loopback
    for h in ("127.0.0.1", "127.0.0.1:8790", "localhost", "::1", "[::1]:8790", "127.5.5.5"):
        assert host_is_loopback(h), h
    for h in ("", "0.0.0.0", "2dfm.org", "10.0.0.1", "[::]", "localhost.evil.com"):
        assert not host_is_loopback(h), h


def test_config_from_env(monkeypatch, tmp_path):
    from config import Config
    monkeypatch.setenv("COMMUNITY_CONFIG", str(tmp_path / "none.env"))
    monkeypatch.setenv("COMMUNITY_FAKE_DISCORD", "1")
    monkeypatch.setenv("COMMUNITY_BIND_HOST", "0.0.0.0")
    monkeypatch.setenv("COMMUNITY_PUBLIC_HOST", "127.0.0.1")
    cfg = Config.from_env()
    assert cfg.fake_discord
    with pytest.raises(RuntimeError):
        app_mod.create_app(cfg, Community(make_cfg(str(tmp_path))))


def test_non_loopback_peer_and_proxied_requests_refused(tmp_path):
    c, _ = build(tmp_path, peer=("203.0.113.9", 4000))
    r = c.get("/health")
    assert r.status_code == 403 and r.json()["error"] == "fake_discord_local_only"
    c2, _ = build(tmp_path / "b")
    r = c2.post("/v1/link/start", json={"client_pub": b64(T.seed_public(bytes([0x51]) * 32))},
                headers={"X-Forwarded-For": "203.0.113.9"})
    assert r.status_code == 403


def test_full_link_with_a_fake_token_then_refresh(tmp_path):
    c, clock = build(tmp_path)
    assert c.get("/health").json()["fake_discord"] is True
    seed = bytes([0x52]) * 32
    pub = T.seed_public(seed)
    st = c.post("/v1/link/start", json={"client_pub": b64(pub)})
    assert st.status_code == 200, st.text  # no Discord client id needed
    j = st.json()
    d = c.post("/v1/link/discord", json={"link_id": j["link_id"], "access_token": "fake:4242:Tester"})
    assert d.json() == {"status": "authorized", "needs_name": True, "suggested_name": "Tester",
                        "discord_handle": "tester"}
    sig = T.sign(seed, T.TAG_LINK, T.link_message(base64.b64decode(j["challenge"]), pub))
    fin = c.post("/v1/link/finish", json={"link_id": j["link_id"], "signature": b64(sig), "name": "DevPlayer"})
    assert fin.status_code == 200, fin.text
    ticket = base64.b64decode(fin.json()["ticket"])
    r, ks = T.parse_keyset(c.get("/v1/keyset").content, ROOTS, 0, NOW)
    assert r == T.OK and T.verify_ticket(ticket, ks, NOW)[0] == T.OK
    # the signed refresh: host 127.0.0.1 (no port) and path /v1/ticket/refresh (no prefix), as the client signs
    nonce, body = b"\x05" * 16, b""
    msg = T.call_message(b"127.0.0.1", b"POST", b"/v1/ticket/refresh", T.sha256(body), clock.t, nonce,
                         T.ticket_id(ticket))
    rr = c.post("/v1/ticket/refresh", content=body, headers={
        "X-PC-Ticket": b64(ticket), "X-PC-Time": str(clock.t), "X-PC-Nonce": b64(nonce),
        "X-PC-Sig": b64(T.sign(seed, b"", msg))})
    assert rr.status_code == 200, rr.text
    assert rr.json()["display"] == fin.json()["display"]


def test_bad_fake_tokens_and_off_by_default(tmp_path, h):
    c, _ = build(tmp_path / "fake")
    lid = c.post("/v1/link/start", json={"client_pub": b64(T.seed_public(bytes([0x53]) * 32))}).json()["link_id"]
    for tok in ("real-discord-token", "fake:", "fake:" + "x" * 65):
        r = c.post("/v1/link/discord", json={"link_id": lid, "access_token": tok})
        assert r.status_code == 401 and r.json()["error"] == "discord_token_invalid", tok
    assert "fake_discord" not in h.get("/health").json()
    lid = h.post("/v1/link/start", json={"client_pub": b64(T.seed_public(bytes([0x54]) * 32))}).json()["link_id"]
    assert h.post("/v1/link/discord", json={"link_id": lid, "access_token": "fake:1"}).status_code == 401
