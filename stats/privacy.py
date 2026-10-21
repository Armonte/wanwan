"""Public identity + profile opt-out for the stats JSON API.

Two rules the API never breaks:
  1. No Discord id and no IP address ever leaves the service. A player is
     addressed by an opaque `public_id`: the first 16 hex chars of
     HMAC-SHA256(secret, discord_id). The secret comes from
     FM2K_STATS_ID_SECRET, else the file named by FM2K_STATS_ID_SECRET_FILE
     (default stats/id_secret.txt, gitignored), else a fixed dev fallback
     that is logged as a warning. Changing the secret changes every
     public id, so production must pin one.
  2. Opted-out players are hidden. The opt-out list is plain data the
     service (or a future launcher setting) writes: a JSON file
     {"hidden": ["<discord id>", ...]} at FM2K_STATS_OPTOUT_PATH (default
     stats/optout.json). `set_hidden()` edits it atomically. There is no
     auth flow yet, so "visible to themselves" is modelled as a
     `viewer_dc_id` argument the endpoints pass as None for now.
"""

from __future__ import annotations

import hashlib
import hmac
import json
import logging
import os
from pathlib import Path
from typing import Iterable, Optional

log = logging.getLogger("fm2k-stats.privacy")

_HERE = Path(__file__).resolve().parent
DEV_SECRET = b"fm2k-stats-dev-secret-not-for-production"


def _load_secret() -> bytes:
    env = os.environ.get("FM2K_STATS_ID_SECRET", "")
    if env:
        return env.encode("utf-8")
    path = Path(os.environ.get("FM2K_STATS_ID_SECRET_FILE",
                               str(_HERE / "id_secret.txt")))
    try:
        data = path.read_bytes().strip()
        if data:
            return data
    except OSError:
        pass
    log.warning("FM2K_STATS_ID_SECRET unset and no secret file: using the "
                "dev fallback; public ids are NOT stable across deploys")
    return DEV_SECRET


_SECRET: bytes = _load_secret()


def set_secret(secret: bytes) -> None:
    """Tests (and a future key rotation tool) replace the secret here."""
    global _SECRET
    _SECRET = secret
    _pid_cache.clear()


_pid_cache: dict[str, str] = {}


def public_id(discord_id: str) -> str:
    pid = _pid_cache.get(discord_id)
    if pid is None:
        mac = hmac.new(_SECRET, discord_id.encode("utf-8"), hashlib.sha256)
        pid = mac.hexdigest()[:16]
        _pid_cache[discord_id] = pid
    return pid


def tag_for(pid: str) -> str:
    """Display tag shown as name#TAG. Discord retired discriminators, so the
    tag is derived from the public id: 4 uppercase hex chars, stable, and
    enough to tell two players with the same nick apart."""
    return pid[:4].upper()


# --- opt-out list ---------------------------------------------------------

def optout_path() -> Path:
    return Path(os.environ.get("FM2K_STATS_OPTOUT_PATH",
                               str(_HERE / "optout.json")))


_optout_cache: dict[str, object] = {"key": None, "ids": frozenset()}


def hidden_ids() -> frozenset[str]:
    """Discord ids that opted out. Cached by the file's mtime + size."""
    path = optout_path()
    try:
        st = path.stat()
    except OSError:
        return frozenset()
    key = (str(path), st.st_mtime_ns, st.st_size)
    if _optout_cache["key"] == key:
        return _optout_cache["ids"]  # type: ignore[return-value]
    try:
        with path.open("r", encoding="utf-8") as f:
            payload = json.load(f)
        raw = payload.get("hidden", []) if isinstance(payload, dict) else []
        ids = frozenset(str(x) for x in raw if x)
    except (OSError, json.JSONDecodeError):
        return _optout_cache["ids"]  # type: ignore[return-value]
    _optout_cache["key"] = key
    _optout_cache["ids"] = ids
    return ids


_community_cache: dict[str, object] = {"key": None, "ids": frozenset()}


def community_hidden_pids() -> frozenset[str]:
    """Public ids hidden through the launcher (the community service's export/hidden.json, at
    FM2K_COMMUNITY_HIDDEN_PATH). Cached by the file's mtime + size; a missing file hides no one."""
    p = os.environ.get("FM2K_COMMUNITY_HIDDEN_PATH", "")
    if not p:
        return frozenset()
    path = Path(p)
    try:
        st = path.stat()
    except OSError:
        return frozenset()
    key = (str(path), st.st_mtime_ns, st.st_size)
    if _community_cache["key"] == key:
        return _community_cache["ids"]  # type: ignore[return-value]
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
        ids = frozenset(str(x) for x in (payload.get("hidden") or []) if x)
    except (OSError, ValueError, AttributeError):
        return _community_cache["ids"]  # type: ignore[return-value]
    _community_cache["key"], _community_cache["ids"] = key, ids
    return ids


def is_hidden(discord_id: str, viewer_dc_id: Optional[str] = None) -> bool:
    if viewer_dc_id and viewer_dc_id == discord_id:
        return False
    return discord_id in hidden_ids() or public_id(discord_id) in community_hidden_pids()


def set_hidden(discord_id: str, hidden: bool) -> None:
    """Add or remove a player from the opt-out list (atomic rewrite)."""
    ids = set(hidden_ids())
    if hidden:
        ids.add(discord_id)
    else:
        ids.discard(discord_id)
    _write_optout(sorted(ids))


def _write_optout(ids: Iterable[str]) -> None:
    path = optout_path()
    tmp = path.with_suffix(path.suffix + ".tmp")
    with tmp.open("w", encoding="utf-8") as f:
        json.dump({"version": 1, "hidden": list(ids)}, f, indent=2)
    os.replace(tmp, path)
