#!/usr/bin/env python3
"""One-way UDP path simulator -- reproduces the ASYMMETRIC-NAT deadlock.

The field bug (2026-08-23, two players who could not connect at all):
one peer's datagrams arrive, the other's never do. That is not packet
loss and it is not "no connectivity" -- it is a path that delivers in
exactly ONE direction, which is what a per-destination source-port remap
or a one-sided firewall drop produces. Both clients then behave
plausibly and still deadlock:

  P2 -> P1 delivers   ==>  P1 authenticates P2's CTRL_PUNCH, concludes the
                           direct path works, and commits to it
  P1 -> P2 vanishes   ==>  P2's punch gate expires, P2 moves to the relay

P1 answers into a dead mapping forever while P2 waits on the relay. The
match never starts and neither side logs an error.

WHY A PROXY AND NOT JUST A BLACKHOLE ADDRESS: pointing P1's remote at an
unroutable IP does not reproduce this. The moment P2's punch lands,
ControlChannel_LatchPeerAddr() overwrites that address with the packet's
real source and P1 starts sending somewhere that works. To hold the path
asymmetric, P2's traffic must ARRIVE FROM an endpoint that is itself a
black hole. That is this proxy:

    P2 --> proxy:PORT --> P1        (forwarded; P1 sees src = proxy)
    P1 --> proxy:PORT --> /dev/null (dropped, silently, forever)

P1 latches the proxy, which is exactly the dead mapping the field logs
show. Point BOTH peers' remote at this proxy.

Pair with tools/relay_stub.py to give the run somewhere to recover TO --
without a relay the deadlock is unrecoverable by design.

Runs as a WINDOWS process for the same reason relay_stub.py does: under
WSL2 NAT networking a WSL-bound socket is not reachable from the Windows
game at 127.0.0.1.
"""

from __future__ import annotations
import argparse
import socket
import sys
import time
from typing import Optional


def run(host: str, port: int, forward_to: tuple[str, int],
        logfile: Optional[str], report_every: float) -> int:
    fh = None
    if logfile:
        try:
            fh = open(logfile, "w", buffering=1)
        except OSError:
            fh = None

    def log(line: str) -> None:
        print(line, flush=True)
        if fh is not None:
            try:
                fh.write(line + "\n"); fh.flush()
            except (OSError, ValueError):
                pass

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    except OSError:
        pass
    sock.bind((host, port))
    sock.settimeout(0.5)
    log(f"[oneway] listening on udp://{host}:{port}")
    log(f"[oneway] FORWARD  *          -> {forward_to[0]}:{forward_to[1]}")
    log(f"[oneway] DROP     {forward_to[0]}:{forward_to[1]} -> * "
        f"(this is the dead leg under test)")
    log("[oneway] READY")

    fwd = 0
    dropped = 0
    last_report = time.monotonic()
    while True:
        try:
            data, addr = sock.recvfrom(4096)
        except socket.timeout:
            data = None
        except OSError:
            continue

        if data is not None:
            # Traffic FROM the protected peer is the leg we are killing.
            # Count it so the harness can prove the drop actually happened
            # rather than the peer simply never sending.
            if addr == forward_to:
                dropped += 1
            else:
                # Forwarded from OUR socket, so the receiver sees this
                # proxy as the source and latches it. That is the point.
                try:
                    sock.sendto(data, forward_to)
                    fwd += 1
                except OSError:
                    pass

        now = time.monotonic()
        if report_every > 0 and now - last_report >= report_every:
            last_report = now
            log(f"[oneway] forwarded={fwd} dropped={dropped}")

    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True,
                    help="port BOTH peers point their remote at")
    ap.add_argument("--forward-to", required=True, metavar="HOST:PORT",
                    help="the peer that RECEIVES (its sends are dropped)")
    ap.add_argument("--logfile", default=None)
    ap.add_argument("--report-every", type=float, default=2.0)
    a = ap.parse_args()
    h, _, p = a.forward_to.rpartition(":")
    try:
        dest = (h, int(p))
    except ValueError:
        print(f"[oneway] bad --forward-to {a.forward_to!r}", file=sys.stderr)
        return 2
    try:
        return run(a.host, a.port, dest, a.logfile, a.report_every)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
