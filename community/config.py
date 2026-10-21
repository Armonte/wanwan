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
    public_host: str  # the host(s) clients sign into a section 6 call: "2dfm.org" or "2dfm.org,povertycaster.org"
    root_path: str  # the public prefix in front of the app (Caddy route), part of the signed path
    root_pubs: tuple[bytes, bytes]
    deny_valid_seconds: int
    deny_refresh_seconds: int
    stats_id_secret: str = ""  # FM2K_STATS_ID_SECRET, shared with the stats site (public ids)
    stats_url: str = ""  # the stats site's base URL, served by /v1/config (no domain in launcher code)
    replay_public_base: str = ""  # public base of shared replays (R2), served by /v1/config
    web_base: str = ""  # the public web root, served by /v1/config
    export_dir: str = ""  # files the stats site reads (hidden.json); default <data>/export
    retired_ticket_keys: str = ""  # comma list of key files of earlier generations (after a rotation)
    fake_discord: bool = False  # LOCAL DEV MODE (fake_discord.py): loopback only, refused anywhere else
    bind_host: str = ""  # the uvicorn --host; fake mode checks it is loopback
    # shared replays (replays.py): Cloudflare R2 through the S3 API. Any of the four missing -> 503 replays_disabled
    r2_endpoint: str = ""
    r2_bucket: str = ""
    r2_access_key_id: str = ""
    r2_secret_access_key: str = ""
    replay_max_bytes: int = 1 << 20  # one upload, checked from Content-Length before reading
    replay_account_daily: int = 200  # stored uploads per account per UTC day (429 cap_daily)
    replay_daily_uploads: int = 20000  # global circuit breaker per UTC day (503 replays_paused)
    replay_daily_bytes: int = 2 << 30

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
            retired_ticket_keys=e("COMMUNITY_RETIRED_TICKET_KEYS", ""),
            stats_id_secret=e("FM2K_STATS_ID_SECRET", ""),
            stats_url=e("COMMUNITY_STATS_URL", ""),
            replay_public_base=e("COMMUNITY_REPLAY_PUBLIC_BASE", ""),
            web_base=e("COMMUNITY_WEB_BASE", ""),
            export_dir=e("COMMUNITY_EXPORT_DIR", os.path.join(DATA, "export")),
            fake_discord=e("COMMUNITY_FAKE_DISCORD", "") == "1",
            bind_host=e("COMMUNITY_BIND_HOST", ""),
            r2_endpoint=e("COMMUNITY_R2_ENDPOINT", ""),
            r2_bucket=e("COMMUNITY_R2_BUCKET", ""),
            r2_access_key_id=e("COMMUNITY_R2_ACCESS_KEY_ID", ""),
            r2_secret_access_key=e("COMMUNITY_R2_SECRET_ACCESS_KEY", ""),
            replay_max_bytes=min(int(e("COMMUNITY_REPLAY_MAX_BYTES", str(1 << 20))), 1 << 20),
            replay_account_daily=int(e("COMMUNITY_REPLAY_ACCOUNT_DAILY", "200")),
            replay_daily_uploads=int(e("COMMUNITY_REPLAY_DAILY_UPLOADS", "20000")),
            replay_daily_bytes=int(e("COMMUNITY_REPLAY_DAILY_BYTES", str(2 << 30))),
        )

    @property
    def public_hosts(self) -> list[str]:
        """Every host a signed call may name. More than one during a domain move (2dfm.org -> povertycaster.org): a
        launcher signs the host of its own community.url, so old and new launchers both verify until the old host
        is dropped."""
        return [h.strip() for h in self.public_host.split(",") if h.strip()]

    def replays_missing(self) -> list[str]:
        """What shared replays still need (names only, never values)."""
        return [n for n, v in (("COMMUNITY_R2_ENDPOINT", self.r2_endpoint), ("COMMUNITY_R2_BUCKET", self.r2_bucket),
                               ("COMMUNITY_R2_ACCESS_KEY_ID", self.r2_access_key_id),
                               ("COMMUNITY_R2_SECRET_ACCESS_KEY", self.r2_secret_access_key),
                               ("COMMUNITY_REPLAY_PUBLIC_BASE", self.replay_public_base)) if not v]

    def discord_missing(self) -> list[str]:
        out = []
        if not self.discord_client_id:
            out.append("COMMUNITY_DISCORD_CLIENT_ID")
        return out
