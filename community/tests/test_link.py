"""The link flow with a mocked Discord: start -> link/discord -> finish -> a ticket clients verify; names and
#tags; relink; refusals; missing configuration."""
import base64
import os

import tickets as T
from conftest import NOW, ROOTS, b64


def client(n):
    return bytes([0x50 + n]) * 32


def test_link_issues_a_ticket_clients_verify(h):
    r = h.link(client(1), name="Skeleton")
    assert r.status_code == 200, r.text
    j = r.json()
    ticket = base64.b64decode(j["ticket"])
    rr, t = T.verify_ticket(ticket, h.keyset(), NOW)
    assert rr == T.OK
    assert t.client_pub == T.seed_public(client(1)) and t.name == "Skeleton" and t.key_gen == 1
    assert t.expires - t.issued == 48 * 3600
    assert j["display"] == f"Skeleton#{t.tag:04d}" and j["account_id"] == t.account.hex()
    assert "expires" not in j  # no expiry is ever shown to the player
    assert r.headers["X-PC-Server-Time"] == str(NOW)


def test_second_link_same_discord_keeps_account_and_name(h):
    a = h.link(client(1), name="Skeleton").json()
    b = h.link(client(1), name=None).json()  # relink on the same key, no new name: keeps it
    assert a["account_id"] == b["account_id"] and a["display"] == b["display"]


def test_relink_with_new_client_key_denies_old_serials(h):
    a = h.link(client(1), name="Skeleton")
    old = base64.b64decode(a.json()["ticket"])
    b = h.link(client(2), name=None)
    new = base64.b64decode(b.json()["ticket"])
    assert T.decode_ticket(old)[1].account == T.decode_ticket(new)[1].account  # same account_id
    r, d = T.parse_deny(h.get("/v1/deny").content, T.seed_public(h.key_seed))
    assert r == T.OK and T.decode_ticket(old)[1].serial in d["serials"]
    assert T.decode_ticket(new)[1].serial not in d["serials"]
    assert h.refresh(client(1), old).json()["error"] == "relink_required"


def test_tags_unique_per_skeleton_and_confusables_flagged(h):
    a = h.link(client(1), name="Skeleton").json()
    h.app.state.discord.users["third-code"] = "222"
    b = h.link(client(2), name="SKELET0N", code="other-code").json()
    c = h.link(client(3), name="Skeleton", code="third-code").json()
    tags = {a["tag"], b["tag"], c["tag"]}
    assert len(tags) == 3
    flagged = h.community.db.q("SELECT name, flagged FROM accounts ORDER BY id")
    assert [r["flagged"] for r in flagged] == [0, 1, 1]


def test_name_rules_and_name_required(h):
    r = h.link(client(1), name="a‮b\x07")
    assert r.status_code == 400 and r.json()["error"] == "bad_name"
    r = h.link(client(1), name=None)
    assert r.status_code == 409 and r.json()["error"] == "name_required"
    r = h.link(client(1), name="  Ske​leton ")
    assert r.json()["name"] == "Skeleton"


def test_bad_link_proof_and_wrong_key(h):
    pub = T.seed_public(client(1))
    j = h.post("/v1/link/start", json={"client_pub": b64(pub)}).json()
    h.post("/v1/link/discord", json={"link_id": j["link_id"], "access_token": "good-code"})
    challenge = base64.b64decode(j["challenge"])
    other = T.sign(client(2), T.TAG_LINK, T.link_message(challenge, pub))
    r = h.post("/v1/link/finish", json={"link_id": j["link_id"], "signature": b64(other), "name": "X"})
    assert r.status_code == 401 and r.json()["error"] == "bad_link_proof"
    wrong_tag = T.sign(client(1), T.TAG_PROOF, T.link_message(challenge, pub))
    r = h.post("/v1/link/finish", json={"link_id": j["link_id"], "signature": b64(wrong_tag), "name": "X"})
    assert r.json()["error"] == "bad_link_proof"


def test_finish_before_discord_and_expired_link(h):
    pub = T.seed_public(client(1))
    j = h.post("/v1/link/start", json={"client_pub": b64(pub)}).json()
    sig = T.sign(client(1), T.TAG_LINK, T.link_message(base64.b64decode(j["challenge"]), pub))
    r = h.post("/v1/link/finish", json={"link_id": j["link_id"], "signature": b64(sig), "name": "X"})
    assert r.status_code == 409 and r.json()["error"] == "not_authorized"
    h.clock.t += 601
    r = h.post("/v1/link/discord", json={"link_id": j["link_id"], "access_token": "good-code"})
    assert r.status_code == 410 and r.json()["error"] == "link_expired"


def start(h, n=1):
    return h.post("/v1/link/start", json={"client_pub": b64(T.seed_public(client(n)))}).json()


def test_start_has_no_browser_sign_in(h):
    j = start(h)
    assert set(j) == {"link_id", "challenge", "expires_in"}
    assert h.get("/discord/callback").status_code == 404 and h.post("/v1/link/poll", json={}).status_code == 404


def test_link_discord_answers_and_suggests_the_discord_name(h):
    j = start(h)
    r = h.post("/v1/link/discord", json={"link_id": j["link_id"], "access_token": "good-code"})
    assert r.status_code == 200, r.text
    assert r.json() == {"status": "authorized", "needs_name": True, "suggested_name": "DiscordSecretName",
                        "discord_handle": "secretuser"}
    fin = h.link(client(1), name=r.json()["suggested_name"]).json()
    assert fin["display"].startswith("DiscordSecretName#") and fin["discord_handle"] == "secretuser"
    again = h.post("/v1/link/discord", json={"link_id": start(h)["link_id"], "access_token": "good-code"}).json()
    assert again["needs_name"] is False and again["display"] == fin["display"]


def test_suggestion_falls_back_to_username_then_empty(h):
    h.app.state.discord.names["111111111111111111"] = ("plainuser", "x" * 40)  # display name breaks the rules (too long)
    r = h.post("/v1/link/discord", json={"link_id": start(h)["link_id"], "access_token": "other-code"}).json()
    assert r["suggested_name"] == "plainuser"
    from app import suggest_name
    assert suggest_name({"global_name": "x" * 40, "username": ""}) == ""


def test_link_discord_refusals(h):
    j = start(h)
    post = lambda tok, lid=j["link_id"]: h.post("/v1/link/discord", json={"link_id": lid, "access_token": tok})
    assert post("nope").json()["error"] == "discord_token_invalid"
    assert post("other-app-token").json()["error"] == "discord_wrong_app"
    assert post("").json()["error"] == "bad_access_token" and post(None).json()["error"] == "bad_access_token"
    assert post("x" * 600).json()["error"] == "bad_access_token"
    assert post("good-code", lid="unknown").json()["error"] == "link_expired"
    assert h.community.db.one("SELECT COUNT(*) FROM accounts")[0] == 0  # nothing created by a refused token
    assert post("good-code").status_code == 200 and post("good-code").status_code == 200  # a retry is fine
    r = post("other-code")  # but the same link cannot switch to another Discord account
    assert r.status_code == 409 and r.json()["error"] == "link_used"


def test_used_link_cannot_be_reused(h):
    h.link(client(1), name="Skeleton")
    lid = h.community.db.one("SELECT link_id FROM links WHERE status = 'consumed'")[0]
    r = h.post("/v1/link/discord", json={"link_id": lid, "access_token": "good-code"})
    assert r.status_code == 409 and r.json()["error"] == "link_used"


def test_discord_token_is_never_stored(h):
    h.link(client(1), name="Skeleton")
    db = h.community.db
    for (table,) in db.q("SELECT name FROM sqlite_master WHERE type = 'table'"):
        for row in db.q(f"SELECT * FROM {table}"):
            assert all(v != "good-code" for v in tuple(row)), table


def test_refresh_returns_the_discord_handle(h):
    t = base64.b64decode(h.link(client(1), name="Skeleton").json()["ticket"])
    r = h.refresh(client(1), t)
    assert r.status_code == 200 and r.json()["discord_handle"] == "secretuser"


def test_start_refuses_weak_client_key(h):
    r = h.post("/v1/link/start", json={"client_pub": b64(bytes(32))})
    assert r.status_code == 400 and r.json()["error"] == "bad_client_key"
    assert h.post("/v1/link/start", json={"client_pub": "!!"}).status_code == 400
    assert h.post("/v1/link/start", content=b"[]").status_code == 400


def test_banned_account_cannot_finish(h):
    h.link(client(1), name="Skeleton")
    acc = h.community.find_account(h.community.db.one("SELECT account_id FROM accounts")[0].hex())
    h.community.ban(acc["id"], "test")
    r = h.link(client(1), name=None)
    assert r.status_code == 403 and r.json()["error"] == "banned"


def test_discord_not_configured(h):
    h.cfg.discord_client_id = ""
    r = h.post("/v1/link/start", json={"client_pub": b64(T.seed_public(client(1)))})
    assert r.status_code == 503 and r.json()["error"] == "discord_not_configured"
    assert "COMMUNITY_DISCORD_CLIENT_ID" in r.json()["detail"]
    assert h.get("/health").json()["discord_configured"] is False


def test_missing_ticket_key_and_keyset(h):
    os.remove(h.cfg.ticket_key_path)
    h.community._key = None
    r = h.link(client(1), name="Skeleton")
    assert r.status_code == 503 and r.json()["error"] == "ticket_key_missing"
    assert "gen-ticket-key" in r.json()["detail"]
    assert h.community.db.one("SELECT client_pub FROM accounts")[0] is None  # nothing bound without a ticket
    for f in os.listdir(h.cfg.keyset_dir):
        os.remove(os.path.join(h.cfg.keyset_dir, f))
    r = h.get("/v1/keyset")
    assert r.status_code == 503 and r.json()["error"] == "keyset_missing"


def test_keyset_endpoint_serves_root_signed_bytes(h):
    r = h.get("/v1/keyset")
    assert r.headers["content-type"] == "application/octet-stream"
    rr, ks = T.parse_keyset(r.content, ROOTS, 0, NOW)
    assert rr == T.OK and r.headers["X-PC-Keyset-Serial"] == str(ks.serial)


def test_health(h):
    j = h.get("/health").json()
    assert j["ok"] and j["discord_configured"] and j["issuing"] == {"key_gen": 1} and j["keyset"] == {"serial": 1}


def test_migrations_idempotent(h):
    from db import SCHEMA_VERSION, Database
    db2 = Database(h.cfg.db_path)
    assert db2.one("PRAGMA user_version")[0] == SCHEMA_VERSION
