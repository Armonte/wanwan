#!/usr/bin/env python3
"""Emulated NAT box for the FM2K netplay/spectate harness (task #57).

Models the RFC 4787 behavior classes that matter to our transport stack,
as a single-destination UDP proxy (our peers/spectators each talk to
exactly one remote -- the host -- so endpoint-dependent mapping collapses
to filtering + lifetime semantics, which is what actually bites us):

  full_cone        endpoint-independent mapping, NO inbound filtering.
  restricted       inbound allowed only from IPs we have sent to.
  port_restricted  inbound allowed only from IP:port we have sent to
                   (typical home router; inbound-before-outbound vanishes).
  symmetric        port_restricted filtering + the mapping is REMADE
                   (fresh external port) whenever the inside peer goes
                   quiet past mapping_ttl -- per-destination state.

Orthogonal knobs, composable with every mode:
  mapping_ttl      seconds of INSIDE->OUT silence after which the mapping
                   dies (conservative carrier NAT: inbound traffic does
                   NOT refresh it). On death the external socket closes --
                   host-side sends to it vanish exactly like a real
                   expired mapping. The next outbound packet mints a
                   fresh external port.
  rebind_after     seconds after start when the mapping is force-remapped
                   once, mid-session (the ETECSA/Melancholy CGNAT event;
                   same behavior transition_churn_smoke.py proved for
                   netplay peers).

Topology (same WSL/Windows reality as the churn smoke): the proxy is a
LINUX process. Windows game -> WSL delivers UDP only via the WSL VM IP
(NEVER 127.0.0.1), and the proxy reaches Windows peers at the default-gw
address. Point the NAT'd instance's remote at WSL_IP:inside_port and the
proxy's dest at WIN_IP:host_port.

Importable (nat_matrix_smoke.py) or runnable standalone:
  python3 tools/nat_proxy.py <inside_port> <dest_ip:port> <mode> [ttl] [rebind_s]
"""
import socket
import sys
import threading
import time

MODES = ("full_cone", "restricted", "port_restricted", "symmetric")


class NatProxy:
    def __init__(self, inside_port, dest_addr, mode="port_restricted",
                 mapping_ttl=None, rebind_after=None, name="nat", quiet=False,
                 fixed_first_ext_port=0):
        if mode not in MODES:
            raise ValueError(f"mode must be one of {MODES}")
        # First mapping binds this external port if nonzero (so a peer with a
        # PRESET remote addr can reach us pre-punch, mirroring the churn rig);
        # every REMAP after that is ephemeral -- the realistic part.
        self.fixed_first = fixed_first_ext_port
        self.mode        = mode
        self.ttl         = float(mapping_ttl) if mapping_ttl else None
        self.rebind_at   = (time.monotonic() + float(rebind_after)) \
                           if rebind_after else None
        self.dest        = dest_addr            # (ip, port) the NAT'd peer talks to
        self.name        = name
        self.quiet       = quiet
        self.inside      = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.inside.bind(("0.0.0.0", inside_port))
        self.inside.settimeout(0.2)
        self.inside_peer = None                 # learned from first inside packet
        self.lock        = threading.Lock()
        self.out         = None                 # external-mapping socket (None = no mapping)
        self.last_outbound = 0.0                # monotonic ts of last inside->out packet
        self.allowed_eps = set()                # (ip, port) endpoints we've sent to
        self.allowed_ips = set()                # ips we've sent to
        # counters for the smoke's assertions
        self.stats = {"out": 0, "in_ok": 0, "in_filtered": 0,
                      "in_expired": 0, "remaps": 0}
        self.alive = True
        threading.Thread(target=self._pump_inside,  daemon=True).start()
        threading.Thread(target=self._pump_outside, daemon=True).start()

    # -- mapping lifecycle ---------------------------------------------------
    def _log(self, msg):
        if not self.quiet:
            print(f"[{self.name}] {msg}", flush=True)

    def _fresh_mapping_locked(self, why):
        if self.out is not None:
            try: self.out.close()
            except OSError: pass
        first = (self.stats["remaps"] == 0)
        self.out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.out.bind(("0.0.0.0", self.fixed_first if first else 0))
        self.out.settimeout(0.2)
        self.stats["remaps"] += 1
        self._log(f"mapping {'re' if self.stats['remaps'] > 1 else ''}created "
                  f"(external port {self.out.getsockname()[1]}, {why})")

    def _expire_mapping_locked(self, why):
        if self.out is None:
            return
        try: self.out.close()
        except OSError: pass
        self.out = None
        self._log(f"mapping EXPIRED ({why}) -- inbound now vanishes until "
                  f"next outbound")

    def _ttl_expired_locked(self, now):
        return (self.ttl is not None and self.out is not None and
                self.last_outbound > 0 and now - self.last_outbound > self.ttl)

    def external_port(self):
        with self.lock:
            return self.out.getsockname()[1] if self.out else 0

    # -- pumps ---------------------------------------------------------------
    def _pump_inside(self):             # NAT'd peer -> world
        while self.alive:
            try:
                data, src = self.inside.recvfrom(65536)
            except (socket.timeout, OSError):
                # idle: still enforce TTL + scheduled rebind so mappings die
                # on time even with no inside traffic
                with self.lock:
                    now = time.monotonic()
                    if self._ttl_expired_locked(now):
                        self._expire_mapping_locked(f"idle > {self.ttl}s")
                    if self.rebind_at and now >= self.rebind_at and self.out:
                        self.rebind_at = None
                        self._fresh_mapping_locked("scheduled CGNAT rebind")
                continue
            self.inside_peer = src
            with self.lock:
                now = time.monotonic()
                if self._ttl_expired_locked(now):
                    self._expire_mapping_locked(f"idle > {self.ttl}s")
                if self.rebind_at and now >= self.rebind_at:
                    self.rebind_at = None
                    self._fresh_mapping_locked("scheduled CGNAT rebind")
                elif self.out is None:
                    self._fresh_mapping_locked("outbound packet")
                self.last_outbound = now
                self.allowed_eps.add(self.dest)
                self.allowed_ips.add(self.dest[0])
                try:
                    self.out.sendto(data, self.dest)
                    self.stats["out"] += 1
                except OSError:
                    pass

    def _pump_outside(self):            # world -> NAT'd peer
        while self.alive:
            with self.lock:
                out = self.out
            if out is None:
                self.stats["in_expired"] += 0  # nothing to receive on
                time.sleep(0.05)
                continue
            try:
                data, src = out.recvfrom(65536)
            except (socket.timeout, OSError):
                continue
            if not self._inbound_allowed(src):
                self.stats["in_filtered"] += 1
                continue
            if self.inside_peer:
                try:
                    self.inside.sendto(data, self.inside_peer)
                    self.stats["in_ok"] += 1
                except OSError:
                    pass

    def _inbound_allowed(self, src):
        if self.mode == "full_cone":
            return True
        if self.mode == "restricted":
            return src[0] in self.allowed_ips
        # port_restricted / symmetric
        return src in self.allowed_eps

    def stop(self):
        self.alive = False


if __name__ == "__main__":
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(2)
    inside = int(sys.argv[1])
    ip, port = sys.argv[2].rsplit(":", 1)
    mode = sys.argv[3]
    ttl = float(sys.argv[4]) if len(sys.argv) > 4 and sys.argv[4] != "0" else None
    reb = float(sys.argv[5]) if len(sys.argv) > 5 and sys.argv[5] != "0" else None
    p = NatProxy(inside, (ip, int(port)), mode, ttl, reb)
    print(f"[nat] {mode} inside={inside} dest={ip}:{port} ttl={ttl} rebind={reb}")
    try:
        while True:
            time.sleep(5)
            print(f"[nat] stats={p.stats} ext_port={p.external_port()}")
    except KeyboardInterrupt:
        p.stop()
