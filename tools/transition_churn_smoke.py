#!/usr/bin/env python3
"""Transition-churn smoke -- repro rig for the mid-session battle-entry freeze
(task #52, Patrick/Melancholy 2026-07-17: 4th battle of a session enters on
both peers, gekko syncs 4/4 + starts, then BOTH freeze at bf~delay+pred with
rb_total=0 while gekko-layer ping stays healthy).

Two FM2K instances over loopback play MANY short matches (1 round, short
timer) under link impairment, churning the CSS<->battle gekko session
create/destroy cycle. Verdict gates:
  (a) FREEZE: consecutive [BEAT] heartbeats with the SAME bf on either peer
  (b) EPOCH PAIRING: the "both peers signaled ... epoch=N" sequences match
      across peers (same epochs, same swap frames)
  (c) desync count == 0
  (d) CYCLES: enough battle sessions actually got created to call it a churn

Usage: python3 tools/transition_churn_smoke.py [seconds] [loss] [delay_ms] [seed] [rebind]
  6th arg: a local IP to move the remap ONTO (default: port-only). An IP move
    is the fatal shape -- control_channel_io re-stamps SAME-IP packets with
    the pinned gekko actor, so a port-only remap is absorbed.
  rebind = "css+N": fire the remap below N MILLISECONDS after the log shows
  "Creating CSS GekkoSession" rather than N seconds after launch. Added
  2026-09-04 for the character-select peer-drop, which lands 0.3-0.5s into
  CSS -- a window a wall-clock sweep hits mostly by luck.
  rebind > 0: a harness-side UDP proxy sits between guest and host and, at
  that many seconds in, CLOSES its host-facing socket and rebinds a fresh
  port -- the host sees the guest's source port change AND the host's sends
  to the old port die, i.e. a faithful one-sided CGNAT mid-session remap
  (task #52, the Patrick/Melancholy freeze; Melancholy is on ETECSA CGNAT).

REQUIRES WSL MIRRORED NETWORKING whenever a proxy is used: the proxy is a
Linux process and the games are Windows processes, so the two must share a
loopback. The rig probes that at startup and REFUSES TO RUN rather than
black-hole every packet and still print a verdict -- which is what happened
on 2026-09-04, twice, before anyone noticed. It equally refuses to report
anything if the peers never reach "Full handshake complete".
"""
import sys, os, time, glob, subprocess, re, socket, threading
sys.path.insert(0, "tools")
import spec_selftest as s


class RebindProxy:
    """Guest <-> proxy <-> host. Socket A (fixed) faces the guest; socket B
    (fixed then REBOUND) faces the host. At rebind() the old B closes (host's
    sends to it now vanish -- dead CGNAT mapping) and a new B takes over
    (host sees a new source port for the guest's traffic)."""
    def __init__(self, a_port, b_port, host_addr):
        self.a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.a.bind(("0.0.0.0", a_port))
        self.a.settimeout(0.2)
        self.b_lock = threading.Lock()
        self.b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.b.bind(("0.0.0.0", b_port))
        self.b.settimeout(0.2)
        self.host_addr = host_addr
        self.guest_addr = None      # learned from first packet on A
        self.alive = True
        self.rebound = False
        threading.Thread(target=self._pump_a, daemon=True).start()
        threading.Thread(target=self._pump_b, daemon=True).start()

    def _pump_a(self):              # guest -> host
        while self.alive:
            try:
                data, src = self.a.recvfrom(65536)
            except (socket.timeout, OSError):
                continue
            self.guest_addr = src
            with self.b_lock:
                try: self.b.sendto(data, self.host_addr)
                except OSError: pass

    def _pump_b(self):              # host -> guest
        while self.alive:
            with self.b_lock:
                b = self.b
            try:
                data, _ = b.recvfrom(65536)
            except (socket.timeout, OSError):
                time.sleep(0.01); continue
            if self.guest_addr:
                try: self.a.sendto(data, self.guest_addr)
                except OSError: pass

    def rebind(self, new_ip=None, new_host=None):
        """Port-only by default. Pass new_ip/new_host to move the remap to a
        DIFFERENT SOURCE IP, which is a materially different failure: the
        receive path in control_channel_io re-stamps same-IP packets with the
        pinned gekko actor, so a port move is absorbed, while an IP move is
        not and gekko drops every peer packet."""
        with self.b_lock:
            try: self.b.close()
            except OSError: pass
            self.b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.b.bind((new_ip or "0.0.0.0", 0))   # fresh ephemeral port
            self.b.settimeout(0.2)
            if new_host: self.host_addr = new_host
            self.rebound = True
        print(f"[churn] PROXY REBOUND -- host-facing addr now "
              f"{self.b.getsockname()} (old mapping dead)"
              + (f", host reached at {self.host_addr}" if new_host else ""))

    def stop(self):
        self.alive = False

if __name__ == "__main__":
    secs   = int(sys.argv[1])   if len(sys.argv) > 1 else 240
    loss   = sys.argv[2]        if len(sys.argv) > 2 else "0.10"
    delay  = sys.argv[3]        if len(sys.argv) > 3 else "100"
    seed   = sys.argv[4]        if len(sys.argv) > 4 else "42"
    rebind = sys.argv[5]        if len(sys.argv) > 5 else "0"
    # 6th arg: a local IP to move the remap ONTO. Empty = port-only remap.
    alt_ip = sys.argv[6]        if len(sys.argv) > 6 else ""
    P1_PORT, P2_PORT = 7000, 7001

    game = s.GAMES["wanwan"]
    if not game.exists():
        print(f"game not found: {game}"); sys.exit(2)
    game_arg = s.to_win(game)
    log_dir = game.parent / "logs"

    s.kill_strays(); time.sleep(1.0)

    common = {
        "FM2K_PARITY_AUTOPLAY":         "1",
        "FM2K_PARITY_AUTOPLAY_BATTLE":  "1",
        "FM2K_AUTO_TITLE_SKIP":         "1",
        "FM2K_TEST_AUTO_CSS":           "0,0,0,0,0",
        "FM2K_TEST_ROUNDS":             "1",
        "FM2K_TEST_ROUND_TIME":         "10",
        "FM2K_PRODUCTION_MODE":         "0",
        "FM2K_NET_LOSS":                loss,
        "FM2K_NET_DELAY_MS":            delay,
        "FM2K_NET_SEED":                seed,
        # Both peers share a match token, exactly as a hub-brokered match
        # does. Without it the NAT layer never arms, and the address-remap
        # recovery path (an authenticated punch challenge) silently no-ops --
        # so a rig without a token cannot test the very thing it remaps.
        "FM2K_HUB_MATCH_TOKEN": "00112233445566778899aabbccddeeff",
    }

    def make_env(extra):
        env = dict(os.environ); env.update(common); env.update(extra)
        keys = list(common.keys()) + list(extra.keys())
        prev = env.get("WSLENV", "")
        env["WSLENV"] = ":".join(keys) + ((":" + prev) if prev else "")
        return env

    PROXY_A, PROXY_B = 7050, 7100

    def loopback_reaches_wsl(port=7049, timeout=6.0):
        """Can a WINDOWS process deliver UDP to a socket this LINUX process
        bound, at 127.0.0.1? Under WSL mirrored networking yes -- the two
        share one network namespace. Under NAT-mode WSL no.

        PROBE IT, NEVER ASSUME IT. Two runs of this rig on 2026-09-04
        measured absolutely nothing because the addressing was wrong and
        nothing said so: the proxy forwarded every game packet to the
        default-route gateway, which under mirrored networking is the LAN
        ROUTER and not Windows at all. Both peers sat emitting HELLO for the
        whole run and the rig still printed a verdict."""
        rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            rx.bind(("0.0.0.0", port))
            rx.settimeout(timeout)
            subprocess.Popen(
                ["powershell.exe", "-NoProfile", "-Command",
                 "$u=New-Object System.Net.Sockets.UdpClient;"
                 "$b=[Text.Encoding]::ASCII.GetBytes('churn-probe');"
                 f"$u.Send($b,$b.Length,'127.0.0.1',{port})|Out-Null"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            rx.recvfrom(64)
            return True
        except Exception:
            return False
        finally:
            rx.close()

    proxy = None
    if rebind != "0":
        # Interpose the rebind proxy. The proxy is a LINUX process and the
        # games are WINDOWS processes, so the two must share a loopback --
        # which is what mirrored networking gives us, and what the probe
        # above confirms before a single game is launched.
        if not loopback_reaches_wsl():
            print("[churn] RIG BROKEN -- a Windows process cannot reach this "
                  "WSL socket at 127.0.0.1, so the proxy would black-hole "
                  "every game packet.\n"
                  "        This rig needs WSL mirrored networking: put "
                  "networkingMode=mirrored under [wsl2] in %USERPROFILE%\\"
                  ".wslconfig and `wsl --shutdown`.\n"
                  "        Refusing to run: a proxy that drops everything "
                  "produces a confident verdict about nothing.")
            sys.exit(5)
        print(f"[churn] proxy on loopback (A={PROXY_A} B={PROXY_B}), "
              f"mirrored networking verified")
        proxy = RebindProxy(PROXY_A, PROXY_B, ("127.0.0.1", P1_PORT))
        host_remote  = f"127.0.0.1:{PROXY_B}"
        guest_remote = f"127.0.0.1:{PROXY_A}"
    else:
        host_remote  = f"127.0.0.1:{P2_PORT}"
        guest_remote = f"127.0.0.1:{P1_PORT}"

    # Optional: make ONE side block inside the stage load, standing in for a
    # slow disk (the 2026-09-05 South American report). Host only, so the
    # asymmetry matches the field case.
    stall_env = {}
    if os.environ.get("CHURN_STAGE_STALL_MS"):
        stall_env["FM2K_TEST_STAGE_LOAD_STALL_MS"] = \
            os.environ["CHURN_STAGE_STALL_MS"]

    p1_env = make_env({"FM2K_LOCAL_PORT": str(P1_PORT),
                       "FM2K_REMOTE_ADDR": host_remote, **stall_env})
    p2_env = make_env({"FM2K_LOCAL_PORT": str(P2_PORT),
                       "FM2K_REMOTE_ADDR": guest_remote})
    p1_args = [str(s.LAUNCHER), "--host", game_arg, "--port", str(P1_PORT)]
    p2_args = [str(s.LAUNCHER), "--connect", guest_remote, game_arg,
               "--port", str(P2_PORT)]

    print(f"[churn] 2 instances, {secs}s of 1-round/{common['FM2K_TEST_ROUND_TIME']}s "
          f"matches, loss={loss} delay={delay}ms seed={seed} guest_rebind_s={rebind}")
    l1 = open(s.OUT_DIR / "churn_p1.log", "w")
    l2 = open(s.OUT_DIR / "churn_p2.log", "w")

    # Only text APPENDED from here on counts. The log directory keeps every
    # previous run's file, and a marker search that reads them whole finds
    # last week's handshake and calls the rig healthy.
    def log_baseline():
        return {f: os.path.getsize(f)
                for f in glob.glob(str(log_dir / "FM2K_P*_Debug.log"))}

    def wait_for_marker(marker, baseline, timeout):
        """Seconds until `marker` first appears in any game log past the
        baseline offsets, or None on timeout. Hook logging is synchronous,
        so a line that has been emitted is already on disk."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            for f in glob.glob(str(log_dir / "FM2K_P*_Debug.log")):
                try:
                    start = baseline.get(f, 0)
                    # The game REUSES these filenames and truncates them at
                    # launch, so a stale offset from the previous run seeks
                    # clean past the whole log. Shrunk file means new file.
                    if os.path.getsize(f) < start:
                        start = 0
                    with open(f, encoding="utf-8", errors="replace") as fh:
                        fh.seek(start)
                        if marker in fh.read():
                            return time.time() - t0
                except OSError:
                    pass
            time.sleep(0.05)
        return None

    base = log_baseline()
    p1 = subprocess.Popen(p1_args, env=p1_env, stdout=l1, stderr=subprocess.STDOUT)
    time.sleep(1.5)
    p2 = subprocess.Popen(p2_args, env=p2_env, stdout=l2, stderr=subprocess.STDOUT)

    def abort_rig(why):
        for q in (p1, p2):
            try: q.terminate()
            except Exception: pass
        if proxy: proxy.stop()
        s.kill_strays()
        print(f"[churn] RIG BROKEN -- {why}\n"
              "        This is an environment failure, NOT a finding. Do not "
              "record it as a pass or a repro.")
        sys.exit(5)

    # PRECONDITION. Everything this rig measures happens downstream of the
    # handshake, so if the peers never connect the verdict is meaningless --
    # and it is not a hypothetical: two runs died exactly here, silently.
    if wait_for_marker("Full handshake complete", base, 60) is None:
        abort_rig("the peers never completed the handshake in 60s "
                  "(both sides still sending HELLO)")

    if proxy and rebind.startswith("css+"):
        # Aim the remap at CHARACTER SELECT, not at the wall clock. The field
        # failure lands 0.3-0.5s after the CSS session is created, and which
        # wall-clock second that falls on varies per run, so a blind sweep
        # mostly tests the parts of the timeline we already know are healthy.
        ms = int(rebind[4:])
        took = wait_for_marker("Creating CSS GekkoSession", base, 90)
        if took is None:
            abort_rig("no CSS GekkoSession was ever created in 90s")
        print(f"[churn] CSS session created at t+{took:.1f}s; "
              f"rebinding {ms}ms into character select")
        time.sleep(ms / 1000.0)
        # Source IP only. The host stays reachable where it already is, so
        # traffic KEEPS FLOWING and only gekko's actor match breaks -- which
        # is the field shape. Redirecting the host address too just
        # black-holes the link and proves nothing (tried 2026-09-04 with the
        # LAN IP, which WSL cannot deliver to Windows at all).
        proxy.rebind(alt_ip) if alt_ip else proxy.rebind()
        time.sleep(max(0, secs - took - ms / 1000.0))
    elif proxy and int(rebind) > 0:
        time.sleep(int(rebind))
        proxy.rebind()
        time.sleep(max(0, secs - int(rebind)))
    else:
        time.sleep(secs)
    for p in (p1, p2):
        try: p.terminate()
        except Exception: pass
    if proxy: proxy.stop()
    s.kill_strays()
    time.sleep(0.5)

    # ---- verdict ----
    def analyze(path, tag):
        text = open(path, encoding="utf-8", errors="replace").read()
        created = text.count("GekkoNet battle session created")
        epochs  = re.findall(
            r"BATTLE (?:END )?SYNC: both peers signaled \([^)]*swap_frame=(\d+), epoch=(\d+)\)",
            text)
        beats   = re.findall(r"\[BEAT\] bf=(\d+)", text)
        # freeze = same bf on consecutive BEATs (10s heartbeat cadence), while
        # more log lines follow (i.e. the process was alive, not just exiting)
        freezes = []
        for i in range(1, len(beats)):
            if beats[i] == beats[i - 1]:
                freezes.append(int(beats[i]))
        desync  = len(re.findall(r"DESYNC DETECTED|desync detected", text))
        crash   = ("=== CRASH" in text or "[TERMINATE]" in text)
        # The character-select peer-drop this rig was re-aimed at on
        # 2026-09-04: the peer stops feeding gekko during CSS and the session
        # aborts. [CSS-BEAT] addr_match=NO says gekko is discarding the
        # peer's packets because they arrive from an address that is not the
        # actor it was registered with -- which names the cause outright.
        css_abort = len(re.findall(r"publishing CSS_ABORT", text))
        # Starvation, not address shape. A remapped address is normal and
        # recoverable; what is fatal is peer input no longer reaching gekko,
        # which [CSS-BEAT] reports directly.
        addr_no   = len(re.findall(r"STARVED", text))
        print(f"[churn] {tag}: battle_sessions={created} barriers={len(epochs)} "
              f"beats={len(beats)} freezes={freezes} desync={desync} "
              f"css_abort={css_abort} addr_mismatch={addr_no} crash={crash}")
        return created, epochs, freezes, desync, crash, css_abort, addr_no

    dbg = sorted(glob.glob(str(log_dir / "FM2K_P*_Debug.log")), key=os.path.getmtime)
    if len(dbg) < 2:
        print("[churn] FAIL -- need both game logs"); sys.exit(1)
    r1 = analyze(dbg[-2], "A")
    r2 = analyze(dbg[-1], "B")

    ok = True
    # Checked FIRST, and before the cycle-count gate: a CSS abort legitimately
    # starves the battle pipeline, and exiting "INCOMPLETE" there would bury
    # the exact repro this run exists to catch.
    if r1[5] or r2[5]:
        print(f"[churn] CSS PEER-DROP REPRODUCED -- CSS_ABORT on "
              f"A={r1[5]} B={r2[5]}")
        if r1[6] or r2[6]:
            print(f"[churn] preceded by CSS starvation (A={r1[6]} B={r2[6]} "
                  "beats with no peer input reaching gekko) -- check the "
                  "addr= field on those beats for a remap that did not recover")
        else:
            print("[churn] with NO starvation beforehand -- the abort is not "
                  "an input-delivery failure; look elsewhere")
        ok = False
    elif r1[6] or r2[6]:
        # Starvation that RECOVERS is the designed outcome, not a failure. A
        # slow stage load genuinely stops peer input for seconds; what must
        # not happen is the session dying of it, and that is checked above.
        print(f"[churn] note: CSS starved briefly (A={r1[6]} B={r2[6]} beats) "
              "but the session rode it out -- expected when a load blocks")
    if r1[2] or r2[2]:
        print(f"[churn] FREEZE REPRODUCED -- bf stalls: A={r1[2]} B={r2[2]}")
        ok = False
    if [e for e in r1[1]] != [e for e in r2[1]]:
        # sequences may differ in tail length if terminated mid-barrier; compare
        # the common prefix strictly
        n = min(len(r1[1]), len(r2[1]))
        if r1[1][:n] != r2[1][:n]:
            print(f"[churn] EPOCH/SWAP MISMATCH: A={r1[1][:n]} B={r2[1][:n]}")
            ok = False
    if r1[3] or r2[3]:
        print("[churn] desync detected"); ok = False
    if r1[4] or r2[4]:
        print("[churn] crash detected"); ok = False
    if min(r1[0], r2[0]) < 4:
        print(f"[churn] INCOMPLETE -- only {min(r1[0], r2[0])} battle sessions "
              "(need >= 4 cycles to exercise the churn)")
        sys.exit(3)
    # Cycle-rate gate: a healthy pair produces a battle roughly every ~18s
    # (1-round/10s matches + CSS walk). A mid-run disruption (rebind freeze,
    # timeout teardown, desync stall) eats cycles even when no single BEAT pair
    # freezes -- the broken-rebind run produced 5 cycles where ~10 fit.
    expected = secs // 18
    if min(r1[0], r2[0]) < max(4, int(expected * 0.7)):
        print(f"[churn] CYCLE-RATE FAIL -- {min(r1[0], r2[0])} battle cycles in "
              f"{secs}s (expected ~{expected}); a disruption ate the pipeline")
        ok = False
    if ok:
        print(f"[churn] PASS -- no freeze across {min(r1[0], r2[0])} battle cycles")
    else:
        # Self-snapshot on ANY failure -- wrapper scripts kept copying the wrong
        # file after later runs clobbered the live logs.
        import shutil
        stamp = time.strftime("%H%M%S")
        keep = s.OUT_DIR / f"churn_fail_{stamp}"
        keep.mkdir(parents=True, exist_ok=True)
        for f in dbg[-2:] + glob.glob(str(log_dir / "FM2K_P*_Crash.log")):
            try: shutil.copy(f, keep)
            except OSError: pass
        print(f"[churn] FAIL -- see above (logs kept in {keep})")
    sys.exit(0 if ok else 4)
