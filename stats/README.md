# fm2k-stats

Public read-only stats site over the FM2K Rollback hub's match log.

Designed alongside the hub but runs as its own process so a site crash
or HTTP DDoS never affects realtime matchmaking. See
[../docs/dev/stats_site_design.md](../docs/dev/stats_site_design.md)
for the full design rationale.

## Run locally

```bash
cd stats
python3 -m venv .venv
. .venv/bin/activate    # or .venv\Scripts\activate on Windows
pip install -r requirements.txt
uvicorn app:app --host 127.0.0.1 --port 8081
```

Then open `http://127.0.0.1:8081/`.

By default it reads `../hub/matches.json`. Override with the
`FM2K_MATCHES_PATH` env var:

```bash
FM2K_MATCHES_PATH=/srv/fm2k/matches.json uvicorn app:app
```

## Production deploy (sub-path under nginx)

The launcher and the design doc both assume the site lives at
`https://<host>/stats`. Run uvicorn on a non-public port and front it
with nginx:

```nginx
location /stats/ {
    proxy_pass        http://127.0.0.1:8081/;
    proxy_set_header  Host $host;
    proxy_set_header  X-Forwarded-For  $proxy_add_x_forwarded_for;
    proxy_set_header  X-Forwarded-Proto $scheme;
}
```

Tell uvicorn what its mount-point looks like via `FM2K_STATS_ROOT_PATH`
so absolute URLs in templates render correctly:

```bash
FM2K_STATS_ROOT_PATH=/stats \
FM2K_MATCHES_PATH=/srv/fm2k/matches.json \
uvicorn app:app --host 127.0.0.1 --port 8081
```

Run as an unprivileged user with **read-only** access to the hub's
data directory. The site never writes to disk; if your filesystem
permissions enforce that, accidental drift can't escalate.

## Routes

- `GET /` — overview: totals, top players, top games.
- `GET /u/{user_id}` — player profile (W/L/D, by-game, by-character,
  top opponents, recent matches).
- `GET /g/{game_id}` — per-game leaderboard + most-played characters.
- `GET /g/{game_id}/c/{char}` — per-character page; `char` matches
  either the resolved `.player` filename or the `id N` fallback used
  when the hook couldn't resolve a name.
- `GET /recent` — last 100 matches across everyone.
- `GET /api/v1/games`, `/api/v1/g/{game_id}/leaderboard`,
  `/api/v1/u/{public_id}`, `/api/v1/recent`, `/api/v1/matches` -- JSON
  API for the launcher (per-game Glicko-2 ratings, profiles, recent
  matches). Shapes, privacy rules (opaque public ids, opt-out list) and
  what counts are in [API.md](API.md). Per-IP rate-limited (60 req/min).

Production must set `FM2K_STATS_ID_SECRET` (or `stats/id_secret.txt`) so
public player ids stay stable across deploys.

## Tests

```bash
python3 -m venv /path/outside/drvfs/venv   # a venv on /mnt/* is very slow
/path/outside/drvfs/venv/bin/pip install -r requirements.txt pytest httpx
/path/outside/drvfs/venv/bin/python -m pytest tests -q
```

## Security

Read the security checklist in `../docs/dev/stats_site_design.md`
before launching this publicly. Highlights:

- All HTML rendering goes through Jinja2 autoescape; no `safe` filter
  is used anywhere in the templates.
- Discord IDs (`p1_dc_id` / `p2_dc_id`) live in the match log on disk
  but are stripped at load time; the site never sees them.
- Strict `Content-Security-Policy` blocks inline scripts.
- `slowapi` rate-limits the JSON API per IP.

## Why no DB

`matches.json` is the source of truth on the hub side and at v1 scale
(thousands of rows) the file fits comfortably in RAM. We re-parse it
on every request rather than maintain a cache that would need
invalidation when the hub commits a new match. If the log grows past
~500k rows we'll switch to SQLite with a one-time importer; SQLite
handles that load trivially. Until then, simpler is better.
