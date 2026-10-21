# fm2k-stats JSON API v1

Read by the launcher's Statistics tab (game leaderboard, my profile, recent
matches). All routes are `GET`, return `application/json`, and are rate
limited to 60 requests per minute per IP (HTTP 429 beyond that). Errors are
`{"error": "not_found", "what": "game" | "user"}` with HTTP 404.

Code: `api_v1.py` (routes + shapes), `ratings.py` (log parsing, rating
computation, cache), `glicko2.py`, `privacy.py` (public ids, opt-out).

## Privacy rules

- **No Discord id and no IP address ever appears in a response.** The log
  stores Discord ids; the API never serializes them.
- A player is `{"public_id", "name", "tag"}`:
  - `public_id`: first 16 hex chars of `HMAC-SHA256(secret, discord_id)`.
    Opaque, stable while the secret is stable. Used in URLs
    (`/api/v1/u/{public_id}`). A raw Discord id is not accepted there.
  - `name`: the player's most recent nick in the log.
  - `tag`: first 4 chars of `public_id`, uppercase. Render as `name#TAG`.
- Secret source, first match wins: env `FM2K_STATS_ID_SECRET`; the file
  named by `FM2K_STATS_ID_SECRET_FILE` (default `stats/id_secret.txt`,
  gitignored); a fixed dev fallback (logged as a warning; production must
  set one, or every public id changes on the next deploy).
- **Opt-out**: `stats/optout.json` (override `FM2K_STATS_OPTOUT_PATH`),
  `{"version": 1, "hidden": ["<discord id>", ...]}`. Written by the
  service (`privacy.set_hidden(discord_id, bool)`, atomic); re-read when
  its mtime/size changes. A hidden player:
  - gets the stub `{"public_id", "name", "tag", "private": true}` from
    `/api/v1/u/{public_id}` (no stats), so clients render "private"
    instead of "missing";
  - is left off every leaderboard (ranks are computed without them);
  - still appears by name + tag (and character) in every match row:
    recent matches, opponents' profiles, the legacy feed. Names are never
    private. Their player object carries `"private": true` so clients
    know the stats link resolves to the stub;
  - is still rated (opponents' ratings must not depend on who opted out).
  "Visible to themselves" needs auth, which does not exist yet; the code
  takes a `viewer` argument that is always `None` for now.

## What counts

Owner decision 2026-10-20: only Discord-linked matches count.

- Both sides must be Discord ids (15 to 21 digits). Guest ids, pre-auth
  nick ids and empty ids are excluded from everything.
- Self-vs-self records are excluded.
- `report_p1 == report_p2 == "disconnect"` is a no-contest (the hub writes
  `winner_id ""` but `match_records.reconcile_outcomes` treats it as "no
  actual match"); excluded.
- One-sided disconnect: the hub already records the survivor as winner;
  we follow it and set `"disconnect": true`.
- `winner_id ""` is a draw and scores 0.5 in the rating.
- **Timeout repair**: an FM2K hook bug (fixed in
  `FM2KHook/src/netplay/netplay_battle_end.cpp`) decided matches by final
  HP only, so a match whose last round ended on time-over was reported as
  a draw. Schema-2 records carry `rounds[]`, whose per-round winner comes
  from the engine's own result flags and was never affected. A draw whose
  `rounds[]` tally is unequal is counted as a win for the side with more
  rounds and flagged `"repaired": true`. Schema-1 draws (no `rounds[]`)
  cannot be repaired and stay draws.

### Community results (PovertyCaster)

The community service writes `export/results.jsonl` (append-only, one line per match state change; read from
`FM2K_RESULTS_PATH`, latest line per `match_id` wins). Shape: `community/docs/STATS_DESIGN.md` 4.4.

- Rated: `status` `pending` (shown and rated at once, final after 24 h), `final` and `left_early` (owner ruling
  2026-10-21: the leaver loses, the stayer wins; `"left_early": "p1"|"p2"` names the leaver, `disconnect` is true).
  `disputed`, `void` and `dropped` are not rated; a row flagged `pair_cap` (past 20 per pair per UTC day) is not rated.
- Players carry a public id, never a Discord id. A public id equal to a legacy player's (`HMAC(secret, discord id)`,
  the same secret on both services) is that player; any other is a community-only player. Their `tag` is the
  community's numeric `#1234`; legacy-only players keep the hex tag.
- Hidden: the service's `hidden.json` (`FM2K_COMMUNITY_HIDDEN_PATH`) when configured, else the latest row's flag.

## Ratings

Glicko-2 (Glickman 2013), per game id: tau 0.5, new players 1500 / RD 350 /
volatility 0.06. Each match is its own rating period, processed in
`finished_at` order (log order breaks ties). RD shrinks only through play;
there is no inflation for idle calendar time.

Leaderboard eligibility: RD <= 150 **or** at least 10 rated matches in that
game. Order: rating desc, then RD asc, then matches desc, then public id.

Ratings are cached by the log's and the results file's (path, mtime, size); a
request after a new commit recomputes once.

## Game ids and families

One log for every engine. `family` is derived from `game_id`:
`"fm2k:<id>"` -> `fm2k`, `"fm95:<id>"` -> `fm95`, `"mbaacc"` -> `mbaacc`.
Bare legacy ids (the hub's existing FM2K entries keep their exe-stem ids)
map to `fm2k`, or `fm95` when the game registry says the engine is FM95.
Game ids are used verbatim in URLs; registry aliases are not merged.

## Shapes

Timestamps are Unix seconds (float, UTC). Ratings and RD are rounded to
one decimal.

### Player

```json
{"public_id": "3f9c0a1b2c3d4e5f", "name": "alice", "tag": "3F9C"}
```

plus `"private": true` when opted out.

### Match

```json
{
  "id": "token",
  "game_id": "fm2k:ww",
  "family": "fm2k",
  "finished_at": 1760000600.0,
  "p1": {"public_id": "...", "name": "alice", "tag": "3F9C", "char": "ryu"},
  "p2": {"public_id": "...", "name": "bob", "tag": "A01B", "char": null},
  "winner": "p1",
  "score": [2, 1],
  "disconnect": false,
  "repaired": false,
  "stage": "dojo",
  "replay_url": null,
  "status": "final",
  "left_early": null
}
```

- `status`: `final` for every legacy row; `pending` / `final` / `left_early` for community results.
- `left_early`: community results only: `"p1"` or `"p2"` left before the deciding round (a loss for them).

- `winner`: `"p1"`, `"p2"` or `"draw"`.
- `score`: rounds won `[p1, p2]` from `rounds[]`, or `null` (schema 1).
- `char`: resolved character filename, `"id N"` fallback, or `null`.
- `replay_url`: the record's `replay_url` field when the log has one,
  else `null` (the current hub log does not write it yet).

### GET /api/v1/games

```json
{"games": [{"game_id": "fm2k:ww", "family": "fm2k", "name": "Wonderful World",
            "matches": 40, "players": 4, "last_played": 1760024000.0}]}
```

Sorted by `matches` desc. Counts only matches that count (see above).

### GET /api/v1/g/{game_id}/leaderboard?limit=50

`limit` 1..200, default 50.

```json
{
  "game_id": "fm2k:ww", "family": "fm2k", "name": "Wonderful World",
  "eligibility": {"max_rd": 150.0, "min_matches": 10},
  "players": [
    {"rank": 1, "public_id": "...", "name": "alice", "tag": "3F9C",
     "rating": 1712.4, "rd": 61.0, "wins": 16, "losses": 12, "draws": 1,
     "matches": 29, "last_played": 1760024000.0}
  ]
}
```

404 if the game has no counted matches.

### GET /api/v1/u/{public_id}

```json
{
  "public_id": "...", "name": "alice", "tag": "3F9C", "private": false,
  "totals": {"wins": 17, "losses": 12, "draws": 1, "matches": 30},
  "games": [
    {"game_id": "fm2k:ww", "family": "fm2k", "rating": 1712.4, "rd": 61.0,
     "provisional": false, "rank": 1, "wins": 16, "losses": 12, "draws": 1,
     "matches": 29, "last_played": 1760024000.0,
     "top_characters": [{"char": "ryu", "matches": 16}]}
  ],
  "recent": [Match, "... up to 20, newest first"]
}
```

`rank` is `null` while `provisional` (not leaderboard-eligible).
`top_characters` holds up to 5. Opted out: the private stub (see above).
404 for an unknown public id.

### GET /api/v1/recent?game=&limit=50

`game` optional (exact game id), `limit` 1..200, default 50.

```json
{"matches": [Match, "... newest first"]}
```

### GET /api/v1/matches?user_id=&game_id=&limit=50 (legacy)

Kept for existing consumers, same field names as before:
`id, game_id, p1_id, p1_nick, p2_id, p2_nick, p1_char_name, p2_char_name,
stage_name, winner_id, finished_at`, newest first, `limit` up to 500.
Changes from the pre-v1 feed: `p1_id`, `p2_id`, `winner_id` and the
`user_id` filter are now **public ids** (`winner_id ""` = draw); only
counted matches appear; `p1_char_name` falls back to `"id N"`; the raw `*_char_id`, `stage_id`,
`started_at` and schema-2 fields are no longer included.
