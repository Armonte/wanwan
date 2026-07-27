#!/usr/bin/env python3
"""Pull the 2dfm-launcher-bugs-feedback Discord forum for triage.

Reads the hub bot token from hub/.env (gitignored) and uses the plain Discord
REST API -- no gateway, no discord.py. Collects active + archived threads under
the forum channel, then every message in each, and writes:

    logs/bugs_forum/bugs_forum.json     full dump
    logs/bugs_forum/digest.md           human-readable, newest activity first

The token is never printed or written to either output.

Usage:  python3 tools/pull_bugs_forum.py [--channel <forum_id>] [--quiet]
"""
import json, os, sys, time, urllib.request, urllib.error
from pathlib import Path
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parent.parent
ENV  = ROOT / "hub" / ".env"
OUT  = ROOT / "logs" / "bugs_forum"
FORUM_DEFAULT = "1500616860645720115"   # 2dfm-launcher-bugs-feedback
API = "https://discord.com/api/v10"

def load_env():
    if not ENV.exists():
        sys.exit(f"missing {ENV} (need DISCORD_BOT_TOKEN + DISCORD_GUILD_ID)")
    kv = {}
    for line in ENV.read_text(errors="replace").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        kv[k.strip()] = v.strip().strip('"').strip("'")
    tok, gid = kv.get("DISCORD_BOT_TOKEN"), kv.get("DISCORD_GUILD_ID")
    if not tok or not gid:
        sys.exit("hub/.env is missing DISCORD_BOT_TOKEN or DISCORD_GUILD_ID")
    return tok, gid

def get(tok, path, tries=6):
    """GET with 429 retry_after handling. Never logs the token."""
    url = API + path
    for attempt in range(tries):
        req = urllib.request.Request(url, headers={
            "Authorization": f"Bot {tok}",
            "User-Agent": "DiscordBot (fm2k-hub, 1.0)",
        })
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return json.loads(r.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            if e.code == 429:
                try:
                    wait = float(json.loads(e.read().decode()).get("retry_after", 2))
                except Exception:
                    wait = 2.0
                time.sleep(min(wait + 0.4, 30))
                continue
            if e.code in (500, 502, 503, 504):
                time.sleep(1.5 * (attempt + 1)); continue
            # Surface the endpoint, never the token.
            sys.exit(f"HTTP {e.code} on {path}")
        except Exception as ex:
            if attempt == tries - 1:
                sys.exit(f"request failed on {path}: {type(ex).__name__}")
            time.sleep(1.5 * (attempt + 1))
    sys.exit(f"gave up on {path}")

def main():
    args = sys.argv[1:]
    forum_id = FORUM_DEFAULT
    if "--channel" in args:
        forum_id = args[args.index("--channel") + 1]
    quiet = "--quiet" in args
    tok, gid = load_env()

    def say(*a):
        if not quiet: print(*a)

    # 1. active threads (guild-wide, filtered to our forum)
    active = get(tok, f"/guilds/{gid}/threads/active").get("threads", [])
    threads = {t["id"]: t for t in active if t.get("parent_id") == forum_id}
    say(f"[forum] active threads: {len(threads)}")

    # 2. archived public threads, paginated by archive_timestamp
    before, page = None, 0
    while True:
        q = f"/channels/{forum_id}/threads/archived/public?limit=100"
        if before: q += f"&before={before}"
        data = get(tok, q)
        batch = data.get("threads", [])
        for t in batch:
            threads.setdefault(t["id"], t)
        page += 1
        say(f"[forum] archived page {page}: +{len(batch)} (total {len(threads)})")
        if not data.get("has_more") or not batch:
            break
        before = batch[-1].get("thread_metadata", {}).get("archive_timestamp")
        if not before:
            break
        time.sleep(0.3)

    # 3. messages per thread (oldest-first once reversed)
    out = []
    for i, (tid, t) in enumerate(threads.items(), 1):
        msgs = get(tok, f"/channels/{tid}/messages?limit=100")
        msgs = list(reversed(msgs))
        out.append({
            "id": tid,
            "name": t.get("name", ""),
            "created": t.get("thread_metadata", {}).get("create_timestamp"),
            "archived": t.get("thread_metadata", {}).get("archived", False),
            "locked": t.get("thread_metadata", {}).get("locked", False),
            "last_activity": t.get("thread_metadata", {}).get("archive_timestamp"),
            "message_count": t.get("message_count", len(msgs)),
            "messages": [{
                "author": m.get("author", {}).get("username", "?"),
                "ts": m.get("timestamp"),
                "content": m.get("content", ""),
                "attachments": [a.get("filename") for a in m.get("attachments", [])],
            } for m in msgs],
        })
        say(f"[forum] {i}/{len(threads)} {t.get('name','')[:60]}")
        time.sleep(0.25)

    out.sort(key=lambda t: t.get("last_activity") or "", reverse=True)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "bugs_forum.json").write_text(json.dumps(out, indent=2, ensure_ascii=False))

    lines = [f"# 2dfm bugs forum -- {len(out)} threads",
             f"pulled {datetime.now(timezone.utc).isoformat()}", ""]
    for t in out:
        flag = []
        if t["archived"]: flag.append("archived")
        if t["locked"]:   flag.append("locked")
        lines.append(f"## {t['name']}  ({t['message_count']} msgs"
                     + (", " + "/".join(flag) if flag else "") + ")")
        lines.append(f"last activity: {t['last_activity']}  id={t['id']}")
        for m in t["messages"]:
            body = " ".join((m["content"] or "").split())
            if m["attachments"]:
                body += f"  [attachments: {', '.join(a for a in m['attachments'] if a)}]"
            lines.append(f"  - **{m['author']}** ({(m['ts'] or '')[:10]}): {body[:600]}")
        lines.append("")
    (OUT / "digest.md").write_text("\n".join(lines), encoding="utf-8")
    print(f"[forum] wrote {OUT/'bugs_forum.json'} and {OUT/'digest.md'} "
          f"({len(out)} threads)")

if __name__ == "__main__":
    main()
