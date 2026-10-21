"""PovertyCaster community service: Discord account linking and ticket issuance (COMMUNITY_TICKET_SPEC.md v9.1
sections 2, 3, 6, 8, 9; COMMUNITY_SERVICE.md section 7).

    uvicorn app:app --host 127.0.0.1 --port 8090      (from community/; Caddy serves it at 2dfm.org/community/)

Endpoints (paths as the app sees them; the public URL adds /community):
    GET  /health                      liveness + what is configured (no secrets)
    POST /v1/link/start               {client_pub} -> {link_id, challenge, expires_in}
    POST /v1/link/discord             {link_id, access_token} -> {status, needs_name, display?, suggested_name,
                                      discord_handle}: the launcher's Discord Social SDK token, used once and dropped
    POST /v1/link/finish              {link_id, signature, name?} -> {ticket, display, discord_handle, ...}
    POST /v1/ticket/refresh           signed call (section 6 headers) -> {ticket, public_id, ...}
    POST /v1/profile/visibility       signed {hidden} -> the stats profile opt-out (export/hidden.json)
    POST /v1/report                   signed receipt list (octet-stream, spec 7c) -> {status, match_id}; the result
                                      lands in export/results.jsonl for the stats site
    POST /v1/replay?match_id=<id>     signed PCRZ1 inputs-only replay (octet-stream, 1 MiB) -> {replay_id, replay_url};
                                      stored in R2, public, kept forever (replays.py)
    GET  /v1/config                   {stats_url, replay_base, web_base}: where the launcher finds the rest
    GET  /v1/keyset                   the current root-signed KEYSET (PCKEYS1 bytes)
    GET  /v1/deny[?gen=N]             the deny list of one key generation, signed by that ticket key (PCDENY1 bytes);
                                      X-PC-Deny-Gens lists every generation served (old ones 72 h past their tickets)

One Discord login: the launcher signs in through the Discord Social SDK (its own token, for presence, invites and
friends) and hands that token here once; the service asks Discord who it is and keeps nothing of the token.

No response carries a Discord id or an IP address. The player's own Discord username (and a name suggestion from it)
goes back only to that player, in link/discord, link/finish and ticket/refresh. Other players see Name#1234.
"""
from __future__ import annotations

import base64
import binascii
import hashlib
import hmac
import json
import secrets
from typing import Optional

import httpx
from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse, Response
from starlette.concurrency import run_in_threadpool

import fake_discord
import games as G
import names
import tickets as T
from config import Config
from replays import Replays
from db import SCHEMA_VERSION
from results import Results
from service import Community, ServiceError

LINK_TTL = 600  # a link must be finished within 10 minutes of its start
REFRESH_AFTER = T.TICKET_LIFETIME // 2  # the launcher refreshes silently at half-life


DISCORD_ME = "https://discord.com/api/v10/oauth2/@me"
MAX_TOKEN = 512


class DiscordClient:
    """Who a launcher's Discord access token belongs to. Tests replace app.state.discord with a fake."""

    def __init__(self, cfg: Config):
        self.cfg = cfg

    async def whoami(self, access_token: str) -> dict:
        """{id, username, global_name} of the token's user. The token must be one issued to OUR application: a token
        another app holds for the same person is refused (discord_wrong_app)."""
        try:
            async with httpx.AsyncClient(timeout=10.0) as cli:
                r = await cli.get(DISCORD_ME, headers={"Authorization": f"Bearer {access_token}"})
        except httpx.HTTPError:
            raise ServiceError(502, "discord_unreachable", "could not reach Discord, try again")
        if r.status_code == 401:
            raise ServiceError(401, "discord_token_invalid", "Discord did not accept the sign-in, sign in again")
        if r.status_code != 200:
            raise ServiceError(502, "discord_unreachable", f"Discord answered {r.status_code}, try again")
        j = r.json()
        if str((j.get("application") or {}).get("id", "")) != self.cfg.discord_client_id:
            raise ServiceError(403, "discord_wrong_app", "this Discord sign-in belongs to another application")
        user = j.get("user") or {}
        if not user.get("id"):
            raise ServiceError(401, "discord_token_invalid", "the Discord sign-in lacks the identify scope")
        return {"id": str(user["id"]), "username": str(user.get("username") or ""),
                "global_name": str(user.get("global_name") or "")}


def suggest_name(who: dict) -> str:
    """The Discord display name if it passes the name rules, else the username, else nothing (the box starts empty)."""
    for raw in (who.get("global_name"), who.get("username")):
        try:
            return names.normalize_name(raw or "")
        except names.BadName:
            continue
    return ""


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
    """Per-client sliding window kept in the database, so a restart does not reset it. The client address is never
    stored: the key is HMAC-SHA256 under a salt that rotates every UTC day (meta rate_salt), and rows are deleted as
    soon as they leave the window."""

    def __init__(self, community: "Community", bucket: str, limit: int, window: int):
        self.c, self.bucket, self.limit, self.window = community, bucket, limit, window

    def _key(self, client: str) -> bytes:
        day = str(int(self.c.now()) // 86400)
        salt = self.c.db.meta_get("rate_salt", "")
        if not salt.startswith(day + ":"):
            salt = day + ":" + secrets.token_hex(16)
            self.c.db.meta_set("rate_salt", salt)
        return hmac.new(salt.encode(), client.encode(), hashlib.sha256).digest()

    def allow(self, client: str, now: float) -> bool:
        key = self._key(client)
        with self.c.db.tx() as db:
            db.x("DELETE FROM rate_hits WHERE ts <= ?", (now - self.window,))
            n = db.one("SELECT COUNT(*) FROM rate_hits WHERE bucket = ? AND key = ?", (self.bucket, key))[0]
            if n >= self.limit:
                return False
            db.x("INSERT INTO rate_hits(bucket, key, ts) VALUES(?, ?, ?)", (self.bucket, key, now))
        return True


def client_key(request: Request) -> str:
    peer = request.client.host if request.client else ""
    fwd = request.headers.get("x-forwarded-for", "")
    if fwd and peer in ("127.0.0.1", "::1"):  # behind Caddy: the hop Caddy appended
        return fwd.split(",")[-1].strip()
    return peer


def create_app(cfg: Optional[Config] = None, community: Optional[Community] = None) -> FastAPI:
    cfg = cfg or Config.from_env()
    fake_discord.check_allowed(cfg)  # local dev mode refuses to start off loopback
    app = FastAPI(title="PovertyCaster community", docs_url=None, redoc_url=None, openapi_url=None)
    app.state.cfg = cfg
    app.state.community = community or Community(cfg)
    app.state.discord = DiscordClient(cfg)
    app.state.link_limit = RateLimit(app.state.community, "link", 20, 600)
    app.state.refresh_limit = RateLimit(app.state.community, "refresh", 30, 600)
    app.state.report_limit = RateLimit(app.state.community, "report", 120, 600)
    app.state.replay_limit = RateLimit(app.state.community, "replay", 60, 600)
    app.state.results = Results(app.state.community)
    app.state.replays = Replays(app.state.community, app.state.results)
    if cfg.fake_discord:
        fake_discord.install(app, cfg)

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
        return JSONResponse(status_code=e.status, content={"error": e.code, "detail": e.detail},
                            headers=getattr(e, "headers", None))

    async def json_body(request: Request) -> dict:
        try:
            body = await request.json()
        except Exception:
            raise ServiceError(400, "bad_json", "the body must be a JSON object")
        if not isinstance(body, dict):
            raise ServiceError(400, "bad_json", "the body must be a JSON object")
        return body

    def ticket_response(b: bytes) -> dict:
        """Only ever sent to the ticket's own player, so it may carry their Discord username."""
        _, t = T.decode_ticket(b)
        acc = svc().db.one("SELECT discord_handle, public_id, stats_hidden FROM accounts WHERE account_id = ?",
                           (t.account,))
        return {"ticket": b64e(b), "account_id": t.account.hex(), "name": t.name, "tag": f"{t.tag:04d}",
                "display": t.display, "refresh_after": REFRESH_AFTER,
                "discord_handle": (acc["discord_handle"] if acc else None) or "",
                "public_id": (acc["public_id"] if acc else None) or "",
                "stats_hidden": bool(acc["stats_hidden"]) if acc else False}

    async def read_capped(request: Request, cap: int) -> bytes:
        """The body, refused above `cap` from Content-Length before a byte is read (and while streaming)."""
        cl = request.headers.get("content-length", "")
        if cl and (not cl.isdigit() or int(cl) > cap):
            raise ServiceError(413, "body_too_large", f"the body is above {cap} bytes")
        out = bytearray()
        async for chunk in request.stream():
            out += chunk
            if len(out) > cap:
                raise ServiceError(413, "body_too_large", f"the body is above {cap} bytes")
        return bytes(out)

    async def signed_call(request: Request, limit: "RateLimit", max_body: int = 65536, pre=None) -> tuple[int, bytes]:
        """Section 6: a ticket this service issued and still bound to the account's client key, X-PC-Time within
        5 minutes, a fresh nonce, and the call signature over one of the public hosts. Returns (account row, body).
        The body is read only after the header and ticket checks (and `pre(account row)`, which may refuse), and never
        above `max_body` (7c). The signed path is the public path plus `?query` when the request has one."""
        c = svc()
        if not limit.allow(client_key(request), float(c.now())):
            raise ServiceError(429, "rate_limited", "too many calls, try again later")
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
        if pre is not None:
            pre(row)
        body = await read_capped(request, max_body)
        path = cfg.root_path + request.url.path
        if request.url.query:
            path += "?" + request.url.query
        digest = T.sha256(body)
        if not any(T.verify_call(t.client_pub, T.call_message(host.encode(), request.method.encode(), path.encode(),
                                                               digest, ts, nonce, T.ticket_id(ticket)), sig)
                   for host in cfg.public_hosts):
            raise ServiceError(401, "bad_call_signature", "the call signature does not verify")
        if c.nonce_seen(nonce):
            raise ServiceError(401, "nonce_reused", "this nonce was already used")
        c.nonce_store(nonce)
        request.state.client_pub = t.client_pub
        return row, body

    @app.get("/health")
    async def health():
        c = svc()
        configured = cfg.fake_discord or not cfg.discord_missing()
        out = {"ok": True, "schema": SCHEMA_VERSION, "discord_configured": configured,
               "replays_configured": not cfg.replays_missing()}
        if cfg.fake_discord:
            out["fake_discord"] = True
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
        missing = [] if cfg.fake_discord else cfg.discord_missing()
        if missing:
            raise ServiceError(503, "discord_not_configured", "the service is missing " + ", ".join(missing))
        if not app.state.link_limit.allow(client_key(request), float(svc().now())):
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
        return {"link_id": link_id, "challenge": b64e(challenge), "expires_in": LINK_TTL}

    def live_link(link_id: str):
        c = svc()
        row = c.db.one("SELECT * FROM links WHERE link_id = ?", (str(link_id or ""),))
        if row is None or row["created"] < c.now() - LINK_TTL:
            raise ServiceError(410, "link_expired", "start linking again")
        return row

    @app.post("/v1/link/discord")
    async def link_discord(request: Request):
        if not app.state.link_limit.allow(client_key(request), float(svc().now())):
            raise ServiceError(429, "rate_limited", "too many link attempts, try again later")
        body = await json_body(request)
        row = live_link(body.get("link_id"))
        token = body.get("access_token")
        if not isinstance(token, str) or not 0 < len(token) <= MAX_TOKEN or not token.isprintable():
            raise ServiceError(400, "bad_access_token", "access_token must be the Discord access token")
        if row["status"] not in ("pending", "authorized"):
            raise ServiceError(409, "link_used", "this link was already used, start again")
        who = await app.state.discord.whoami(token)  # the token is used for this one call and never stored
        c = svc()
        acc_row = c.account_for_discord(who["id"], who["username"] or None)
        if row["status"] == "authorized" and row["account"] != acc_row:
            raise ServiceError(409, "link_used", "this link belongs to another Discord account, start again")
        c.db.x("UPDATE links SET account = ?, status = 'authorized' WHERE link_id = ?", (acc_row, row["link_id"]))
        acc = c.account(acc_row)
        if acc["banned"]:
            raise ServiceError(403, "banned", "this account is banned")
        out = {"status": "authorized", "needs_name": acc["name"] is None, "suggested_name": suggest_name(who),
               "discord_handle": who["username"]}
        if acc["name"]:
            out["display"] = f"{acc['name']}#{acc['tag']:04d}"
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
        row, _ = await signed_call(request, app.state.refresh_limit)
        return ticket_response(svc().issue_ticket(row))

    # ---- the player's own settings (signed) ----
    @app.post("/v1/profile/visibility")
    async def profile_visibility(request: Request):
        row, body = await signed_call(request, app.state.refresh_limit)
        try:
            hidden = json.loads(body or b"{}").get("hidden")
        except (ValueError, AttributeError):
            hidden = None
        if not isinstance(hidden, bool):
            raise ServiceError(400, "bad_json", 'the body must be {"hidden": true|false}')
        svc().set_stats_hidden(row, hidden)
        return {"hidden": hidden}

    # ---- match results (spec section 7, 7c; STATS_DESIGN.md 4.3-4.5) ----
    @app.post("/v1/report")
    async def report(request: Request):
        row, body = await signed_call(request, app.state.report_limit, G.max_report_body(svc().db))
        return app.state.results.report(row, request.state.client_pub, body)

    # ---- shared replays (STATS_DESIGN.md 4.6, replays.py) ----
    @app.post("/v1/replay")
    async def replay(request: Request, match_id: str = ""):
        rp = app.state.replays
        rp.enabled()  # 503 replays_disabled before anything else
        row, body = await signed_call(request, app.state.replay_limit, cfg.replay_max_bytes,
                                      pre=lambda acct: rp.precheck(acct, match_id))
        return await run_in_threadpool(rp.upload, row, match_id, body)

    # ---- discovery: where the stats site and replays live (no domain in launcher code) ----
    @app.get("/v1/config")
    async def public_config():
        return {"stats_url": cfg.stats_url, "replay_base": cfg.replay_public_base, "web_base": cfg.web_base}

    # ---- published records ----
    @app.get("/v1/keyset")
    async def keyset():
        b, ks = svc().keyset()
        return Response(content=b, media_type="application/octet-stream",
                        headers={"X-PC-Keyset-Serial": str(ks.serial)})

    @app.get("/v1/deny")
    async def deny(gen: Optional[int] = None):
        c = svc()
        return Response(content=c.deny_list(gen), media_type="application/octet-stream",
                        headers={"X-PC-Deny-Gens": ",".join(str(g) for g in c.deny_gens())})

    return app


def __getattr__(name):  # `uvicorn app:app` builds the app on first use, so importing for tests needs no config
    if name == "app":
        globals()["app"] = create_app()
        return globals()["app"]
    raise AttributeError(name)
