"""PovertyCaster community service: Discord account linking and ticket issuance (COMMUNITY_TICKET_SPEC.md v9.1
sections 2, 3, 6, 8, 9; COMMUNITY_SERVICE.md section 7).

    uvicorn app:app --host 127.0.0.1 --port 8090      (from community/; Caddy serves it at 2dfm.org/community/)

Endpoints (paths as the app sees them; the public URL adds /community):
    GET  /health                      liveness + what is configured (no secrets)
    POST /v1/link/start               {client_pub} -> {link_id, challenge, sign_in_url}
    GET  /discord/callback            Discord's OAuth redirect (the browser lands here)
    POST /v1/link/poll                {link_id} -> {status, needs_name, display?}
    POST /v1/link/finish              {link_id, signature, name?} -> {ticket, display, ...}
    POST /v1/ticket/refresh           signed call (section 6 headers) -> {ticket, ...}
    GET  /v1/keyset                   the current root-signed KEYSET (PCKEYS1 bytes)
    GET  /v1/deny                     the deny list, signed by the ticket key (PCDENY1 bytes)

No response carries a Discord id, a Discord name or an IP address. Players are Name#1234.
"""
from __future__ import annotations

import base64
import binascii
import html
import secrets
import time
import urllib.parse
from collections import defaultdict, deque
from typing import Optional

import httpx
from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse, JSONResponse, Response

import tickets as T
from config import Config
from db import SCHEMA_VERSION
from service import Community, ServiceError

LINK_TTL = 600  # a link must be finished within 10 minutes of its start
REFRESH_AFTER = T.TICKET_LIFETIME // 2  # the launcher refreshes silently at half-life


class DiscordClient:
    """The two Discord calls of the OAuth code flow. Tests replace app.state.discord with a fake."""

    def __init__(self, cfg: Config):
        self.cfg = cfg

    def authorize_url(self, state: str) -> str:
        return "https://discord.com/oauth2/authorize?" + urllib.parse.urlencode({
            "client_id": self.cfg.discord_client_id, "response_type": "code",
            "redirect_uri": self.cfg.discord_redirect_uri, "scope": "identify", "state": state, "prompt": "none"})

    async def user_id(self, code: str) -> Optional[str]:
        async with httpx.AsyncClient(timeout=10.0) as cli:
            r = await cli.post("https://discord.com/api/oauth2/token", data={
                "client_id": self.cfg.discord_client_id, "client_secret": self.cfg.discord_client_secret,
                "grant_type": "authorization_code", "code": code, "redirect_uri": self.cfg.discord_redirect_uri})
            if r.status_code != 200:
                return None
            tok = r.json().get("access_token")
            if not tok:
                return None
            me = await cli.get("https://discord.com/api/users/@me", headers={"Authorization": f"Bearer {tok}"})
            if me.status_code != 200:
                return None
            uid = me.json().get("id")
            return str(uid) if uid else None


def b64d(s, n: Optional[int] = None) -> bytes:
    try:
        b = base64.b64decode(s or "", validate=True)
    except (binascii.Error, ValueError, TypeError):
        raise ServiceError(400, "bad_base64", "not base64")
    if n is not None and len(b) != n:
        raise ServiceError(400, "bad_length", f"expected {n} bytes")
    return b


def b64e(b: bytes) -> str:
    return base64.b64encode(b).decode()


class RateLimit:
    """In-memory per-client sliding window. The key is hashed in memory only; nothing about a client is stored."""

    def __init__(self, limit: int, window: int):
        self.limit, self.window = limit, window
        self.hits: dict[int, deque] = defaultdict(deque)

    def allow(self, key: str, now: float) -> bool:
        q = self.hits[hash(key)]
        while q and q[0] <= now - self.window:
            q.popleft()
        if len(q) >= self.limit:
            return False
        q.append(now)
        return True


def client_key(request: Request) -> str:
    peer = request.client.host if request.client else ""
    fwd = request.headers.get("x-forwarded-for", "")
    if fwd and peer in ("127.0.0.1", "::1"):  # behind Caddy: the hop Caddy appended
        return fwd.split(",")[-1].strip()
    return peer


def page(title: str, body: str, status: int = 200) -> HTMLResponse:
    return HTMLResponse(status_code=status, content=(
        "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
        f"<title>{html.escape(title)}</title><body style='font-family:sans-serif;max-width:36em;margin:3em auto;"
        f"padding:0 1em'><h2>{html.escape(title)}</h2><p>{html.escape(body)}</p></body>"))


def create_app(cfg: Optional[Config] = None, community: Optional[Community] = None) -> FastAPI:
    cfg = cfg or Config.from_env()
    app = FastAPI(title="PovertyCaster community", docs_url=None, redoc_url=None, openapi_url=None)
    app.state.cfg = cfg
    app.state.community = community or Community(cfg)
    app.state.discord = DiscordClient(cfg)
    app.state.link_limit = RateLimit(20, 600)
    app.state.refresh_limit = RateLimit(30, 600)

    def svc() -> Community:
        return app.state.community

    @app.middleware("http")
    async def server_time(request: Request, call_next):
        resp = await call_next(request)
        resp.headers["X-PC-Server-Time"] = str(svc().now())  # every response, errors included (section 6)
        resp.headers["Cache-Control"] = "no-store"
        return resp

    @app.exception_handler(ServiceError)
    async def service_error(request: Request, e: ServiceError):
        return JSONResponse(status_code=e.status, content={"error": e.code, "detail": e.detail})

    async def json_body(request: Request) -> dict:
        try:
            body = await request.json()
        except Exception:
            raise ServiceError(400, "bad_json", "the body must be a JSON object")
        if not isinstance(body, dict):
            raise ServiceError(400, "bad_json", "the body must be a JSON object")
        return body

    def ticket_response(b: bytes) -> dict:
        _, t = T.decode_ticket(b)
        return {"ticket": b64e(b), "account_id": t.account.hex(), "name": t.name, "tag": f"{t.tag:04d}",
                "display": t.display, "refresh_after": REFRESH_AFTER}

    @app.get("/health")
    async def health():
        c = svc()
        out = {"ok": True, "schema": SCHEMA_VERSION, "discord_configured": not cfg.discord_missing()}
        for name, fn in (("ticket_key", lambda: {"key_gen": c.ticket_key().key_gen}),
                         ("keyset", lambda: {"serial": c.keyset()[1].serial}),
                         ("issuing", lambda: {"key_gen": c.signing_key().key_gen})):
            try:
                out[name] = fn()
            except ServiceError as e:
                out[name] = {"error": e.code}
        return out

    # ---- linking (section 9) ----
    @app.post("/v1/link/start")
    async def link_start(request: Request):
        missing = cfg.discord_missing()
        if missing:
            raise ServiceError(503, "discord_not_configured", "the service is missing " + ", ".join(missing))
        if not app.state.link_limit.allow(client_key(request), time.monotonic()):
            raise ServiceError(429, "rate_limited", "too many link attempts, try again later")
        body = await json_body(request)
        client_pub = b64d(body.get("client_pub"), 32)
        if not T.pubkey_strong(client_pub):
            raise ServiceError(400, "bad_client_key", "client_pub is not a usable Ed25519 key")
        c = svc()
        now = c.now()
        link_id, state, challenge = secrets.token_urlsafe(24), secrets.token_urlsafe(24), secrets.token_bytes(32)
        c.db.x("DELETE FROM links WHERE created < ?", (now - LINK_TTL,))
        c.db.x("INSERT INTO links(link_id, state, challenge, client_pub, created, status) VALUES(?,?,?,?,?, 'pending')",
               (link_id, state, challenge, client_pub, now))
        return {"link_id": link_id, "challenge": b64e(challenge), "sign_in_url": app.state.discord.authorize_url(state),
                "expires_in": LINK_TTL}

    def live_link(link_id: str):
        c = svc()
        row = c.db.one("SELECT * FROM links WHERE link_id = ?", (str(link_id or ""),))
        if row is None or row["created"] < c.now() - LINK_TTL:
            raise ServiceError(410, "link_expired", "start linking again")
        return row

    @app.get("/discord/callback")
    async def discord_callback(state: str = "", code: str = "", error: str = ""):
        c = svc()
        row = c.db.one("SELECT * FROM links WHERE state = ?", (state,)) if state else None
        if row is None or row["created"] < c.now() - LINK_TTL or row["status"] != "pending":
            return page("Link expired", "This sign-in link has expired or was already used. "
                        "Start again from PovertyCaster.", 410)
        if error or not code:
            c.db.x("UPDATE links SET status = 'failed' WHERE link_id = ?", (row["link_id"],))
            return page("Sign-in cancelled", "Discord sign-in was cancelled. You can close this tab and try again "
                        "from PovertyCaster.", 400)
        try:
            uid = await app.state.discord.user_id(code)
        except httpx.HTTPError:
            uid = None
        if not uid:
            return page("Sign-in failed", "Discord did not confirm the sign-in. Close this tab and try again.", 502)
        acc = c.account_for_discord(uid)
        c.db.x("UPDATE links SET account = ?, status = 'authorized' WHERE link_id = ? AND status = 'pending'",
               (acc, row["link_id"]))
        return page("Signed in", "Discord is linked. Return to PovertyCaster to finish; you can close this tab.")

    @app.post("/v1/link/poll")
    async def link_poll(request: Request):
        body = await json_body(request)
        row = live_link(body.get("link_id"))
        out = {"status": row["status"]}
        if row["status"] == "authorized":
            acc = svc().account(row["account"])
            out["needs_name"] = acc["name"] is None
            if acc["name"]:
                out["display"] = f"{acc['name']}#{acc['tag']:04d}"
            if acc["banned"]:
                out["status"] = "banned"
        return out

    @app.post("/v1/link/finish")
    async def link_finish(request: Request):
        body = await json_body(request)
        row = live_link(body.get("link_id"))
        if row["status"] != "authorized":
            raise ServiceError(409, "not_authorized", f"link is {row['status']}: sign in with Discord first")
        sig = b64d(body.get("signature"), 64)
        if not T.verify(row["client_pub"], T.TAG_LINK, T.link_message(row["challenge"], row["client_pub"]), sig):
            raise ServiceError(401, "bad_link_proof", "the challenge signature does not verify under client_pub")
        c = svc()
        acc = c.account(row["account"])
        if acc["banned"]:
            raise ServiceError(403, "banned", "this account is banned")
        name = body.get("name")
        if name is not None:
            c.set_name(acc["id"], name)
        elif acc["name"] is None:
            raise ServiceError(409, "name_required", "choose a name")
        c.signing_key()  # refuse before binding when no ticket can be minted
        c.bind_client(acc["id"], row["client_pub"])
        b = c.issue_ticket(acc["id"])
        c.db.x("UPDATE links SET status = 'consumed' WHERE link_id = ?", (row["link_id"],))
        return ticket_response(b)

    # ---- renewal (section 6) ----
    @app.post("/v1/ticket/refresh")
    async def ticket_refresh(request: Request):
        c = svc()
        if not app.state.refresh_limit.allow(client_key(request), time.monotonic()):
            raise ServiceError(429, "rate_limited", "too many refreshes")
        h = request.headers
        ticket = b64d(h.get("x-pc-ticket"), T.TICKET_SIZE)
        nonce = b64d(h.get("x-pc-nonce"), 16)
        sig = b64d(h.get("x-pc-sig"), 64)
        ts_s = h.get("x-pc-time", "")
        if not ts_s.isdigit() or len(ts_s) > 20:
            raise ServiceError(401, "bad_time", "X-PC-Time must be decimal unix seconds")
        ts = int(ts_s)
        if abs(ts - c.now()) > 300:
            raise ServiceError(401, "clock_skew", "X-PC-Time is more than 5 minutes off; use X-PC-Server-Time")
        row, t = c.account_for_refresh(ticket)
        body = await request.body()
        path = cfg.root_path + request.url.path
        msg = T.call_message(cfg.public_host.encode(), request.method.encode(), path.encode(), T.sha256(body), ts,
                             nonce, T.ticket_id(ticket))
        if not T.verify_call(t.client_pub, msg, sig):
            raise ServiceError(401, "bad_call_signature", "the call signature does not verify")
        if c.nonce_seen(nonce):
            raise ServiceError(401, "nonce_reused", "this nonce was already used")
        c.nonce_store(nonce)
        return ticket_response(c.issue_ticket(row))

    # ---- published records ----
    @app.get("/v1/keyset")
    async def keyset():
        b, ks = svc().keyset()
        return Response(content=b, media_type="application/octet-stream",
                        headers={"X-PC-Keyset-Serial": str(ks.serial)})

    @app.get("/v1/deny")
    async def deny():
        return Response(content=svc().deny_list(), media_type="application/octet-stream")

    return app


def __getattr__(name):  # `uvicorn app:app` builds the app on first use, so importing for tests needs no config
    if name == "app":
        globals()["app"] = create_app()
        return globals()["app"]
    raise AttributeError(name)
