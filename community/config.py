"""Configuration: environment variables, with an optional KEY=VALUE file (COMMUNITY_CONFIG, default
community/data/community.env) for values the environment does not set. Nothing secret is ever logged or returned.
"""
from __future__ import annotations

import os
from dataclasses import dataclass

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")


def load_env_file(path: str) -> None:
    """KEY=VALUE lines, # comments, optional quotes. The environment wins over the file."""
    if not path or not os.path.isfile(path):
        return
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = (s.strip() for s in line.split("=", 1))
            if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
                v = v[1:-1]
            os.environ.setdefault(k, v)


@dataclass
class Config:
    discord_client_id: str
    db_path: str
    ticket_key_path: str
    keyset_dir: str
    public_host: str  # the host clients sign into a section 6 call
    root_path: str  # the public prefix in front of the app (Caddy route), part of the signed path
    root_pubs: tuple[bytes, bytes]
    deny_valid_seconds: int
    deny_refresh_seconds: int
    fake_discord: bool = False  # LOCAL DEV MODE (fake_discord.py): loopback only, refused anywhere else
    bind_host: str = ""  # the uvicorn --host; fake mode checks it is loopback

    @classmethod
    def from_env(cls) -> "Config":
        load_env_file(os.environ.get("COMMUNITY_CONFIG", os.path.join(DATA, "community.env")))
        from tickets import ROOT_A_PUB, ROOT_B_PUB
        e = os.environ.get
        roots = (ROOT_A_PUB, ROOT_B_PUB)
        override = e("COMMUNITY_ROOT_PUBKEYS", "")
        if override:  # staging and tests only: production uses the compiled-in roots
            a, b = (bytes.fromhex(x.strip()) for x in override.split(","))
            roots = (a, b)
        return cls(
            discord_client_id=e("COMMUNITY_DISCORD_CLIENT_ID", ""),
            db_path=e("COMMUNITY_DB", os.path.join(DATA, "community.sqlite3")),
            ticket_key_path=e("COMMUNITY_TICKET_KEY", os.path.join(DATA, "ticket_key.txt")),
            keyset_dir=e("COMMUNITY_KEYSET_DIR", os.path.join(DATA, "keysets")),
            public_host=e("COMMUNITY_PUBLIC_HOST", "2dfm.org"),
            root_path=e("COMMUNITY_ROOT_PATH", "/community").rstrip("/"),
            root_pubs=roots,
            deny_valid_seconds=int(e("COMMUNITY_DENY_VALID_SECONDS", "86400")),
            deny_refresh_seconds=int(e("COMMUNITY_DENY_REFRESH_SECONDS", "900")),
            fake_discord=e("COMMUNITY_FAKE_DISCORD", "") == "1",
            bind_host=e("COMMUNITY_BIND_HOST", ""),
        )

    def discord_missing(self) -> list[str]:
        out = []
        if not self.discord_client_id:
            out.append("COMMUNITY_DISCORD_CLIENT_ID")
        return out
