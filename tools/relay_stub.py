#!/usr/bin/env python3
"""Minimal standalone UDP relay -- hub data-plane stub for the selftest.

This reproduces EXACTLY the hub's UDP relay semantics (hub/hub.py
1826-1878) plus the UDP STUN ack (hub/hub.py 1759-1777), with none of the
WebSocket matchmaking / Discord-auth / HTTP surface. It exists so
tools/replay_netplay_selftest.py --relay can exercise the 0xCF relay data
plane locally and hermetically, without standing up the full hub.

Wire behavior (must stay byte-identical to the hub so a peer cannot tell
the difference):

  0xCF relay envelope (peer -> stub or stub -> peer):
    0xCF 0x01 [16-byte session_id] [original UDP payload bytes...]
  Routing: a session has two slots. The first unique source endpoint fills
  slot A, the second fills slot B; each inbound packet is forwarded as-is
  to the OTHER slot. Third+ unique sources for the same session are dropped
  (mirrors RelaySession.route()).

  0xCD UDP STUN (peer -> stub):
    0xCD 0x01 [user_id bytes...]   ->   0xCD 0x02 [4-byte ip_be][2-byte port_be]
  We reflect the source endpoint back. We do NOT need to read the user_id
  (observing the datagram source is enough).

NOTE: the REAL relay path is hub/hub.py run with --test-relay-session. This
stub is the dependency-free fallback the selftest uses by default. Where
the `websockets` package is installed, re-verify the same scenario against
the real hub.py at least once so the two data planes stay in agreement.
"""

from __future__ import annotations
import argparse
import socket
import struct
import sys
from typing import Optional

# Wire constants -- copied verbatim from hub/hub.py so the stub and the
# real hub stay in lockstep. If hub.py's protocol bytes ever change, change
# them here too (and update the selftest).
RELAY_MAGIC          = 0xCF
RELAY_TAG_DATA       = 0x01
RELAY_SESSION_ID_LEN = 16

STUN_MAGIC     = 0xCD
STUN_PROBE_TAG = 0x01
STUN_ACK_TAG   = 0x02


def _assert_in_sync_with_hub() -> None:
    """Fail loudly if these wire constants have drifted from hub/hub.py.

    The stub reproduces the hub's data plane byte-for-byte; if a protocol byte
    changes in hub.py but not here (or vice-versa), the selftest would pass
    against a relay the real hub no longer matches. Rather than a stale "keep
    in sync" comment, actually verify it at startup. Best-effort: if hub.py
    isn't co-located (stub copied elsewhere), warn and continue.
    """
    import os
    import re
    hub_path = os.path.join(os.path.dirname(__file__), "..", "hub", "hub.py")
    try:
        with open(hub_path, "r", encoding="utf-8", errors="ignore") as fh:
            src = fh.read()
    except OSError:
        print(f"[relay_stub] WARN: hub.py not found at {hub_path} — "
              f"skipping wire-constant sync check", file=sys.stderr)
        return
    expected = {
        "RELAY_MAGIC":          RELAY_MAGIC,
        "RELAY_TAG_DATA":       RELAY_TAG_DATA,
        "RELAY_SESSION_ID_LEN": RELAY_SESSION_ID_LEN,
        "STUN_MAGIC":           STUN_MAGIC,
        "STUN_PROBE_TAG":       STUN_PROBE_TAG,
    }
    drift = []
    for name, ours in expected.items():
        m = re.search(rf"^{name}\s*=\s*(0x[0-9A-Fa-f]+|\d+)", src, re.MULTILINE)
        if not m:
            continue  # hub may name it differently; don't false-alarm
        theirs = int(m.group(1), 0)
        if theirs != ours:
            drift.append(f"{name}: stub={ours:#x} hub={theirs:#x}")
    if drift:
        raise SystemExit(
            "[relay_stub] FATAL: wire constants have DRIFTED from hub/hub.py:\n  "
            + "\n  ".join(drift)
            + "\nThe stub no longer reproduces the real hub — fix before testing.")


_assert_in_sync_with_hub()


class RelaySession:
    """Two-slot dynamic peer learning -- identical policy to hub.py's
    RelaySession.route(): first source fills A, second fills B, each
    packet routes to the other slot, third+ unique sources dropped."""

    def __init__(self) -> None:
        self.slot_a: Optional[tuple[str, int]] = None
        self.slot_b: Optional[tuple[str, int]] = None

    def route(self, src: tuple[str, int]) -> Optional[tuple[str, int]]:
        if self.slot_a == src:
            return self.slot_b
        if self.slot_b == src:
            return self.slot_a
        if self.slot_a is None:
            self.slot_a = src
            return self.slot_b
        if self.slot_b is None:
            self.slot_b = src
            return self.slot_a
        return None


def _session_id_from_hex(token_hex: str) -> bytes:
    """Mirror hub.py make_relay_session()'s key derivation exactly: hex ->
    bytes, truncate/pad to 16."""
    sid = bytes.fromhex(token_hex)[:RELAY_SESSION_ID_LEN]
    if len(sid) < RELAY_SESSION_ID_LEN:
        sid = sid.ljust(RELAY_SESSION_ID_LEN, b"\x00")
    return sid


class Logger:
    """Writes lines to stdout AND, optionally, a self-managed file. When the
    stub runs as a Windows process launched from WSL, a WSL-side stdout pipe
    does not reliably capture its output, so the harness passes --logfile and
    the stub owns the file write itself (Windows process -> Windows file)."""

    def __init__(self, logfile: Optional[str]) -> None:
        self.fh = None
        if logfile:
            try:
                self.fh = open(logfile, "w", buffering=1)  # line-buffered
            except OSError:
                self.fh = None

    def __call__(self, line: str) -> None:
        print(line, flush=True)
        if self.fh is not None:
            try:
                self.fh.write(line + "\n")
                self.fh.flush()
            except (OSError, ValueError):
                pass


def run(host: str, port: int, session_hexes: list[str], verbose: bool,
        log: Logger) -> int:
    sessions: dict[bytes, RelaySession] = {}
    for h in session_hexes:
        try:
            sid = _session_id_from_hex(h)
        except ValueError:
            log(f"[relay-stub] bad session hex {h!r}; ignored")
            continue
        sessions[sid] = RelaySession()
        log(f"[relay-stub] pre-registered session token={h} sid={sid.hex()}")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    except OSError:
        pass
    sock.bind((host, port))
    log(f"[relay-stub] listening on udp://{host}:{port} "
        f"({len(sessions)} session(s) pre-registered)")
    # Tell the harness/operator we are ready in a single stable line it can
    # grep for if it ever wants to gate launch on readiness.
    log("[relay-stub] READY")

    while True:
        try:
            data, addr = sock.recvfrom(2048)
        except OSError:
            continue
        if len(data) < 2:
            continue

        # ---- 0xCF relay data plane (the hot path) ----
        if data[0] == RELAY_MAGIC and data[1] == RELAY_TAG_DATA:
            if len(data) < 2 + RELAY_SESSION_ID_LEN:
                continue
            session_id = data[2:2 + RELAY_SESSION_ID_LEN]
            sess = sessions.get(session_id)
            if sess is None:
                if verbose:
                    log(f"[relay-stub] drop: unknown session "
                        f"{session_id.hex()} from {addr}")
                continue
            dst = sess.route(addr)
            if dst is None:
                if verbose:
                    log(f"[relay-stub] hold: no peer yet for "
                        f"{session_id.hex()} (src {addr})")
                continue
            # Forward the WHOLE envelope as-is; the peer strips it.
            sock.sendto(data, dst)
            if verbose:
                log(f"[relay-stub] route {addr} -> {dst} "
                    f"({len(data)}B, sid {session_id.hex()[:8]})")
            continue

        # ---- 0xCD UDP STUN ack ----
        if data[0] == STUN_MAGIC and data[1] == STUN_PROBE_TAG:
            try:
                ip_bytes   = socket.inet_aton(addr[0])
                port_bytes = struct.pack("!H", addr[1])
                ack = bytes([STUN_MAGIC, STUN_ACK_TAG]) + ip_bytes + port_bytes
                sock.sendto(ack, addr)
                if verbose:
                    log(f"[relay-stub] STUN ack -> {addr}")
            except OSError:
                pass
            continue

        # Anything else is not ours -- drop silently (matches hub guards).


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Minimal hub-data-plane relay stub (0xCF + 0xCD STUN).")
    ap.add_argument("--host", default="127.0.0.1",
                    help="bind address (default 127.0.0.1)")
    ap.add_argument("--port", type=int, default=7712,
                    help="UDP port (default 7712, matches hub relay)")
    ap.add_argument("--session", action="append", default=[], metavar="HEX",
                    help="pre-register a relay session id (32-hex match "
                         "token). Repeatable.")
    ap.add_argument("--verbose", action="store_true",
                    help="log every routed/dropped datagram")
    ap.add_argument("--logfile", default=None, metavar="PATH",
                    help="also write log lines to this file (used by the "
                         "selftest harness when the stub runs as a Windows "
                         "process and its stdout pipe is not captured).")
    args = ap.parse_args()
    log = Logger(args.logfile)
    try:
        return run(args.host, args.port, args.session, args.verbose, log) or 0
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
