#!/usr/bin/env python3
"""Pull recent messages + attachments from a Discord text channel for triage.

Same auth as tools/pull_bugs_forum.py (hub bot token from hub/.env, plain REST,
token never printed or written). Writes, per channel:

    logs/discord/<channel>/messages.json   raw messages (oldest first)
    logs/discord/<channel>/digest.md       readable transcript
    logs/discord/<channel>/att/<msgid>_<filename>   every attachment

Re-running skips attachments already on disk.

Usage:
  python3 tools/pull_discord_channel.py --channel 2dfm-testing [--since 2026-11-08] [--limit 1000]
  (--channel takes a name or an id)
"""
import argparse, json, re, sys, urllib.request
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pull_bugs_forum as forum  # load_env(), get()

ROOT = Path(__file__).resolve().parent.parent
OUT_ROOT = ROOT / "logs" / "discord"


def resolve_channel(tok, gid, name_or_id):
    if name_or_id.isdigit():
        c = forum.get(tok, f"/channels/{name_or_id}")
        return c["id"], c.get("name", name_or_id)
    for c in forum.get(tok, f"/guilds/{gid}/channels"):
        if c.get("name") == name_or_id:
            return c["id"], c["name"]
    sys.exit(f"no channel named {name_or_id}")


def fetch(tok, cid, since, limit):
    msgs, before = [], None
    while len(msgs) < limit:
        q = f"/channels/{cid}/messages?limit=100" + (f"&before={before}" if before else "")
        page = forum.get(tok, q)
        if not page:
            break
        stop = False
        for m in page:
            ts = datetime.fromisoformat(m["timestamp"])
            if since and ts < since:
                stop = True
                break
            msgs.append(m)
        if stop or len(page) < 100:
            break
        before = page[-1]["id"]
    return list(reversed(msgs))


def safe(name):
    return re.sub(r"[^A-Za-z0-9._-]", "_", name)[:120]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--channel", required=True)
    ap.add_argument("--since", help="YYYY-MM-DD (UTC)")
    ap.add_argument("--limit", type=int, default=1000)
    a = ap.parse_args()
    since = (datetime.fromisoformat(a.since).replace(tzinfo=timezone.utc)
             if a.since else None)
    tok, gid = forum.load_env()
    cid, cname = resolve_channel(tok, gid, a.channel)
    msgs = fetch(tok, cid, since, a.limit)
    out = OUT_ROOT / safe(cname)
    att = out / "att"
    att.mkdir(parents=True, exist_ok=True)
    (out / "messages.json").write_text(json.dumps(msgs, indent=1, ensure_ascii=False))
    lines = [f"# #{cname} -- {len(msgs)} messages", f"pulled {datetime.now(timezone.utc).isoformat()}", ""]
    got = 0
    for m in msgs:
        who = m["author"].get("global_name") or m["author"]["username"]
        lines.append(f"**{who}** ({m['timestamp'][:19]}) id={m['id']}")
        if m.get("content"):
            lines.append("  " + m["content"].replace("\n", "\n  "))
        for x in m.get("attachments", []):
            dst = att / f"{m['id']}_{safe(x['filename'])}"
            if not dst.exists():
                req = urllib.request.Request(x["url"], headers={"User-Agent": "fm2k-triage"})
                with urllib.request.urlopen(req, timeout=60) as r:
                    dst.write_bytes(r.read())
                got += 1
            lines.append(f"  [att] {dst.relative_to(ROOT)} ({x.get('size', 0)} B)")
        lines.append("")
    (out / "digest.md").write_text("\n".join(lines))
    print(f"[discord] #{cname}: {len(msgs)} msgs, {got} new attachments -> {out.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
