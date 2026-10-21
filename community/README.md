# PovertyCaster community service: Discord linking and tickets

First slice of the community service (fm2k-port `docs/COMMUNITY_SERVICE.md`, ticket formats in
`docs/COMMUNITY_TICKET_SPEC.md` v9.1). A player links Discord ONCE; the service issues a signed PCTKT1 ticket that
peers verify offline, renews it silently, and publishes the root-signed KEYSET and the deny list (bans). Records,
stats, chat and uploads come later.

Standalone FastAPI app. It does not touch the live hub (`hub/`).

## What is where

| File | What |
|---|---|
| `app.py` | the HTTP endpoints |
| `service.py` | accounts, names and #tags, ticket issuance, KEYSET choice, deny list |
| `tickets.py` | PCTKT1 / PCKEYS1 / PCDENY1 encode, sign and verify, the link, proof and call messages (byte-identical to pc-core) |
| `names.py`, `name_table.py` | section 10 name rules; the table is generated from pc-core `NameTable.inc` by `gen_name_table.py` |
| `db.py` | SQLite schema and migrations-lite (`PRAGMA user_version`) |
| `admin.py` | server CLI: online key, unsigned KEYSET body, ban, revoke, stats |
| `sign_keyset.py` | the owner's offline signer (runs where the root keys are; needs only `cryptography`) |
| `fake_discord.py`, `dev_run_local.sh` | LOCAL DEV MODE: fake Discord tokens and throwaway test roots, loopback only |
| `tests/` | pytest, including the pinned C++ vectors |

## Endpoints

Public base: `https://2dfm.org/community` (Caddy strips the prefix; the app sees the paths below).

| Method and path | Body / headers | Answer |
|---|---|---|
| `GET /health` | | `{ok, schema, discord_configured, ticket_key, keyset, issuing}`, no secrets |
| `POST /v1/link/start` | `{"client_pub": b64(32)}` | `{link_id, challenge: b64(32), expires_in}` |
| `POST /v1/link/discord` | `{"link_id", "access_token"}` | `{status: authorized, needs_name, display?, suggested_name, discord_handle}` |
| `POST /v1/link/finish` | `{"link_id", "signature": b64(64), "name"?}` | `{ticket: b64(252), account_id, name, tag, display, refresh_after, discord_handle}` |
| `POST /v1/ticket/refresh` | section 6 headers `X-PC-Ticket`, `X-PC-Time`, `X-PC-Nonce`, `X-PC-Sig` | same as finish |
| `GET /v1/keyset` | | the current KEYSET bytes (`application/octet-stream`, `X-PC-Keyset-Serial`) |
| `GET /v1/deny` | | the PCDENY1 deny list bytes, signed by the ticket key |

Errors are `{"error": code, "detail": text}`. Every response carries `X-PC-Server-Time`.

The link flow (spec section 9). There is ONE Discord login: the launcher's Discord Social SDK sign-in.
1. The launcher makes its client key and calls `link/start` with the public half, keeping `link_id` secret.
2. It signs in through the Discord Social SDK (the full social bundle plus `identify`; Public Client, redirect
   `http://127.0.0.1/callback`). That token stays in the launcher for presence, invites and friends.
3. It sends the access token once to `link/discord`. The service asks Discord `GET /api/v10/oauth2/@me`, refuses a
   token issued to another application (`discord_wrong_app`), maps the Discord user to an account (a random 16-byte
   account_id on first sign-in, never derived from Discord) and forgets the token. `needs_name` says whether the
   player must choose a name; `suggested_name` is their Discord display name (or username) when it passes the name
   rules, for the launcher to prefill.
4. The launcher signs `PC-COMMUNITY-LINK-V1\n || challenge || client_pub` and calls `link/finish` with the name.
   The service checks the proof of possession, normalizes the name, assigns the #tag (unique per name skeleton)
   and returns the ticket.

Linking again with a new client key keeps the account_id and denies every unexpired ticket of the old key.

Renewal: tickets live 48 h (the client verifier refuses more than 72 h). The launcher calls `ticket/refresh` at
`refresh_after` (half-life) as a signed call. The host it signs is `COMMUNITY_PUBLIC_HOST` (`2dfm.org`) and the path
is the public one, `/community/v1/ticket/refresh`. A ticket of any age refreshes while the account is linked to
that client key and not banned, so the player never signs in again and never sees an expiry.

Privacy: no response carries a Discord id or an IP. Discord ids live only in `accounts.discord_id`, and the Discord
access token is never stored. The player's own Discord username (`discord_handle`) and name suggestion go back only to
that player (link/discord, link/finish, ticket/refresh), so their settings can show which Discord account is linked.
Other players see `Name#1234`; names are public (they are in the ticket).

## Run locally

Keep the venv off `/mnt` (drvfs is slow):

```bash
python3 -m venv ~/.cache/pc-community-venv
~/.cache/pc-community-venv/bin/pip install -r community/requirements.txt
cd community
~/.cache/pc-community-venv/bin/python -m pytest -q tests
~/.cache/pc-community-venv/bin/uvicorn app:app --host 127.0.0.1 --port 8090
```

Without Discord credentials the app still starts: `/health` shows what is missing and `link/start` answers 503
`discord_not_configured`. Without the ticket key or a KEYSET, issuing answers 503 with what to run.

## Local dev mode (fake Discord, test roots)

End-to-end testing without the real Discord application or the real roots:

```bash
cd community && ./dev_run_local.sh            # --port N, --keep, --data DIR
```

It makes a data dir under `~/.cache/pc-community-dev/` (removed on exit unless `--keep`), generates two THROWAWAY test
roots there (never the real ones), an online ticket key gen 1 and a KEYSET serial 1 signed by test root a, installs it,
and runs uvicorn on `127.0.0.1:8790` with `COMMUNITY_FAKE_DISCORD=1`, `COMMUNITY_PUBLIC_HOST=127.0.0.1` and an empty
`COMMUNITY_ROOT_PATH`. That is what a client with `community.url=http://127.0.0.1:8790` signs: pc-core
`splitBaseUrl` takes the host without its port and the path after it. It prints the base URL and the test root public
keys.

In fake mode `link/start` needs no Discord client id and `link/discord` takes an access token `fake:<id>[:<name>]`
instead of asking Discord (a second id is a second account). The launcher sends `PC_COMMUNITY_FAKE_DISCORD_TOKEN`
instead of its SDK sign-in when its `community.url` is loopback, and trusts `PC_COMMUNITY_TEST_ROOTS` the same way.
Accounts, names, tickets, the KEYSET, the deny list, bans and refresh all run for real (`admin.py` works with the same env).

Guards: the app refuses to start in fake mode unless `COMMUNITY_BIND_HOST` and `COMMUNITY_PUBLIC_HOST` are both
loopback (production signs `2dfm.org`, so it can never come up there), and every request is refused (403
`fake_discord_local_only`) from a non-loopback peer or with `X-Forwarded-For` (a proxy in front).

## Configuration

Environment variables; any not set are read from `COMMUNITY_CONFIG` (default `community/data/community.env`,
`KEY=VALUE` lines). `community/data/` is gitignored.

| Variable | Default | |
|---|---|---|
| `COMMUNITY_DISCORD_CLIENT_ID` | (required for linking) | the PovertyCaster Discord application; a token issued to any other app is refused |
| `COMMUNITY_DB` | `community/data/community.sqlite3` | |
| `COMMUNITY_TICKET_KEY` | `community/data/ticket_key.txt` | the online key file (0600) |
| `COMMUNITY_KEYSET_DIR` | `community/data/keysets` | signed `*.pckeys`; the newest valid one is served |
| `COMMUNITY_PUBLIC_HOST` | `2dfm.org` | the host clients sign into calls |
| `COMMUNITY_ROOT_PATH` | `/community` | the public prefix, part of the signed path |
| `COMMUNITY_DENY_VALID_SECONDS` | `86400` | deny list valid_until - issued |
| `COMMUNITY_DENY_REFRESH_SECONDS` | `900` | the deny list is re-signed at least this often |
| `COMMUNITY_CONFUSABLES` | (unset) | path to Unicode `confusables.txt` for the full UTS #39 skeleton |
| `COMMUNITY_ROOT_PUBKEYS` | (unset) | staging/tests only: `hexA,hexB` instead of the compiled-in roots |
| `COMMUNITY_FAKE_DISCORD` | (unset) | `1`: LOCAL DEV MODE (below). Refused unless bind and public host are loopback |
| `COMMUNITY_BIND_HOST` | (unset) | dev mode: the uvicorn `--host`, must be loopback |

The two root PUBLIC keys are built in (`tickets.py`, from COMMUNITY_SERVICE.md section 7): root A `c9e1bf09...`,
root B `b597400b...`. The service never reads a root private key.

## Deploy (the owner, on the droplet)

Not deployed yet. Suggested layout, like the other services there:

`/etc/systemd/system/pc-community.service`:

```ini
[Unit]
Description=PovertyCaster community service
After=network.target

[Service]
Type=simple
User=fm2k
WorkingDirectory=/home/fm2k/wanwan/community
EnvironmentFile=/home/fm2k/wanwan/community/data/community.env
ExecStart=/home/fm2k/pc-community-venv/bin/uvicorn app:app --host 127.0.0.1 --port 8090 --no-access-log
Restart=on-failure
RestartSec=5
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=strict
ReadWritePaths=/home/fm2k/wanwan/community/data
ProtectHome=read-only

[Install]
WantedBy=multi-user.target
```

`--no-access-log` keeps client IPs out of the journal (spec section 11 allows connection logs for 30 days only;
set Caddy's log retention to match if Caddy logs requests).

Caddy, inside the existing `2dfm.org` site block:

```
handle_path /community/* {
    reverse_proxy 127.0.0.1:8090
}
```

`handle_path` strips `/community`, so the app sees `/v1/...`. Caddy adds
`X-Forwarded-For`, which the app reads only for in-memory rate limits.

## The owner's steps

1. **Discord application.** The launcher's own Social SDK application (PovertyCaster, 1558033167527575594). Under
   OAuth2 turn **Public Client** on and add the redirect `http://127.0.0.1/callback` (the SDK sign-in). Put its id in
   `community/data/community.env` on the server:

       COMMUNITY_DISCORD_CLIENT_ID=...

   The service needs no client secret: it only asks Discord who a launcher's token belongs to.
2. **Online ticket key** (on the server):

       cd community && python3 admin.py gen-ticket-key --gen 1

   It writes `data/ticket_key.txt` (0600) and prints the public key.
3. **KEYSET body** (on the server), with that public key:

       python3 admin.py keyset-body --serial 1 --root a --ticket-key 1:<public key hex> --out keyset_1.body

   Defaults: issued now, valid 45 days, the key's window from now to valid_until + 30 days. Pre-sign the next
   months the same way with later `--issued` values and higher serials.
4. **Sign offline** on the machine holding the root keys (copy `keyset_1.body` and `sign_keyset.py` there):

       python3 sign_keyset.py keyset_1.body root_a.private.pem root_b.private.pem keyset_1.pckeys

   Only the root the body names is opened (pass `-` for the other). The tool refuses a key that is not that root.
5. **Install** on the server: `python3 admin.py install-keyset keyset_1.pckeys`, then start the service and check
   `https://2dfm.org/community/health` shows `"issuing": {"key_gen": 1}`.

Moderation: `admin.py ban "Name#1234" --reason ...` (live at the next deny-list fetch, refresh refused),
`admin.py unban`, `admin.py revoke-ticket --gen 1 --serial N`, `admin.py show`, `admin.py stats`. Every action goes to
the `audit` table.

## Known limits of this slice

- Perks records (section 4) are not issued yet; refresh returns only the ticket.
- The deny list is signed for the current key generation only; after a rotation, keep serving the old generation's
  list for 72 h (not automated yet).
- The skeleton is a simplified UTS #39 unless `COMMUNITY_CONFUSABLES` points at `confusables.txt`.
- Rate limits are in memory (reset on restart).
