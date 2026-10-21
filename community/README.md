# PovertyCaster community service: Discord linking, tickets and match results

First slice of the community service (fm2k-port `docs/COMMUNITY_SERVICE.md`, ticket formats in
`docs/COMMUNITY_TICKET_SPEC.md` v9.1). A player links Discord ONCE; the service issues a signed PCTKT1 ticket that
peers verify offline, renews it silently, and publishes the root-signed KEYSET and the deny list (bans). Records,
stats come from signed match receipts (`POST /v1/report`, below); inputs-only replays of recorded matches go to R2
(`POST /v1/replay`, below); chat comes later.

Standalone FastAPI app. It does not touch the live hub (`hub/`).

## What is where

| File | What |
|---|---|
| `app.py` | the HTTP endpoints |
| `service.py` | accounts, names and #tags, ticket issuance, KEYSET choice, deny list |
| `tickets.py` | PCTKT1 / PCKEYS1 / PCDENY1 encode, sign and verify, the link, proof and call messages (byte-identical to pc-core) |
| `names.py`, `name_table.py` | section 10 name rules; the table is generated from pc-core `NameTable.inc` by `gen_name_table.py` |
| `db.py` | SQLite schema and migrations-lite (`PRAGMA user_version`) |
| `receipts.py` | RESULT-V1 receipts, PCCHAIN1/PCSLOT1 chains, the rules between receipts, the 7c receipt list (byte-identical to pc-core) |
| `counting.py` | the counting decision for one match key from its receipts (pure) |
| `results.py` | `/v1/report`: verification, storage, timing (pending, late, 7-day close, caps), the `results.jsonl` export |
| `replays.py` | `/v1/replay`: the PCRZ1 / inputs-only .pcrep checks, the abuse caps, the R2 PutObject |
| `games.py` | the per-game table, imported from fm2k-port's `gen_game_table.py --format tsv` |
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
| `POST /v1/profile/visibility` | signed, `{"hidden": bool}` | `{hidden}`; republishes `export/hidden.json` |
| `POST /v1/report` | signed, `application/octet-stream` receipt list (below) | 200 `{status, match_id}`; errors below |
| `POST /v1/replay?match_id=<id>` | signed (the query is part of the signed path), `application/octet-stream` PCRZ1 (below), at most 1 MiB | 200 `{replay_id, replay_url}`; errors below |
| `GET /v1/config` | | `{stats_url, replay_base, web_base}` |
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

## Match results (`POST /v1/report`)

A signed call (section 6 headers; the signature covers `sha256(body)`) whose body is exactly one spec 7c receipt
list: `u16 n | n x { u16 len (1..512) | receipt }`, each receipt `fields || Ed25519 signature` (RESULT-V1, section 7),
sorted by (signer_slot, frame, kind, bytes), no duplicates, at most 2 per (signer_slot, frame, kind), `n` in
1..max_tape_receipts of the game row. The launcher sends the signer's result (or last progress) plus every receipt it
holds from both sides. Content-Length above the largest row's cap (928286 for the default row) is refused before a
byte is read.

Checks: every receipt names one match (session key, match index, game, build, rounds_to_win, started_unix); a
`games` row exists for (game_id, build_id); no zero state_hash; the reporter signed at least one slot (both slots =
self-play); a new match key needs both started receipts, and the opponent is the account whose client key verifies
its started receipt among the accounts holding a ticket at `started_unix` (receipts carry no ticket); every receipt
verifies under its signer_slot's key; frames fit the wall time since `started_unix`; reports close 7 days after it.

| Answer | When |
|---|---|
| 200 `{"status", "match_id"}` | stored. status `open` (no outcome yet), `pending` (counted, rated at once, `final` after 24 h), `left_early`, `disputed`, `void`, `dropped`. A byte-identical re-send answers the same and touches nothing |
| 400 `bad_receipt` | the list or a receipt fails parsing, mixes matches, does not verify, or a frame is implausible |
| 403 `not_participant` / `self_play` / `anonymous_peer` | the caller signed nothing here or is not in the match; one player in both slots; no linked (unbanned) opponent |
| 409 `too_late` / `closed` | more than 7 days after the match; a dispute that already closed void |
| 413 `body_too_large` | over the cap (checked before reading) |
| 422 `unknown_game` / `no_started_pair` / `no_evidence` | no game row; a new match without both started receipts; a zero state_hash |
| 429 `cap_daily` + `Retry-After` | 300 new reports per account per UTC day, or a 4th left-early result filed by one stayer that day (retried the next day) |
| 429 `rate_limited` | the per-client window (120 per 10 min) |

Counting (`counting.py`, spec section 7 with the owner's 2026-10-21 rulings): agreeing results count (a state_hash
difference only adds the `desync` flag); a lone valid result counts; a claim contradicted by the other side's later
receipt is stale (strike, the other result counts) and two reachable contradicting claims are `disputed` (no W/L,
closes `void` at 7 days); a progress/started desync is `void` (on a game with a replay worker both of these are
`disputed` for the replay); left early (a stayer's 0xFE result within the gate of the leaver's latest progress) is a
rated loss for the leaver plus `left_early` (and `left_early_behind`) marks; both claiming the other left is `dropped`.
After the 24 h pending window a contradicting report never flips the result: `late_claim` mark and flag (or
`disputed` on a worker game). Past 20 counted matches per pair per UTC day the row carries `pair_cap` (not rated).

Export: every state change of a recorded match appends one line to `export/results.jsonl` (fsync per line, latest
line per `match_id` wins), the shape of `docs/STATS_DESIGN.md` 4.4: `match_id`, `game_id` `"<family>:<catalog id>"`,
`build`, `status`, `started_at`, `finished_at`, `rounds_to_win`, `p1`/`p2` `{public_id, name, tag (4 digits),
hidden, char: null}`, `winner` (`p1`/`p2`/`draw`/null), `score`, `left_early`, `flags`, `replay_url` (null until a
replay is stored; the upload appends a new line with it). No
Discord id, account id, session key, signature or IP. The stats site reads it at `FM2K_RESULTS_PATH`.

Server steps: `admin.py import-games game_table.tsv` (from fm2k-port `tools/community/gen_game_table.py --format
tsv`; a game/build without a row records nothing), set `FM2K_STATS_ID_SECRET` (public ids), install
`deploy/pc-community-settle.{service,timer}` (hourly `admin.py settle`; reports also settle lazily), point the stats
site's `FM2K_RESULTS_PATH` at `community/data/export/results.jsonl` and `FM2K_COMMUNITY_HIDDEN_PATH` at `hidden.json`.
`admin.py reexport` appends every match's current state (after the secret was set late, or a name change).

## Shared replays (`POST /v1/replay`)

Owner rules: inputs-only and compressed, uploaded by default, attached to a RECORDED match, public to anyone, kept
forever, player names kept as-is (`CH_SESSION_NAMES`). Stored in Cloudflare R2 through the S3 API (boto3).

After `/v1/report` answered a `match_id` with a recorded status, the launcher sends a signed call
`POST /community/v1/replay?match_id=<12 hex>`. The signed path INCLUDES the query
(`/community/v1/replay?match_id=<id>`), so the match id cannot be swapped in transit. Body: one PCRZ1 file,
little-endian:

| Offset | Field | |
|---|---|---|
| 0 | `"PCRZ"` | magic |
| 4 | u16 version | 1 |
| 6 | u16 header_len | bytes before the payload: 26 + len(game_id) + len(build_id) |
| 8 | u32 match_index | the receipts' match_index; must equal the match's |
| 12 | u32 frames | battle frames of the match, 1..`max_match_frames` of the game row |
| 16 | u32 raw_len | decompressed size, 1..16 MiB |
| 20 | u32 z_len | header_len + z_len must equal the body length exactly |
| 24 | u8 n, n bytes | game_id (ASCII, 1..64), equal to the match's (the receipts' catalog id) |
| | u8 n, n bytes | build_id (ASCII, 1..64), equal to the match's |
| header_len | z_len bytes | ONE zstd frame (window at most 8 MiB) of an inputs-only `.pcrep` |

The inner `.pcrep` is pc-replay `ReplayFormat.hpp` version 14 (88-byte FileHeader `PCRP`), complete and CRC-clean
chunk by chunk, with `inputSize` equal to the game row's `input_width`; exactly one `CH_SESSION` before exactly one
`CH_MATCH_START` of this match index; at most one `CH_MATCH_END` (same index, `frames` equal to the wrapper's; absent
for a match that ended early) and at most one `CH_SESSION_NAMES`. Allowed chunks: `CH_GEN`, `CH_INPUTS`, `CH_CHECK`,
`CH_ROUND_END`, `CH_RETRY`, `CH_END`, `CH_TAPE`, `CH_TAPE_MARK`, `CH_TAPE_CHECK`, `CH_TAPE_END`, `CH_SESSION`,
`CH_MATCH_START`, `CH_MATCH_END`, `CH_SETUP_EXT`, `CH_CSS_LIVE`, `CH_SESSION_NAMES`, `CH_MATCH_CHARS`. Refused:
`CH_GEN_STATE`, `CH_GEN_STATE_Z` (snapshots), `CH_CHAT`, and any unknown type.

Stored as `replays/v1/<family>/<game_id>/<yyyy-mm of started_at>/<replay_id>.pcrz` (`replay_id` = 16 random hex, no
account or session data), `Content-Type: application/octet-stream`, `Cache-Control: public, max-age=31536000,
immutable`, no lifecycle rule. The database keeps the key (`replays` table, schema 6); `replay_url` =
`COMMUNITY_REPLAY_PUBLIC_BASE + "/" + key`, composed at answer and export time, so moving the public domain needs no
rewrite.

Limits (nobody can drain the bucket; every check runs before a byte goes to R2, and the ticket, match and cap checks
run before the body is read):

| Answer | When |
|---|---|
| 200 `{"replay_id", "replay_url"}` | stored; or the match already has a replay (first valid upload wins: a re-upload by either player answers the same URL and stores nothing, also past a cap) |
| 400 `bad_replay` / `bad_match_id` | the file fails any check above (detail says which); the match id is not 12 hex |
| 403 `not_participant` | the caller is not one of the match's two players |
| 409 `match_unknown` | no such match, or it is not recorded (`pending`/`final`/`left_early`) yet: retry like `not_disputed` |
| 409 `replay_busy` + `Retry-After: 30` | the other player's upload of this match is in flight (a reservation older than 5 min is reclaimed) |
| 413 `body_too_large` | above 1 MiB (`COMMUNITY_REPLAY_MAX_BYTES`, never above 1 MiB), from Content-Length before reading and while streaming |
| 429 `cap_daily` + `Retry-After` | the account stored `COMMUNITY_REPLAY_ACCOUNT_DAILY` (200) replays this UTC day |
| 429 `rate_limited` | the per-client window (60 per 10 min) |
| 502 `replay_store_failed` | R2 refused; nothing is kept, retry later |
| 503 `replays_paused` + `Retry-After` | the global breaker: `COMMUNITY_REPLAY_DAILY_UPLOADS` (20000) uploads or `COMMUNITY_REPLAY_DAILY_BYTES` (2 GiB) stored this UTC day |
| 503 `replays_disabled` | any of the four `COMMUNITY_R2_*` or `COMMUNITY_REPLAY_PUBLIC_BASE` is unset |

A stored replay appends a new `results.jsonl` line for its match carrying `replay_url`; the stats site passes it
through (`ratings.parse_result`, `api_v1.match_obj`).

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
| `COMMUNITY_PUBLIC_HOST` | `2dfm.org` | the host(s) clients sign into calls; a comma list during a domain move (`2dfm.org,povertycaster.org`) |
| `COMMUNITY_ROOT_PATH` | `/community` | the public prefix, part of the signed path |
| `COMMUNITY_DENY_VALID_SECONDS` | `86400` | deny list valid_until - issued |
| `COMMUNITY_DENY_REFRESH_SECONDS` | `900` | the deny list is re-signed at least this often |
| `COMMUNITY_CONFUSABLES` | (unset) | path to Unicode `confusables.txt` for the full UTS #39 skeleton |
| `COMMUNITY_ROOT_PUBKEYS` | (unset) | staging/tests only: `hexA,hexB` instead of the compiled-in roots |
| `COMMUNITY_FAKE_DISCORD` | (unset) | `1`: LOCAL DEV MODE (below). Refused unless bind and public host are loopback |
| `COMMUNITY_BIND_HOST` | (unset) | dev mode: the uvicorn `--host`, must be loopback |
| `FM2K_STATS_ID_SECRET` | (unset) | the stats site's id secret: `accounts.public_id`, the results export's player ids |
| `COMMUNITY_EXPORT_DIR` | `community/data/export` | `hidden.json` and `results.jsonl` for the stats site |
| `COMMUNITY_STATS_URL`, `COMMUNITY_REPLAY_PUBLIC_BASE`, `COMMUNITY_WEB_BASE` | (unset) | served by `/v1/config`; the replay base also composes every `replay_url` |
| `COMMUNITY_R2_ENDPOINT`, `COMMUNITY_R2_BUCKET`, `COMMUNITY_R2_ACCESS_KEY_ID`, `COMMUNITY_R2_SECRET_ACCESS_KEY` | (unset) | R2 (S3 API) for shared replays; any missing -> `503 replays_disabled` |
| `COMMUNITY_REPLAY_MAX_BYTES` | `1048576` | one upload (clamped to 1 MiB) |
| `COMMUNITY_REPLAY_ACCOUNT_DAILY` | `200` | stored replays per account per UTC day |
| `COMMUNITY_REPLAY_DAILY_UPLOADS`, `COMMUNITY_REPLAY_DAILY_BYTES` | `20000`, `2147483648` | the global daily breaker |

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

Backups: `deploy/pc-community-backup.{service,timer}` run `admin.py backup --keep 30` daily (an online SQLite snapshot
into `data/backups/`, 0600). The unit files for the service itself are in `deploy/` too.

Moderation: `admin.py ban "Name#1234" --reason ...` (live at the next deny-list fetch, refresh refused),
`admin.py unban`, `admin.py revoke-ticket --gen 1 --serial N`, `admin.py show`, `admin.py stats`. Every action goes to
the `audit` table.

## Known limits of this slice

- Results: no `/v1/tape`, worker or `/v1/verdict` yet, so a game with a worker keeps its disputes until the 7-day void
  close. `/v1/replay` does not yet compare the replay's inputs with the receipts' `inputs_chain`. The spec's "frame gap at most ~1320" check is not enforced (a
  report holding a gap is accepted). The opponent of a new match is found only while it still holds the client key
  that signed (a relink before the first report leaves that match unrecordable).
- Perks records (section 4) are not issued yet; refresh returns only the ticket.
- After a key rotation, list the old key file in `COMMUNITY_RETIRED_TICKET_KEYS`: `/v1/deny?gen=N` keeps serving that
  generation's deny list until 72 h past its last ticket (`X-PC-Deny-Gens` names every generation served). The
  launcher still fetches only the current generation (`/v1/deny`); fetching each listed generation is pending there.
- Names use the full UTS #39 table (`unicode/confusables.txt`, Unicode 16.0.0) on top of case folding. After changing
  the table, run `admin.py reskeleton` (recomputes skeletons and look-alike flags; the oldest account keeps a #tag).
- Rate limits live in the database (`rate_hits`): a salted HMAC of the client address, salt rotated daily, rows
  deleted as they leave the 10-minute window. No address is ever stored.
