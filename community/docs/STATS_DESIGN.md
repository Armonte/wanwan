# Stats, leaderboards and replay sharing: design (2026-10-21)

Scope: PovertyCaster (PC) match results -> recorded matches -> per-game Glicko-2 + W/L -> the launcher Statistics tab
and public profiles on the existing stats site; then inputs-only replays in Cloudflare R2 attached to recorded matches.
Every PC game is designed for; FM2K / FM95 are wired first. Analysis only: nothing here is deployed.

Sources read: wanwan `stats/` and `community/` (HEAD of `bleeding`), the droplet (read-only), fm2k-port
`docs/COMMUNITY_SERVICE.md` and `docs/COMMUNITY_TICKET_SPEC.md` v9.2, PovertyCaster `origin/main` (`pc-core`,
`pc-runtime`, `pc-replay`, `targets/pc-ui`), fm2k-port `shell/fm2k/pcrt/`.

---

## 1. Current state

### 1.1 The stats site (wanwan `stats/`)

- FastAPI, read-only over the **wanwan hub's** `matches.json` (`stats/app.py:53` `FM2K_MATCHES_PATH`, default
  `../hub/matches.json`). No DB; the README's "why no DB" says re-parse per request, switch to SQLite past ~500k rows.
- HTML: `/`, `/games`, `/u/{public_id}`, `/g/{game_id}`, `/g/{game_id}/c/{char}`, `/recent`, `/sessions`
  (`stats/app.py:669-836`), plus a bearer-token `/admin` (`stats/admin.py`, `admin_tokens.json`).
- JSON API v1 (`stats/api_v1.py:218-259`, spec `stats/API.md`): `/api/v1/games`, `/api/v1/g/{id}/leaderboard`,
  `/api/v1/u/{public_id}`, `/api/v1/recent`, legacy `/api/v1/matches`. Glicko-2 per game id (`stats/glicko2.py`,
  `stats/ratings.py`; tau 0.5, 1500/350/0.06, each match its own period, cache keyed by log mtime/size).
- Counting (`stats/API.md` "What counts"): both sides Discord ids, no self-play, mutual disconnect = no contest,
  one-sided disconnect follows the hub, draw = 0.5, timeout-draw repair from schema-2 `rounds[]`.
- Privacy (`stats/privacy.py`): `public_id = HMAC-SHA256(secret, discord_id)[:16 hex]` (`privacy.py:65-71`), secret
  from `FM2K_STATS_ID_SECRET` / `FM2K_STATS_ID_SECRET_FILE` / a logged dev fallback (`privacy.py:32-49`). Display
  `tag` = first 4 hex of the public id. Opt-out = `stats/optout.json` (Discord ids), private stub on the profile,
  still rated, still named in match rows.
- Game ids are used verbatim; `ratings.game_family()` (`ratings.py:54`) maps `fam:<id>` and bare legacy exe stems.
  Registry aliases (`app.py:207-245`, `games/registry.json` `aliased_ids`) are used by the HTML pages only; the API
  does not merge aliases (`API.md` "Game ids and families").

### 1.2 What is deployed (droplet `2dfm`, read-only inspection 2026-10-21)

- `fm2k-stats.service`: `/home/fm2k/stats`, `uvicorn app:app --host 127.0.0.1 --port 8000 --workers 2`,
  `EnvironmentFile=-/home/fm2k/stats/.env` (only `HUB_STATE_DIR` set; **no `FM2K_STATS_ID_SECRET`**), hardening
  `ProtectHome=read-only`, `ReadWritePaths=/home/fm2k/stats`. The deployed code is the **May 10** copy (flat
  layout, `app.py` + `admin.py`, no `api_v1.py` / `ratings.py` / `privacy.py` / `glicko2.py`): the v1 API, ratings and
  public ids exist only in the repo (commits `97a73a5`, `1fbfd81`).
- Caddy: `stats.2dfm.org` -> `127.0.0.1:8000`, **basic-auth gated** except `/admin` and `/static`. There is **no
  `2dfm.org/stats` route**: `https://2dfm.org/stats/api/v1/games` answers 200 `2dfm forever` (the apex catch-all).
  `2dfm.org/community/*` -> `127.0.0.1:8090` (`pc-community.service`, live: `/health` shows key_gen 1, KEYSET 1).
- Data: `/home/fm2k/hub/matches.json`, **46.6 MB, 43,738 rows** (schema 2), every row with both Discord ids. By
  game: `pkmncc` 37,546, `WonderfulWorld_ver_0946` 2,634, `URORFGRelease103` 1,164, ... The hub rewrites the whole
  file (`json.dump(indent=2)`) on every commit (`hub/match_records.py:46` on the droplet). 28 dispute lines in
  `match_disputes.json`. The hub has its own `replay_store.py` (`/replay/upload|list|get`, index.jsonl, 1 entry).
- So today **no PovertyCaster match reaches any log**: the stats site sees only the wanwan FM2KHook launcher and
  PKMNCC through the old hub.

### 1.3 The community service (wanwan `community/`, live)

Accounts (`community/db.py:15-30`: random 16-byte `account_id`, server-only `discord_id`, `name`, `skeleton`,
numeric `tag`, `client_pub`, `banned`), PCTKT1 issuance, section-6 signed refresh, KEYSET, deny list. Endpoints:
`/health`, `/v1/link/{start,discord,finish}`, `/v1/ticket/refresh`, `/v1/keyset`, `/v1/deny` (`community/app.py:170-303`).
`Config.public_hosts` already accepts several hosts for the domain move (`community/config.py`). No results, no
replays, no stats.

### 1.4 The client (PovertyCaster `origin/main`)

Built and unit-tested, but **"nothing calls these yet"** (their own header comments, confirmed by `git grep`):

| Piece | File | Status |
|---|---|---|
| receipts (started/progress/result), session key, PCCHAIN1/PCSLOT1 chains, score/reachability rules | `pc-core/include/pc/community/Receipt.hpp`, `src/community/Receipt.cpp`, `ReceiptRules.cpp` | library only |
| receipt list, PCTAPE1 tape, caps | `Tape.hpp`, `ReceiptList.cpp`, `Tape.cpp` | library only |
| PCQUEUE1 queue, retry classes, file lock, game->launcher handoff | `Queue.hpp`, `QueueFile.hpp`, `Queue*.cpp` | library only, no sender |
| signed call (section 6) | `Session.hpp` `signCall` | used only by `Link.cpp` refresh |
| 0xE6 identity hello/proof carriage | `PeerIdentity.hpp` | built into pc-download, **not wired into any netplay session** |
| worker verdict layout | `Verdict.hpp` | library only |
| Link Discord, refresh, KEYSET, deny | `Link.cpp` | live |
| Statistics tab (leaderboard, my profile, recent) | `targets/pc-ui/src/CommunityStats*.cpp` (commit `6ba7b956c`) | live UI, reads the stats API |

**Server endpoints the client already calls that do not answer today:**

| Client call | Code | Today |
|---|---|---|
| `GET <stats>/api/v1/g/{game}/leaderboard?limit=` | `CommunityStats.cpp:153` | `2dfm.org/stats` is unrouted (catch-all text) |
| `GET <stats>/api/v1/recent?game=&limit=` | `CommunityStats.cpp:156` | same |
| `GET <stats>/api/v1/u/{public_id}` | `CommunityStats.cpp:159` | same |

Default base `https://2dfm.org/stats` (`CommunityStats.hpp:21`, ini `stats.baseUrl`). Even on `stats.2dfm.org` they
would 401 (basic auth) and 404 (old code). No results/report/tape/replay endpoint is called anywhere; no path name is
fixed in client code (`/v1/result` appears only as a negative test in `tests/community_ticket/ticket_test.cpp:385`).
The spec defines the report and tape **bodies and answer classes** (section 7c: 2xx removes; 409 `not_disputed`;
429 `cap_daily` + Retry-After; 401 -> refresh once; 408/425/5xx back off; other 4xx definitive) but not the paths.

"My profile": the launcher takes `stats.publicId` from the ini; `setLinkedPublicId` exists
(`CommunityStatsUi.hpp:24`) but is deliberately not fed (`UiBridgeCommunity.cpp:10-12`: the ticket's account id is not
the stats public id "until the community service returns that id").

### 1.5 Per-game match-result hooks

`IGameAdapter::matchResult(int8_t& winnerSlot, uint8_t roundsWon[4])` (`pc-core/include/pc/adapter/IGameAdapter.hpp:690`,
default false -> CH_MATCH_END records `winnerSlot = -1`). Read by `pc-runtime/include/pc/runtime/MatchOrdinal.hpp:104-113`
only on a frame whose match ordinal steps. Implemented by:

- in tree: `eneins`, `fuuka`, `mbaacc`, `qoh99` (QoH99 is 4-participant; receipts v1 are 1v1 only);
- fm2k-port: `fm2k::Fm2kSim::matchResult` (`shell/fm2k/pcrt/fm2k_sim.cpp:355`) from the engine's own round wins
  (post-match status first, so time-over wins count), plus `matchOrdinal()`. One sim serves FM2K and FM95
  (`pcrt_shell.cpp:122` `runtime_engine_tag()`); FM95's result fields are unverified.
- every other adapter (as2, azfight, efz, gd, mb*, ml2, ...) cannot report a winner yet.

---

## 2. Gap list

1. **Stats deploy:** v1 API/ratings/privacy not deployed; no secret on the server; no `/stats` route; basic auth on.
2. **Game ids:** the launcher asks `fm2k:<catalog id>` / `fm95:<id>` (`CommunityStats.cpp` `apiGameId`), the hub
   log holds exe stems (`WonderfulWorld_ver_0946`), the API matches verbatim -> the FM2K leaderboard 404s even after
   deploy. No canonical id or alias map in the API. Receipts key on the catalog `game_id` (spec section 7 rows);
   the PC FM2K runtime labels itself `fm2k`/`fm95` only (`pcrt_shell.cpp:122`).
3. **Identity join:** stats public id = HMAC(Discord id); community gives an opaque `account_id`; nothing returns
   the stats public id to the launcher. Two different `#tag` schemes (hex prefix vs the community's numeric tag).
4. **Opt-out:** stored as Discord ids in a file in the stats dir that only the stats process may write; no signed
   call lets a player toggle it.
5. **Results path, server:** no report endpoint, no match/receipt tables, no counting state machine (pending 24 h,
   final, disputed, void, left early, strikes), no game table loaded server-side.
6. **Results path, client:** no session wires the identity hello/proof (no `session_key`), no chain feeding, no
   receipt exchange over the control channel, no progress gate, no queue sender.
7. **Disputes:** no tape endpoint, no worker; spec says without a worker a dispute closes void at 7 days (v7-D2).
8. **Replays:** no inputs-only export (a `.pcrep` set carries `CH_GEN_STATE(_Z)` snapshots, `CH_CHAT`), no
   compression, no upload, no R2 wiring, no `replay_url` source (API field exists, always null).
9. **Domain:** `2dfm.org` is hardcoded in the launcher defaults and UI strings (`Link.hpp:44`,
   `CommunityStats.hpp:21`, `CommunityStats.cpp:105-115, 369-389`).
10. **Scale:** a 46 MB JSON re-parsed by two workers on each hub commit (PKMNCC commits often).
11. **Non-FM2K games** have no per-game table rows (the generator reads only the fm2k-catalog).

---

## 3. Responsibilities

| | Community service (`community/`, writer) | Stats site (`stats/`, reader) |
|---|---|---|
| identity | accounts, `public_id`, name#tag, opt-out flag | never sees Discord ids for new rows |
| results | verify signed reports, store receipts, counting state machine, strikes, disputes, verdicts | none |
| output | append-only `results.jsonl` export (section 4.4) | merges legacy `matches.json` + `results.jsonl` |
| ratings | none | Glicko-2 + W/L, leaderboards, profiles, recent (existing code) |
| replays | accept signed uploads, validate, store to R2, attach to the match | renders `replay_url` |
| HTML/API | `/v1/*` (signed writes, `/v1/config`) | `/api/v1/*`, public pages |

Rationale: the stats site stays crash-isolated and read-only (its README rule), and the service that verifies
signatures is the only writer. The file export avoids cross-process SQLite WAL reads under `ProtectHome=read-only`
(a WAL reader needs to write `-shm`).

---

## 4. Server design

### 4.1 Public id (one derivation, two services)

- Keep `public_id = HMAC-SHA256(FM2K_STATS_ID_SECRET, discord_id)[:16 hex]` (`privacy.py:65`). It is the only
  derivation that joins the 43.7k legacy rows (keyed by Discord id) with new accounts, and it never exposes the
  Discord id or the random `account_id`.
- The community service gets the **same secret** (`community.env`), computes it at account creation and stores it
  (`accounts.public_id TEXT UNIQUE`). Changing the secret later = every public URL changes, so it is written once.
- Returned to the owner only: `link/finish` and `ticket/refresh` responses gain `"public_id"`; the launcher calls
  `setLinkedPublicId` (removes the `stats.publicId` ini workaround). Others learn it only from public pages.
- Display: for a player with an account, name#tag = the community name and numeric tag (the ticket's); a legacy-only
  player keeps hub nick + hex tag. `results.jsonl` rows carry `name`/`tag` so the stats site needs no account table.

### 4.2 Schema (community DB migration 2 -> 3, `community/db.py` `MIGRATIONS`)

```sql
ALTER TABLE accounts ADD COLUMN public_id TEXT;            -- UNIQUE index; backfilled from discord_id
ALTER TABLE accounts ADD COLUMN stats_hidden INTEGER NOT NULL DEFAULT 0;
CREATE TABLE games (                                       -- loaded from fm2k-port game_table.tsv (+ PC rows)
  game_id TEXT NOT NULL, build_id TEXT NOT NULL, family TEXT NOT NULL, name TEXT,
  rounds_to_win_legal TEXT, fps INT, min_round_frames INT, double_ko INT, input_width INT,
  max_match_frames INT, worker INT, setup_layout TEXT, PRIMARY KEY (game_id, build_id));
CREATE TABLE matches (
  id INTEGER PRIMARY KEY, match_id TEXT NOT NULL UNIQUE,   -- public, random 12 hex, never derived
  session_key BLOB NOT NULL, match_index INT NOT NULL,
  game_id TEXT NOT NULL, build_id TEXT NOT NULL,
  acct0 INT NOT NULL REFERENCES accounts(id), acct1 INT NOT NULL REFERENCES accounts(id),
  started_unix INT NOT NULL, rounds_to_win INT NOT NULL,
  status TEXT NOT NULL,     -- pending | final | disputed | void | left_early | dropped
  winner_slot INT, rounds0 INT, rounds1 INT, decided_frame INT,
  flags INT NOT NULL DEFAULT 0,  -- 1 desync, 2 tail one side, 4 late claim, 8 pair cap (not rated)
  left_slot INT, first_report INT NOT NULL, settled_at INT, updated INT NOT NULL,
  UNIQUE (session_key, match_index));
CREATE TABLE receipts (match INT NOT NULL, signer_slot INT, kind INT, frame INT, sha BLOB, bytes BLOB NOT NULL,
  received INT NOT NULL, PRIMARY KEY (match, sha));
CREATE TABLE marks (account INT, match INT, kind TEXT, ts INT);   -- strike, left_early, left_early_behind, late_claim, disputed
CREATE TABLE tapes (match INT, account INT, sha BLOB, bytes BLOB, received INT, PRIMARY KEY (match, account));
CREATE TABLE verdicts (match INT PRIMARY KEY, bytes BLOB, outcome INT, received INT);
CREATE TABLE replays (replay_id TEXT PRIMARY KEY, session_key BLOB, match INT, r2_key TEXT, size INT, sha256 BLOB,
  uploader INT, uploaded INT);
```

Receipts hold opponents' signatures and session keys: never exported, deleted with the account on "reset identity".

### 4.3 Endpoints (community service; paths relative to `COMMUNITY_ROOT_PATH`)

| Method, path | Auth | Body | Answers |
|---|---|---|---|
| `GET /v1/config` | none | | `{stats_url, replay_base, web_base}` from config (domain discovery, 4.7) |
| `POST /v1/report` | signed (sec. 6) | receipt list, `application/octet-stream` (7c, `reportBodyCap` checked before reading) | 200 `{status, match_id}`; 409 `closed`/`too_late`; 400 `bad_receipt`; 403 `not_participant`/`self_play`/`anonymous_peer`; 429 `cap_daily` + Retry-After |
| `POST /v1/tape` | signed | PCTAPE1 (`tapeCap`) | 200 `{queued}`; 409 `not_disputed`; 429 `cap_daily` |
| `POST /v1/replay?match_index=` | signed | inputs-only replay, zstd (cap 1 MiB) | 200 `{replay_id, replay_url}`; 409 `match_unknown`; 413 |
| `POST /v1/profile/visibility` | signed | `{"hidden": bool}` | 200 `{hidden}` |
| `POST /v1/verdict` | worker key | REPLAY-V1 verdict | later (step 7) |
| `link/finish`, `ticket/refresh` | as today | | + `public_id` |

Error bodies keep `{"error", "detail"}`; codes match the queue's answer classes so `QueueRetry.cpp` needs no change.
Status names: `409 not_disputed` and `429 cap_daily` exactly as 7c / vector 68.

### 4.4 Results export (service -> stats site)

`data/export/results.jsonl`, append-only, one line per match **state change** (latest line per `match_id` wins),
fsync per line. The stats site reads it beside `matches.json` (new env `FM2K_RESULTS_PATH`, readable via the
existing `ProtectHome=read-only`), cache keyed by both files' (mtime, size).

```json
{"v":1,"match_id":"9c1e04d2a7b3","game_id":"fm2k:wonderfulworld","family":"fm2k","build":"cdc1f905",
 "status":"final","started_at":1792530000,"finished_at":1792530410,"rounds_to_win":2,
 "p1":{"public_id":"3f9c0a1b2c3d4e5f","name":"alice","tag":"0412","hidden":false,"char":null},
 "p2":{"public_id":"a01b...","name":"bob","tag":"7781","hidden":true,"char":null},
 "winner":"p1","score":[2,1],"flags":[],"replay_url":"https://<replay_base>/r/....pcrz"}
```

`ratings.parse_record` gains a v1-results branch; legacy rows are unchanged. Hidden state comes from the row's
`hidden` (latest wins) plus legacy `optout.json`; the service writes a small `export/hidden.json` (public ids) on
toggle so legacy-only rows of that player hide too. Characters: receipts carry none; the replay setup or a later
receipt field fills `char` (v1: null, as for schema-1 rows).

### 4.5 Receipts, D1-D3 and the rating

Counting follows spec section 7 / 7a exactly; the server state machine per match key:

| Evidence | Status | Rated? | Marks |
|---|---|---|---|
| started pair + one valid, reachable result (or both agreeing) | `pending` (24 h), then `final` | yes, as soon as pending | none |
| same winner, state_hash differs only | `pending/final` + flag desync | yes | none |
| both final claims reachable (D1, A1) | `disputed` | no | `disputed` on both |
| claim contradicted by the other's later receipt, no worker | other result `pending` | yes | `strike` on the stale claimant |
| desync (started/progress state_hash or own chain), no worker | `void` | no | none |
| stayer holds leaver's latest progress (U4) | `left_early` | **no W/L** | `left_early` (+`_behind`) on leaver |
| both claim the other left | `dropped` | no | none |
| late report (24 h..7 d), no worker (N7) | unchanged | unchanged | `late_claim` + moderator flag |
| dispute without verdict at 7 days (v7-D2) | `void` | no | counters stay |
| verdict outcome 0 / 3 (worker, v7-D1) | `final` with the reproduced result | yes | struck slot: `strike` |
| verdict outcome 1 / 4 (v7-D1, v7-D3) | `left_early` | no | struck slot: `strike` + `left_early` |
| verdict outcome 2 twice (v7-D3) | `void` | no | none |
| > 20 counted per pair per UTC day | counted, flag `pair_cap` | **no** | none |

- Glicko-2 input: every `pending`/`final` row ordered by `started_at`, winner 1 / 0, draw 0.5 (only on games whose
  `double_ko` is 1). Pending rows are rated because the stats site recomputes from the whole log on each change
  (`ratings.snapshot`), so a later flip (verdict, void at 7 days) simply recomputes; nothing is stored incrementally.
- W/L/D shown beside the rating; profiles also show strikes, left-early ratio, disputed ratio (spec section 7
  "the profile says W-L-D is social"). The rating is per `game_id` (catalog id), not per build.
- Refused up front: self-play (same account in both slots), an anonymous peer, a game/build with no `games` row,
  receipts with mismatched game_id/build_id (N3), a zero state_hash (see open question 3).

### 4.6 Replay upload to R2 (tiny files: proxy, not presigned)

Owner rules: inputs-only, compressed, uploaded by default (setting turns it off), attached to a recorded match,
public, kept forever, R2.

- **Proxy through the community service**, not presigned PUT. A 30-minute FM2K match is 180000 x 2 slots x 2 bytes
  = 720 KB raw and typically a few KB compressed; the service must parse and check the file anyway (magic, version,
  no `CH_GEN_STATE`/`CH_GEN_STATE_Z`/`CH_CHAT`, game/build equal to the match's, frame count <= `max_match_frames`,
  input chain equal to the receipts' `inputs_chain` at the result frame when the format allows). A presigned PUT
  cannot enforce any of that and would need a second "commit" call; R2 bandwidth is free either way.
- Flow: the launcher's report gets 2xx -> it builds the inputs-only file for that match from the session `.pcrep`
  (new `pc-replay` export: keep `CH_SESSION`, `CH_SESSION_NAMES` (names are public), the match's `CH_MATCH_START`..
  `CH_MATCH_END`, `CH_TAPE*`/`CH_INPUTS`, RNG marks; drop snapshots, checks and chat; zstd) -> enqueues a third
  queue kind (Replay, PCQUEUE1 is versioned per entry kind; a new kind needs a queue version bump or a separate
  small queue file) -> `POST /v1/replay` signed. 409 `match_unknown` retries like `not_disputed` (the report may land
  first on the other side). The first valid upload per match wins; a second is answered 200 with the same URL.
- Storage: `PutObject` via the S3 API (boto3, `COMMUNITY_R2_ENDPOINT`, `_BUCKET`, `_ACCESS_KEY_ID`,
  `_SECRET_ACCESS_KEY`), key `replays/v1/<family>/<game_id>/<yyyy-mm>/<replay_id>.pcrz`, `replay_id` = 16 random
  hex (no account or session data in the key), `Content-Type: application/octet-stream`,
  `Cache-Control: public, max-age=31536000, immutable`. Public read through an R2 custom domain (or `r2.dev` until
  then): `replay_url = COMMUNITY_REPLAY_PUBLIC_BASE + "/" + key`. No lifecycle rule (kept forever).
- Limits: 1 MiB per file, per-account daily cap (429 `cap_daily`), only participants of a recorded match.
- Note: the PCTAPE1 dispute tape (7a) is a different object (receipts inside, deleted at settlement or 30 days)
  and stays in the service DB, never in R2.

### 4.7 Domain-agnostic configuration

- Service: `COMMUNITY_PUBLIC_HOST` already takes `2dfm.org,povertycaster.org`; add `COMMUNITY_STATS_URL`,
  `COMMUNITY_REPLAY_PUBLIC_BASE`, `COMMUNITY_WEB_BASE`, served by `GET /v1/config`. No host literal in code paths
  other than defaults.
- Stats site: `FM2K_STATS_ROOT_PATH=/stats` (exists), new `FM2K_STATS_PUBLIC_URL` for any absolute link (OG tags,
  canonical); templates keep relative `url_for`.
- Launcher: stats base = ini `stats.baseUrl`, else `/v1/config` `stats_url` (cached beside the launcher), else the
  community URL's origin + `/stats`. Only `kDefaultCommunityUrl` (`Link.hpp:44`) stays a literal, updated once at the
  move. UI strings say "the stats site" instead of `2dfm.org/stats` (`CommunityStats.cpp:105-115, 369-389`).
- Caddy at the move: add `povertycaster.org` to the apex site address list (same `handle_path` blocks).

### 4.8 Launcher Statistics tab API

Unchanged paths (`stats/API.md`), so the shipped tab works as soon as step 1 lands:
- leaderboard `GET /api/v1/g/{game_id}/leaderboard`, recent `GET /api/v1/recent?game=`, profile
  `GET /api/v1/u/{public_id}`, games `GET /api/v1/games`.
- Additive fields: match `status` (`pending`|`final`), `match_id`, `replay_url`; profile `strikes`,
  `left_early`, `disputed`; game `aliases`. Later: `GET /api/v1/u/{a}/vs/{b}` (head-to-head) and
  `GET /api/v1/m/{match_id}`.
- Self-view of a private profile needs auth the stats site does not have; later `GET /v1/me/stats` (signed, on the
  community service, reading the same export) if the owner wants it.

---

## 5. Client work (PovertyCaster + fm2k-port), FM2K first

1. Wire the 0xE6 identity hello/proof into the netplay session (pc-runtime handshake) -> both tickets + nonces ->
   `sessionKey()`; anonymous peer = no receipts (play unaffected).
2. `RuntimeDriver`: an `InputChains` per match fed from confirmed frames only; sign started at frame 120 (slot 0's
   corrected time), progress at round starts / +600, result at the deciding frame (FM2K: the `matchOrdinal()` step,
   `matchResult()` gives winner + `roundsWon`); exchange receipts over a control tag; progress gate (+1200 frames,
   ~8 s stall = left early).
3. Game row: the runtime must label receipts with the catalog `game_id` and the `.kgt` `build_id` (today it says
   `fm2k`/`fm95`), and look up `GameRules` from a bundled `game_table.tsv`.
4. The launcher sender: drain handoff files into `.pc_community_queue`, send due entries as signed calls, apply
   the answer classes (library exists).
5. Replays: inputs-only export + upload (4.6), setting "Share my replays" default on.
6. Settings: "Show my stats publicly" -> `POST /v1/profile/visibility`; feed `setLinkedPublicId` from link/refresh.

---

## 6. Rollout order

1. **Ship the read side on legacy data (recommended first step).** Ops + small code, no client change:
   a. generate the secret once, put `FM2K_STATS_ID_SECRET=` in `/home/fm2k/stats/.env` and (for step 2) in
      `community/data/community.env`; back it up offline (losing it changes every public URL);
   b. deploy current `stats/` (api_v1, ratings, privacy, glicko2) to `/home/fm2k/stats` with `FM2K_STATS_ROOT_PATH=/stats`,
      `FM2K_MATCHES_PATH=/home/fm2k/hub/matches.json`;
   c. add `handle_path /stats/* { reverse_proxy 127.0.0.1:8000 }` to the apex block and decide on the
      `stats.2dfm.org` basic auth (keep it on the subdomain or drop it at public launch);
   d. add a game alias map to the API (canonical `fm2k:<catalog id>` <- exe stems, from `games/registry.json`
      `aliased_ids` or generated from fm2k-catalog), so `fm2k:wonderfulworld` finds `WonderfulWorld_ver_0946`;
   e. check: `/stats/api/v1/games`, a leaderboard, a profile, the launcher tab against it.
2. **Identity glue:** migration 3 (public_id, stats_hidden), `public_id` in link/refresh, `/v1/profile/visibility`,
   `export/hidden.json`, `/v1/config`; launcher feeds `setLinkedPublicId`, stats base from config.
3. **Server results:** games table import, `POST /v1/report` with full section-7 verification and the 4.5 state
   machine (pending -> final timer, 7-day close), `results.jsonl` export, stats merge; tests reuse the pinned C++
   vectors (receipts 22, 35-50; list/report 52-68).
4. **Client results for FM2K:** section 5 items 1-4 in PovertyCaster + fm2k-port; validate on loopback and under
   loss (2500+ frames) that every match produces exactly one counted row and that a killed peer gives left early.
5. **Replays:** pc-replay inputs-only export, R2 bucket + credentials, `POST /v1/replay`, `replay_url` in rows, the
   launcher upload (default on) and a "watch" button in recent matches.
6. **FM95, then other PC games:** confirm FM95 `matchResult`; add per-game rows for in-tree adapters that report
   (`mbaacc`, `eneins`, `fuuka`), then adapters as they gain `matchResult`; QoH99 waits for a 2v2 receipt version.
7. **Disputes:** `POST /v1/tape`, the wonderfulworld worker, `POST /v1/verdict`, moderator view.
8. **Retire the hub log for FM2K:** freeze `matches.json` as legacy (PKMNCC continues there until it moves), or
   convert it once to a compact file to stop the 46 MB re-parse.

---

## 6a. Owner rulings (2026-10-21) -- these override the spec and the questions below

- The stats site stays private (stats.2dfm.org basic auth) until the owner opens it; no public `/stats` route yet.
- Left early / rage-quit: a LOSS for the leaver and a WIN for the stayer (rated), plus the `left_early` mark. This
  overrides spec "never a W or L" (section 4.5 row U4 and verdict outcomes 1/4 become rated).
- Shared replays keep player names as-is forever, also after an identity reset.
- Pending matches show and rate immediately, marked `pending`; a later dispute or void recomputes.
- Public id = HMAC(stats secret, Discord id) (question 1: yes). Legacy hub history auto-claims on link (live).

## 7. Open questions

1. **Secret identity:** is HMAC(Discord id) acceptable as the long-term public id (it ties the profile to the
   Discord account, so a relink on a new Discord account is a new profile)? The alternative, HMAC(account_id),
   loses the 43.7k legacy rows' join.
2. **Legacy merge:** `COMMUNITY_SERVICE.md` section 7.5 says hub history is merged by Discord id. Should a legacy
   player who never links PC still appear (as today), or only accounts that linked?
3. **Zero state_hash:** spec section 7 says zero = "the game has none" and also "a zero state_hash match never
   counts". Games without a sim hash then can never be rated; confirm or relax per game row.
4. **Left early and the rating:** spec says never a W or L. Fightcade-style ladders often count a rage-quit as a
   loss. Keep the spec?
5. **Replay granularity:** one object per match (needs a per-match cut of a set `.pcrep`, plus whatever setup state a
   mid-set match needs; FM2K boots from `SimBootSpec` + CSS inputs, so per match is plausible) or one per set with
   rows pointing at `#m=<index>`? This doc assumes per match.
6. **Reset my identity vs keep forever:** spec section 11 deletes records on reset; replays are kept forever and
   carry names (`CH_SESSION_NAMES`). Strip names from uploaded replays, or keep them as public?
7. **Pending window display:** show pending matches in recent lists (marked) and rate them immediately (this doc),
   or only after 24 h?
8. **`stats.2dfm.org`:** keep as an alias of `/stats`, or redirect it once the apex path is public?
9. **Game id form:** `fm2k:<catalog id>` (launcher today) vs bare catalog id in receipts; this doc keeps the family
   prefix in the stats key and the bare id in receipts, joined through the `games` table.
