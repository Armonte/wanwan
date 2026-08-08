#!/usr/bin/env python3
"""fake_spectator.py -- a protocol-level FM2K spectator that loads the HOST's
spectator fan-out path WITHOUT running a game.

Why: a REAL spectator is a full FM2K + hook process; running N of them on the
test box burns N cores and lags the machine ITSELF, which confounds whether the
*host* is hiccuping from fan-out work vs just local CPU starvation. This client
speaks only the wire protocol -- it costs ~nothing -- so we can put 3..7 (or
more) subscribers + join/leave churn on the host and measure the host's per-frame
time honestly.

It replicates exactly what a real spectator does on the host side:
  UDP 0xCC control plane: SPEC_JOIN_REQ -> SPEC_JOIN_ACK (learn host_tcp_port),
                          SPEC_HEARTBEAT (1Hz keepalive), SPEC_LEAVE.
  TCP 0xCE bulk plane:    connect host_tcp_port, drain+discard the stream so the
                          host's per-subscriber send() keeps flowing (= the load).

Wire layout (verified vs FM2KHook/src/netplay/netplay_state.h, #pragma pack(1)):
  CtrlPacket = 7-byte header (magic:u8(0xCC) seq:u16 ack:u16 type:u8 player_id:u8)
  + the union. The union grew to 28 B when host_config gained session_id (2026-08),
  so a real packet is 35 bytes; the 31 we send is still accepted, because the hook's
  receive only requires >= the 7-byte header and zero-fills the tail of its receive
  buffer -- fields past our payload read as their "not announced" sentinel.
  CtrlMsg ordinals: SPEC_JOIN_REQ=17 SPEC_JOIN_ACK=18 SPEC_HEARTBEAT=20 SPEC_LEAVE=21.
  spec_join_req: mode:u8(off7)=0(FULL_SESSION) reserved[7].
  spec_join_ack: host_session_kind:u8(off7) host_tcp_port:u16(off8, LE) ...

Usage:
  fake_spectator.py --host-udp 127.0.0.1:7000 --local-udp-port 7200 \
                    --schedule join@0,leave@15,join@20 --duration 30 --label spec0
"""
import argparse
import socket
import struct
import sys
import threading
import time

CTRL_MAGIC = 0xCC
# CtrlMsg ordinals -- positional from netplay_state.h:55 (enum class : uint8_t).
SPEC_JOIN_REQ = 17
SPEC_JOIN_ACK = 18
SPEC_HEARTBEAT = 20
SPEC_LEAVE = 21

CTRL_SIZE = 31  # 7-byte header + 24 B of union -- a short-but-valid control packet


def _ctrl_packet(msg_type, payload=b""):
    # header: magic u8, seq u16, ack u16, type u8, player_id u8 = 7 bytes
    hdr = struct.pack("<BHHBB", CTRL_MAGIC, 0, 0, msg_type, 0)
    body = payload[:24].ljust(24, b"\x00")
    return hdr + body  # 31 bytes


def _join_req(mode=0):
    # data.spec_join_req: mode u8 (0 = FULL_SESSION -- no snapshot ceremony),
    # reserved[7] = 0 (no UDP_OK cap: we only want the authoritative TCP stream).
    return _ctrl_packet(SPEC_JOIN_REQ, struct.pack("<B", mode))


def _now():
    return time.monotonic()


class FakeSpectator:
    def __init__(self, host_ip, host_udp_port, local_udp_port, label, join_mode=0):
        self.host_ip = host_ip
        self.host_udp_port = host_udp_port
        self.local_udp_port = local_udp_port
        self.label = label
        self.join_mode = join_mode   # 0=FULL_SESSION, 1=CURRENT_MATCH (snapshot)
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.udp.bind(("0.0.0.0", local_udp_port))
        self.tcp = None
        self.subscribed = False
        self.host_gone = False               # set when the host closes the stream
        self._stop = threading.Event()       # set on leave -> stops hb/drain threads
        self._hb_thread = None
        self._tcp_thread = None

    def log(self, msg):
        print(f"[fake {self.label}] {msg}", flush=True)

    # ---- subscribe: JOIN_REQ -> ACK -> TCP connect -> start hb + drain ----
    def join(self):
        if self.subscribed:
            return True
        self._stop.clear()
        host_tcp_port = self._handshake()
        if not host_tcp_port:
            self.log("JOIN failed (no ACK / no tcp port)")
            return False
        try:
            self.tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.tcp.settimeout(3.0)
            self.tcp.connect((self.host_ip, host_tcp_port))
            self.tcp.settimeout(0.25)
        except OSError as e:
            self.log(f"TCP connect to {host_tcp_port} failed: {e}")
            self.tcp = None
            return False
        self.subscribed = True
        self.log(f"joined (host_tcp_port={host_tcp_port})")
        self._hb_thread = threading.Thread(target=self._heartbeat_loop, daemon=True)
        self._tcp_thread = threading.Thread(target=self._drain_loop, daemon=True)
        self._hb_thread.start()
        self._tcp_thread.start()
        return True

    def _handshake(self):
        # Send JOIN_REQ and wait (up to ~2s, retrying) for a SPEC_JOIN_ACK.
        self.udp.settimeout(0.4)
        deadline = _now() + 2.0
        attempt = 0
        while _now() < deadline:
            if attempt % 3 == 0:  # (re)send roughly every ~1.2s of waiting
                self.udp.sendto(_join_req(self.join_mode),
                                (self.host_ip, self.host_udp_port))
            attempt += 1
            try:
                data, _ = self.udp.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            if len(data) >= 10 and data[0] == CTRL_MAGIC and data[5] == SPEC_JOIN_ACK:
                # spec_join_ack: host_session_kind u8 @7, host_tcp_port u16 LE @8
                host_tcp_port = struct.unpack_from("<H", data, 8)[0]
                return host_tcp_port
        return 0

    def _heartbeat_loop(self):
        hb = _ctrl_packet(SPEC_HEARTBEAT)
        while not self._stop.is_set():
            try:
                self.udp.sendto(hb, (self.host_ip, self.host_udp_port))
            except OSError:
                pass
            # Drain any host heartbeat echoes so the UDP recv buffer stays clean.
            self.udp.settimeout(0.0)
            for _ in range(64):
                try:
                    self.udp.recvfrom(2048)
                except (socket.timeout, BlockingIOError, OSError):
                    break
            self._stop.wait(1.0)

    def _drain_loop(self):
        # Read-and-discard at any granularity -- keeps the host's per-subscriber
        # NET_WriteToStreamSocket unblocked, which is the fan-out cost we load.
        while not self._stop.is_set():
            if self.tcp is None:
                break
            try:
                data = self.tcp.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:  # host closed the stream
                # If WE didn't initiate this (no leave pending), the host
                # terminated -> signal the run loop to exit promptly.
                if not self._stop.is_set():
                    self.host_gone = True
                break

    # ---- unsubscribe: LEAVE + close TCP + stop threads ----
    def leave(self):
        if not self.subscribed:
            return
        self._stop.set()
        try:
            self.udp.sendto(_ctrl_packet(SPEC_LEAVE), (self.host_ip, self.host_udp_port))
        except OSError:
            pass
        if self.tcp is not None:
            try:
                self.tcp.close()
            except OSError:
                pass
            self.tcp = None
        self.subscribed = False
        self.log("left")

    def close(self):
        self.leave()
        try:
            self.udp.close()
        except OSError:
            pass

    # ---- run a scheduled join/leave timeline, then exit ----
    def run(self, schedule, duration):
        # schedule: list of (t_seconds, action) where action in {"join","leave"}.
        events = sorted(schedule, key=lambda e: e[0])
        start = _now()
        i = 0
        while True:
            elapsed = _now() - start
            if elapsed >= duration:
                break
            if self.host_gone:   # host terminated -> stop, don't reconnect-storm
                self.log("host closed stream -- exiting")
                break
            if i < len(events) and elapsed >= events[i][0]:
                _, action = events[i]
                i += 1
                if action == "join":
                    self.join()
                elif action == "leave":
                    self.leave()
                continue
            time.sleep(0.05)
        self.close()


def _parse_schedule(s):
    # "join@0,leave@15,join@20" -> [(0.0,"join"),(15.0,"leave"),(20.0,"join")]
    out = []
    for tok in s.split(","):
        tok = tok.strip()
        if not tok:
            continue
        action, _, t = tok.partition("@")
        action = action.strip().lower()
        if action not in ("join", "leave"):
            raise ValueError(f"bad schedule action: {action!r}")
        out.append((float(t), action))
    return out


def main():
    ap = argparse.ArgumentParser(description="protocol-level FM2K fake spectator (host load generator)")
    ap.add_argument("--host-udp", default="127.0.0.1:7000",
                    help="host UDP gameplay/control addr (where JOIN_REQ + heartbeats go)")
    ap.add_argument("--local-udp-port", type=int, required=True,
                    help="local UDP port to bind (must be unique per fake spectator)")
    ap.add_argument("--schedule", default="join@0",
                    help="comma list of action@seconds, e.g. join@0,leave@15,join@20")
    ap.add_argument("--duration", type=float, default=30.0,
                    help="total run seconds before clean exit")
    ap.add_argument("--join-mode", default="current", choices=["current", "full"],
                    help="current = CURRENT_MATCH (triggers the host's ~1MB snapshot "
                         "send = the expensive path); full = FULL_SESSION (event history only)")
    ap.add_argument("--label", default="spec")
    args = ap.parse_args()

    host_ip, _, host_port = args.host_udp.partition(":")
    mode = 1 if args.join_mode == "current" else 0
    fs = FakeSpectator(host_ip, int(host_port), args.local_udp_port, args.label, mode)
    try:
        fs.run(_parse_schedule(args.schedule), args.duration)
    except KeyboardInterrupt:
        fs.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
