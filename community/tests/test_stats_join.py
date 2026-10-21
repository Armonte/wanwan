"""The stats join: public_id equals the stats site's own derivation (so legacy hub history is the linked player's),
the profile opt-out (signed) and its export, /v1/config, and backfill of accounts made before the secret existed."""
import base64
import importlib.util
import json
import os

import tickets as T
from conftest import DISCORD_ID, b64

C = bytes([0x91]) * 32
SECRET = "0f" * 32


# stats/privacy.py public_id("987654321098765432") under SECRET, pinned so the check runs where the stats code is not
# beside this repo (the server keeps it in ~/stats); stats_privacy() cross-checks live wherever it is found.
PINNED_PID = "6eb9a8a90fedc425"


class _Pinned:
    @staticmethod
    def public_id(discord_id):
        assert discord_id == DISCORD_ID
        return PINNED_PID


def stats_privacy():
    here = os.path.dirname(__file__)
    for p in (os.environ.get("FM2K_STATS_DIR", ""), os.path.join(here, "..", "..", "stats"),
              os.path.expanduser("~/stats")):
        if p and os.path.isfile(os.path.join(p, "privacy.py")):
            break
    else:
        return _Pinned
    spec = importlib.util.spec_from_file_location("stats_privacy", os.path.join(p, "privacy.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    m.set_secret(SECRET.encode())
    assert m.public_id(DISCORD_ID) == PINNED_PID
    return m


def test_public_id_matches_the_stats_site_and_reaches_only_the_player(h):
    h.cfg.stats_id_secret = SECRET
    fin = h.link(C, name="Skeleton").json()
    assert fin["public_id"] == stats_privacy().public_id(DISCORD_ID) and fin["stats_hidden"] is False
    t = base64.b64decode(fin["ticket"])
    assert h.refresh(C, t, nonce=b"\x31" * 16).json()["public_id"] == fin["public_id"]
    for r in (h.get("/health"), h.get("/v1/config"), h.get("/v1/keyset"), h.get("/v1/deny")):
        assert fin["public_id"].encode() not in r.content


def test_backfill_when_the_secret_arrives_later(h):
    h.link(C, name="Skeleton")
    assert h.community.db.one("SELECT public_id FROM accounts")[0] is None
    h.cfg.stats_id_secret = SECRET
    assert h.community.ensure_public_ids() == 1
    assert h.community.db.one("SELECT public_id FROM accounts")[0] == stats_privacy().public_id(DISCORD_ID)


def signed_post(h, path, ticket, body, nonce):
    msg = T.call_message(b"2dfm.org", b"POST", ("/community" + path).encode(), T.sha256(body), h.clock.t, nonce,
                         T.ticket_id(ticket))
    return h.post(path, content=body, headers={"X-PC-Ticket": b64(ticket), "X-PC-Time": str(h.clock.t),
                                               "X-PC-Nonce": b64(nonce), "X-PC-Sig": b64(T.sign(C, b"", msg))})


def test_visibility_is_signed_and_exported(h, tmp_path):
    h.cfg.stats_id_secret = SECRET
    h.cfg.export_dir = str(tmp_path / "export")
    fin = h.link(C, name="Skeleton").json()
    t = base64.b64decode(fin["ticket"])
    r = signed_post(h, "/v1/profile/visibility", t, b'{"hidden": true}', b"\x41" * 16)
    assert r.status_code == 200 and r.json() == {"hidden": True}
    out = json.load(open(tmp_path / "export" / "hidden.json"))
    assert out == {"v": 1, "hidden": [fin["public_id"]]}
    assert signed_post(h, "/v1/profile/visibility", t, b'{"hidden": "yes"}', b"\x42" * 16).status_code == 400
    r = signed_post(h, "/v1/profile/visibility", t, b'{"hidden": false}', b"\x41" * 16)  # nonce reused
    assert r.json()["error"] == "nonce_reused"
    bad = h.post("/v1/profile/visibility", content=b'{"hidden": false}')  # unsigned
    assert bad.status_code in (400, 401)
    assert signed_post(h, "/v1/profile/visibility", t, b'{"hidden": false}', b"\x43" * 16).json() == {"hidden": False}
    assert json.load(open(tmp_path / "export" / "hidden.json"))["hidden"] == []


def test_config_names_the_other_services(h):
    h.cfg.stats_url, h.cfg.replay_public_base = "https://example.test/stats", "https://r2.example.test"
    assert h.get("/v1/config").json() == {"stats_url": "https://example.test/stats",
                                          "replay_base": "https://r2.example.test", "web_base": ""}
