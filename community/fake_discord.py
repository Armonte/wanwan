"""LOCAL DEV MODE: a stand-in for Discord's "who is this token" call, so the whole link flow runs end to end on one
machine without a Discord login.

Enabled by COMMUNITY_FAKE_DISCORD=1. Refused at startup unless BOTH the bind host (COMMUNITY_BIND_HOST, the --host
given to uvicorn) and the signed public host (COMMUNITY_PUBLIC_HOST) are loopback, and every request is refused from a
non-loopback peer or when proxied (X-Forwarded-For: Caddy adds it in production). The production service is reached
through 2dfm.org, so its public host alone keeps this off there.

What it replaces: only DiscordClient.whoami. /v1/link/discord accepts an access_token "fake:<discord id>[:<name>]"
(the launcher sends PC_COMMUNITY_FAKE_DISCORD_TOKEN instead of signing in through the SDK when its community.url is
loopback). Accounts, names, tickets, the KEYSET, the deny list and refresh all run for real. Two ids are two accounts.
"""
from __future__ import annotations

import ipaddress

from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse

from config import Config
from service import ServiceError

CODE_PREFIX = "fake:"
MAX_ID = 64


def host_is_loopback(host: str) -> bool:
    """127.0.0.0/8, ::1 or localhost, with an optional :port or [brackets]."""
    h = (host or "").strip().lower()
    if h.startswith("["):
        h = h[1:h.find("]")] if "]" in h else ""
    elif h.count(":") == 1:
        h = h.split(":", 1)[0]
    if h == "localhost":
        return True
    try:
        return ipaddress.ip_address(h).is_loopback
    except ValueError:
        return False


def check_allowed(cfg: Config) -> None:
    """Raises RuntimeError (the app refuses to start) unless fake mode is confined to loopback."""
    if not cfg.fake_discord:
        return
    if not host_is_loopback(cfg.bind_host):
        raise RuntimeError("COMMUNITY_FAKE_DISCORD=1 needs COMMUNITY_BIND_HOST set to a loopback address "
                           f"(the uvicorn --host); got {cfg.bind_host!r}")
    if not host_is_loopback(cfg.public_host):
        raise RuntimeError("COMMUNITY_FAKE_DISCORD=1 needs a loopback COMMUNITY_PUBLIC_HOST; "
                           f"got {cfg.public_host!r}")


def request_is_local(request: Request) -> bool:
    peer = request.client.host if request.client else ""
    return host_is_loopback(peer) and "x-forwarded-for" not in request.headers


class FakeDiscordClient:
    """Same surface as app.DiscordClient: "fake:<id>[:<name>]" is that Discord user; anything else is refused."""

    def __init__(self, cfg: Config):
        self.cfg = cfg

    async def whoami(self, access_token: str) -> dict:
        if not access_token.startswith(CODE_PREFIX):
            raise ServiceError(401, "discord_token_invalid", "local dev mode takes only fake:<id>[:<name>] tokens")
        uid, _, name = access_token[len(CODE_PREFIX):].partition(":")
        uid = uid.strip()
        if not 0 < len(uid) <= MAX_ID:
            raise ServiceError(401, "discord_token_invalid", "fake:<id> needs an id of 1 to 64 characters")
        name = name.strip() or "Tester"
        return {"id": uid, "username": name.lower(), "global_name": name}


def install(app: FastAPI, cfg: Config) -> None:
    """Swap in the fake client and confine the app to loopback clients. Call only after check_allowed."""
    app.state.discord = FakeDiscordClient(cfg)

    @app.middleware("http")
    async def fake_local_only(request: Request, call_next):
        if not request_is_local(request):
            return JSONResponse(status_code=403, content={"error": "fake_discord_local_only",
                                                          "detail": "local dev mode answers loopback clients only"})
        return await call_next(request)
