"""LOCAL DEV MODE: a stand-in for the Discord OAuth app, so the whole link flow runs end to end on one machine.

Enabled by COMMUNITY_FAKE_DISCORD=1. Refused at startup unless BOTH the bind host (COMMUNITY_BIND_HOST, the --host
given to uvicorn) and the signed public host (COMMUNITY_PUBLIC_HOST) are loopback, and every fake route also refuses a
non-loopback peer or a proxied request (X-Forwarded-For: Caddy adds it in production). The production service is
reached through 2dfm.org, so its public host alone keeps this off there.

What it replaces: only the two Discord calls. /v1/link/start's sign_in_url points at this service's own
/fake/authorize page, whose button lands on the normal /discord/callback with a code "fake:<discord id>". The code
exchange returns that id. Accounts, tickets, the KEYSET, the deny list and refresh all run for real.

The fake Discord user: COMMUNITY_FAKE_DISCORD_USER = "ID[:NAME]" (default "fake-user:Tester"). The page lets the tester
edit the id before continuing, so two accounts are two ids.
"""
from __future__ import annotations

import html
import ipaddress
import urllib.parse
from typing import Optional

from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse, JSONResponse

from config import Config

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


def fake_user(cfg: Config) -> tuple[str, str]:
    uid, _, name = (cfg.fake_discord_user or "").partition(":")
    return (uid.strip() or "fake-user")[:MAX_ID], name.strip() or "Tester"


def request_is_local(request: Request) -> bool:
    peer = request.client.host if request.client else ""
    return host_is_loopback(peer) and "x-forwarded-for" not in request.headers


class FakeDiscordClient:
    """Same surface as app.DiscordClient. authorize_url needs the request's base URL (scheme, host, port, prefix)."""

    def __init__(self, cfg: Config):
        self.cfg = cfg

    def authorize_url(self, state: str, base: str = "") -> str:
        return base.rstrip("/") + "/fake/authorize?" + urllib.parse.urlencode({"state": state})

    async def user_id(self, code: str) -> Optional[str]:
        if not code.startswith(CODE_PREFIX):
            return None
        uid = code[len(CODE_PREFIX):].strip()
        return uid if 0 < len(uid) <= MAX_ID and uid.isprintable() else None


def _page(status: int, title: str, body_html: str) -> HTMLResponse:
    return HTMLResponse(status_code=status, content=(
        "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
        f"<title>{html.escape(title)}</title><body style='font-family:sans-serif;max-width:36em;margin:3em auto;"
        f"padding:0 1em'><h2>{html.escape(title)}</h2>{body_html}</body>"))


def install(app: FastAPI, cfg: Config) -> None:
    """Swap in the fake client and add GET /fake/authorize. Call only after check_allowed."""
    app.state.discord = FakeDiscordClient(cfg)

    @app.middleware("http")
    async def fake_local_only(request: Request, call_next):
        if not request_is_local(request):
            return JSONResponse(status_code=403, content={"error": "fake_discord_local_only",
                                                          "detail": "local dev mode answers loopback clients only"})
        return await call_next(request)

    @app.get("/fake/authorize")
    async def fake_authorize(state: str = ""):
        uid, name = fake_user(cfg)
        s = html.escape(state, quote=True)
        return _page(200, "Fake Discord sign-in (local dev)", (
            "<p>LOCAL DEV MODE. No Discord is involved. Continue as fake Discord user "
            f"<b>{html.escape(name)}</b>:</p>"
            "<form method=get action='../discord/callback'>"
            f"<input type=hidden name=state value='{s}'>"
            f"<label>Discord id <input name=code value='{html.escape(CODE_PREFIX + uid, quote=True)}' "
            "size=30></label> "
            "<button type=submit autofocus>Continue</button></form>"
            f"<p><a href='../discord/callback?state={urllib.parse.quote(state)}&amp;error=access_denied'>"
            "Cancel</a></p>"))
