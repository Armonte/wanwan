"""Shared fixtures: test roots (seeds 0x01 / 0x07, the C++ vectors' roots), a temp data dir with an online ticket key
and a KEYSET signed by test root a, a fake clock and a fake Discord. No network, no real roots."""
from __future__ import annotations

import base64
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import tickets as T  # noqa: E402
from config import Config  # noqa: E402

NOW = 1800000000
ROOT_A, ROOT_B = bytes([1]) * 32, bytes([7]) * 32
ROOTS = (T.seed_public(ROOT_A), T.seed_public(ROOT_B))
DISCORD_ID = "987654321098765432"
DISCORD_NAME = "DiscordSecretName"


class Clock:
    def __init__(self, t=NOW):
        self.t = t

    def __call__(self):
        return self.t


class FakeDiscord:
    """code -> Discord user id; anything else fails like a refused exchange."""

    def __init__(self):
        self.users = {"good-code": DISCORD_ID, "other-code": "111111111111111111"}

    def authorize_url(self, state):
        return f"https://discord.example/authorize?state={state}"

    async def user_id(self, code):
        return self.users.get(code)


def make_cfg(tmp, **over) -> Config:
    vals = dict(discord_client_id="cid", discord_client_secret="secret",
                discord_redirect_uri="https://2dfm.org/community/discord/callback",
                db_path=os.path.join(tmp, "c.sqlite3"), ticket_key_path=os.path.join(tmp, "ticket_key.txt"),
                keyset_dir=os.path.join(tmp, "keysets"), public_host="2dfm.org", root_path="/community",
                root_pubs=ROOTS, deny_valid_seconds=86400, deny_refresh_seconds=900)
    vals.update(over)
    return Config(**vals)


def make_keyset(ticket_pub: bytes, serial=1, gen=1, issued=NOW - 86400, root=0, **kw) -> bytes:
    k = T.Keyset(serial, issued, issued + 45 * 86400, kw.pop("min_key_gen", 0), root,
                 [T.KeysetEntry(gen, ticket_pub, issued - 86400, issued + 90 * 86400)], **kw)
    return T.sign_keyset(k, (ROOT_A, ROOT_B)[root])


class Harness:
    def __init__(self, tmp):
        from fastapi.testclient import TestClient

        import app as app_mod
        from service import Community, write_ticket_key
        self.tmp = str(tmp)
        self.cfg = make_cfg(self.tmp)
        self.clock = Clock()
        self.key_seed = bytes([0x33]) * 32
        write_ticket_key(self.cfg.ticket_key_path, 1, self.key_seed)
        os.makedirs(self.cfg.keyset_dir)
        self.install_keyset(make_keyset(T.seed_public(self.key_seed)), "keyset_000001.pckeys")
        self.community = Community(self.cfg, clock=self.clock)
        self.app = app_mod.create_app(self.cfg, self.community)
        self.app.state.discord = FakeDiscord()
        self.client = TestClient(self.app)
        self.responses = []  # every response, for the privacy scan

    def install_keyset(self, b: bytes, name: str):
        with open(os.path.join(self.cfg.keyset_dir, name), "wb") as f:
            f.write(b)

    def keyset(self):
        r, ks = T.parse_keyset(self.get("/v1/keyset").content, ROOTS, 0, self.clock.t)
        assert r == T.OK
        return ks

    def _rec(self, r):
        self.responses.append(r)
        return r

    def get(self, path, **kw):
        return self._rec(self.client.get(path, **kw))

    def post(self, path, **kw):
        return self._rec(self.client.post(path, **kw))

    def link(self, client_seed: bytes, name="Skeleton", code="good-code"):
        """start -> Discord callback -> poll -> finish. Returns the finish response."""
        pub = T.seed_public(client_seed)
        st = self.post("/v1/link/start", json={"client_pub": b64(pub)})
        assert st.status_code == 200, st.text
        j = st.json()
        state = j["sign_in_url"].split("state=")[1]
        cb = self.get("/discord/callback", params={"state": state, "code": code})
        assert cb.status_code == 200, cb.text
        poll = self.post("/v1/link/poll", json={"link_id": j["link_id"]})
        assert poll.json()["status"] in ("authorized", "banned")
        challenge = base64.b64decode(j["challenge"])
        sig = T.sign(client_seed, T.TAG_LINK, T.link_message(challenge, pub))
        body = {"link_id": j["link_id"], "signature": b64(sig)}
        if name is not None:
            body["name"] = name
        return self.post("/v1/link/finish", json=body)

    def refresh(self, client_seed: bytes, ticket: bytes, nonce=b"\x09" * 16, ts=None, path="/v1/ticket/refresh",
                signed_path=None, host="2dfm.org", body=b""):
        ts = self.clock.t if ts is None else ts
        msg = T.call_message(host.encode(), b"POST", (signed_path or "/community" + path).encode(), T.sha256(body),
                             ts, nonce, T.ticket_id(ticket))
        sig = T.sign(client_seed, b"", msg)
        return self.post(path, content=body, headers={
            "X-PC-Ticket": b64(ticket), "X-PC-Time": str(ts), "X-PC-Nonce": b64(nonce), "X-PC-Sig": b64(sig)})


def b64(b: bytes) -> str:
    return base64.b64encode(b).decode()


@pytest.fixture
def h(tmp_path):
    return Harness(tmp_path)
