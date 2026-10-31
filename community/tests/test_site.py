"""First-party site endpoints (/v1/site/*): signed calls, account lookup/create/rename, the assertion, limits."""
import base64
import json
import logging

import pytest

import tickets as T
from conftest import DISCORD_ID, NOW, b64

SITE = "meltyblood.org"
SEED = bytes([1]) * 32  # TEST seed only; the real site seed is never in the repo
NONCE_N = [0]


def fresh_nonce():
    NONCE_N[0] += 1
    return NONCE_N[0].to_bytes(16, "big")


def sign_headers(method, path, body, seed=SEED, site=SITE, ts=NOW, nonce=None):
    nonce = fresh_nonce() if nonce is None else nonce
    nb = base64.b64encode(nonce)
    msg = T.site_call_message(site.encode(), method.encode(), path.encode(), str(ts).encode(), nb, body)
    return {"X-PC-Site": site, "X-PC-Time": str(ts), "X-PC-Nonce": nb.decode(),
            "X-PC-Sig": b64(T.sign(seed, b"", msg)), "Content-Type": "application/json"}


def call(h, path, obj, **kw):
    body = json.dumps(obj, separators=(",", ":")).encode()
    return h.post(path, content=body, headers=sign_headers("POST", path, body, **kw))


@pytest.fixture
def s(h):
    h.cfg.stats_id_secret = "test-stats-secret"
    h.community.add_site(SITE, T.seed_public(SEED))
    return h


def test_pinned_vector():
    body = b'{"discord_id":"123456789012345678"}'
    nonce = bytes([2]) * 16
    msg = T.site_call_message(b"meltyblood.org", b"POST", b"/v1/site/account", b"1800000000",
                              base64.b64encode(nonce), body)
    assert T.seed_public(SEED).hex() == "8a88e3dd7409f195fd52db2d3cba5d72ca6709bf1d94121bf3748801b40f6f5c"
    assert base64.b64encode(nonce).decode() == "AgICAgICAgICAgICAgICAg=="
    assert T.sha256(body).hex() == "b8d6d9faacb431d3253c27bb4fc90bcb69eab2f5e59a9b8d45a8c249ae6aa714"
    assert msg.hex() == (
        "50432d434f4d4d554e4954592d5349544543414c4c2d56310a6d656c7479626c6f6f642e6f72670a504f53540a2f76312f736974"
        "652f6163636f756e740a313830303030303030300a416749434167494341674943416749434167494341673d3d0a62386436643966"
        "616163623433316433323533633237626234666339306263623639656162326635653539613962386434356138633234396165366161373134")
    assert b64(T.sign(SEED, b"", msg)) == (
        "jLTfRT3cEIq8AlVRg4KNO4+QeT8lB28zXSMY6pG8nk7gizuRcYHJvujciXjP1WpylsuKA6c45VnjCJxM3RTuDA==")


def test_vector_end_to_end_accepted(s):
    body = b'{"discord_id":"123456789012345678"}'
    hd = sign_headers("POST", "/v1/site/account", body, nonce=bytes([2]) * 16)
    assert hd["X-PC-Sig"] == ("jLTfRT3cEIq8AlVRg4KNO4+QeT8lB28zXSMY6pG8nk7gizuRcYHJvujciXjP1WpylsuKA6c45VnjCJxM3RTuDA==")
    r = s.post("/v1/site/account", content=body, headers=hd)
    assert r.status_code == 200 and r.json() == {"found": False}


def test_tags_prefix_free_and_present():
    assert T.TAG_SITECALL in T.ALL_TAGS and T.TAG_SITEACCT in T.ALL_TAGS
    for a in T.ALL_TAGS:
        for b in T.ALL_TAGS:
            assert a is b or not b.startswith(a)


def test_not_found_then_found_with_assertion(s):
    assert call(s, "/v1/site/account", {"discord_id": DISCORD_ID}).json() == {"found": False}
    link = s.link(bytes([0x51]) * 32, name="Skeleton").json()
    r = call(s, "/v1/site/account", {"discord_id": DISCORD_ID})
    assert r.status_code == 200, r.text
    j = r.json()
    assert j["found"] and j["name"] == "Skeleton" and j["display"] == link["display"] and j["tag"] == link["display"][-4:]
    assert base64.b64decode(j["account_id"]).hex() == link["account_id"]
    assert len(j["public_id"]) == 16 and j["stats_hidden"] is False and j["denied"] is False
    msg = T.site_account_message(SITE.encode(), base64.b64decode(j["account_id"]), j["name"], int(j["tag"]), j["issued"])
    assert msg.startswith(T.TAG_SITEACCT + b"meltyblood.org")
    assert T.verify(T.seed_public(s.key_seed), T.TAG_SITEACCT, msg[len(T.TAG_SITEACCT):], base64.b64decode(j["assertion"]))
    assert DISCORD_ID not in r.text


def test_denied_when_banned(s):
    s.link(bytes([0x51]) * 32, name="Skeleton")
    acc = s.community.find_account("Skeleton#" + s.community.db.one("SELECT tag FROM accounts")[0].__format__("04d"))
    s.community.ban(acc["id"], "x")
    assert call(s, "/v1/site/account", {"discord_id": DISCORD_ID}).json()["denied"] is True


def test_bad_signature_skew_replay_unknown_site(s):
    body = json.dumps({"discord_id": DISCORD_ID}).encode()
    p = "/v1/site/account"
    hd = sign_headers("POST", p, body)
    assert s.post(p, content=body + b" ", headers=hd).status_code == 401  # body changed
    assert s.post(p, content=body, headers=dict(hd, **{"X-PC-Time": str(NOW + 1)})).json()["error"] == "bad_site_signature"
    assert s.post(p, content=body, headers=sign_headers("POST", "/community" + p, body)).status_code == 401  # wrong path
    assert s.post(p, content=body, headers=sign_headers("POST", p, body, seed=bytes([9]) * 32)).status_code == 401
    r = s.post(p, content=body, headers=sign_headers("POST", p, body, ts=NOW - 121))
    assert r.status_code == 401 and r.json()["error"] == "clock_skew"
    assert s.post(p, content=body, headers=sign_headers("POST", p, body, ts=NOW + 120)).status_code == 200
    hd = sign_headers("POST", p, body)
    assert s.post(p, content=body, headers=hd).status_code == 200
    r = s.post(p, content=body, headers=hd)
    assert r.status_code == 401 and r.json()["error"] == "nonce_reused"
    r = s.post(p, content=body, headers=sign_headers("POST", p, body, site="evil.example"))
    assert r.status_code == 401 and r.json()["error"] == "unknown_site"
    assert s.post(p, content=body).status_code == 401  # no headers
    assert s.community.remove_site(SITE)
    assert s.post(p, content=body, headers=sign_headers("POST", p, body)).json()["error"] == "unknown_site"


def test_query_is_part_of_the_signed_path(s):
    body = b'{"discord_id":"1"}'
    hd = sign_headers("POST", "/v1/site/account?a=1", body)
    assert s.post("/v1/site/account?a=1", content=body, headers=hd).status_code == 200
    hd = sign_headers("POST", "/v1/site/account?a=1", body)
    assert s.post("/v1/site/account?a=2", content=body, headers=hd).status_code == 401


def test_bad_discord_id(s):
    for bad in ("", "abc", "12 3", 123, None, "9" * 21, "-5", "18446744073709551616"):
        r = call(s, "/v1/site/account", {"discord_id": bad})
        assert r.status_code == 400 and r.json()["error"] == "bad_discord_id", bad


def test_create_then_launcher_link_maps_to_same_account(s):
    r = call(s, "/v1/site/account/create", {"discord_id": DISCORD_ID, "name": "Skeleton"})
    assert r.status_code == 200, r.text
    j = r.json()
    assert j["found"] and j["created"] is True and j["name"] == "Skeleton"
    # the launcher: start -> link/discord with the SAME Discord user -> no name needed, same account, same name/tag
    seed = bytes([0x52]) * 32
    st = s.post("/v1/link/start", json={"client_pub": b64(T.seed_public(seed))}).json()
    d = s.post("/v1/link/discord", json={"link_id": st["link_id"], "access_token": "good-code"}).json()
    assert d["needs_name"] is False and d["display"] == j["display"]
    fin = s.post("/v1/link/finish", json={"link_id": st["link_id"], "signature": b64(
        T.sign(seed, T.TAG_LINK, T.link_message(base64.b64decode(st["challenge"]), T.seed_public(seed))))}).json()
    assert fin["account_id"] == base64.b64decode(j["account_id"]).hex() and fin["display"] == j["display"]
    assert fin["public_id"] == j["public_id"]


def test_create_existing_is_unchanged(s):
    a = call(s, "/v1/site/account/create", {"discord_id": DISCORD_ID, "name": "Skeleton"}).json()
    b = call(s, "/v1/site/account/create", {"discord_id": DISCORD_ID, "name": "Other"}).json()
    assert b["created"] is False and b["name"] == "Skeleton" and b["account_id"] == a["account_id"]
    s.link(bytes([0x51]) * 32, name=None)  # launcher link then keeps it
    assert call(s, "/v1/site/account", {"discord_id": DISCORD_ID}).json()["display"] == a["display"]


def test_create_name_rules_make_no_account(s):
    for bad in ("", "x" * 100, "ad\x00min", None):
        r = call(s, "/v1/site/account/create", {"discord_id": DISCORD_ID, "name": bad})
        assert r.status_code == 400 and r.json()["error"] == "bad_name", bad
    assert s.community.db.one("SELECT COUNT(*) FROM accounts")[0] == 0
    # the same code the launcher's finish returns
    s.link(bytes([0x51]) * 32, name="Skeleton")
    assert s.link(bytes([0x51]) * 32, name="ad\x00min").json()["error"] == "bad_name"


def test_rename(s):
    assert call(s, "/v1/site/account/name", {"discord_id": DISCORD_ID, "name": "Zed"}).status_code == 404
    c = call(s, "/v1/site/account/create", {"discord_id": DISCORD_ID, "name": "Skeleton"}).json()
    r = call(s, "/v1/site/account/name", {"discord_id": DISCORD_ID, "name": "Zed"})
    assert r.status_code == 200 and r.json()["name"] == "Zed" and r.json()["account_id"] == c["account_id"]
    assert call(s, "/v1/site/account/name", {"discord_id": DISCORD_ID, "name": "a\x00"}).json()["error"] == "bad_name"
    assert call(s, "/v1/site/account", {"discord_id": DISCORD_ID}).json()["name"] == "Zed"


def test_rate_limit_per_minute_and_day(s):
    for i in range(30):
        assert call(s, "/v1/site/account", {"discord_id": "1"}).status_code == 200, i
    r = call(s, "/v1/site/account", {"discord_id": "1"})
    assert r.status_code == 429 and r.json()["error"] == "rate_limited"
    s.clock.t += 61
    assert call(s, "/v1/site/account", {"discord_id": "1"}, ts=NOW + 61).status_code == 200
    s.community.SITE_PER_DAY = 31
    s.clock.t += 61
    assert call(s, "/v1/site/account", {"discord_id": "1"}, ts=NOW + 122).status_code == 429


def test_no_discord_id_in_logs(s, caplog):
    caplog.set_level(logging.DEBUG)
    did = "424242424242424242"
    call(s, "/v1/site/account/create", {"discord_id": did, "name": "Logless"})
    call(s, "/v1/site/account", {"discord_id": did})
    call(s, "/v1/site/account", {"discord_id": "bad"})
    assert did not in caplog.text and "bad" not in caplog.text.replace("bad_", "")
    assert did not in json.dumps(s.community.db.q("SELECT detail FROM audit") and
                                 [r[0] for r in s.community.db.q("SELECT detail FROM audit")])
