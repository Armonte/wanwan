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

Usage: python3 tools/transition_churn_smoke.py [seconds] [loss] [delay_ms] [seed] [rebind_s]
  rebind_s > 0: a harness-side UDP proxy sits between guest and host and, at
  that many seconds in, CLOSES its host-facing socket and rebinds a fresh
  port -- the host sees the guest's source port change AND the host's sends
  to the old port die, i.e. a faithful one-sided CGNAT mid-session remap
  (task #52, the Patrick/Melancholy freeze; Melancholy is on ETECSA CGNAT).
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

    def rebind(self):
        with self.b_lock:
            try: self.b.close()
            except OSError: pass
            self.b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.b.bind(("0.0.0.0", 0))   # fresh ephemeral port
            self.b.settimeout(0.2)
            self.rebound = True
        print(f"[churn] PROXY REBOUND -- host-facing port now "
              f"{self.b.getsockname()[1]} (old mapping dead)")

    def stop(self):
        self.alive = False

if __name__ == "__main__":
    secs   = int(sys.argv[1])   if len(sys.argv) > 1 else 240
    loss   = sys.argv[2]        if len(sys.argv) > 2 else "0.10"
    delay  = sys.argv[3]        if len(sys.argv) > 3 else "100"
    seed   = sys.argv[4]        if len(sys.argv) > 4 else "42"
    rebind = sys.argv[5]        if len(sys.argv) > 5 else "0"
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
    }

    def make_env(extra):
        env = dict(os.environ); env.update(common); env.update(extra)
        keys = list(common.keys()) + list(extra.keys())
        prev = env.get("WSLENV", "")
        env["WSLENV"] = ":".join(keys) + ((":" + prev) if prev else "")
        return env

    PROXY_A, PROXY_B = 7050, 7100

    def wsl_ip():
        return subprocess.check_output(["hostname", "-I"], text=True).split()[0]

    def win_host_ip():
        out = subprocess.check_output(["ip", "route"], text=True)
        for ln in out.splitlines():
            if ln.startswith("default"):
                return ln.split()[2]
        raise RuntimeError("no default route")

    proxy = None
    if rebind != "0":
        # Interpose the rebind proxy. The proxy is a LINUX process: Windows
        # game -> WSL only delivers UDP via the WSL VM IP (never 127.0.0.1),
        # and WSL -> Windows game targets the Windows host at the default-gw
        # address. Games' Windows-sourced packets arrive NAT'd from that same
        # gw address; replies route back through the NAT.
        WSL_IP, WIN_IP = wsl_ip(), win_host_ip()
        print(f"[churn] proxy on WSL {WSL_IP} (A={PROXY_A} B={PROXY_B}), "
              f"games via Windows host {WIN_IP}")
        proxy = RebindProxy(PROXY_A, PROXY_B, (WIN_IP, P1_PORT))
        host_remote  = f"{WSL_IP}:{PROXY_B}"
        guest_remote = f"{WSL_IP}:{PROXY_A}"
    else:
        host_remote  = f"127.0.0.1:{P2_PORT}"
        guest_remote = f"127.0.0.1:{P1_PORT}"

    p1_env = make_env({"FM2K_LOCAL_PORT": str(P1_PORT),
                       "FM2K_REMOTE_ADDR": host_remote})
    p2_env = make_env({"FM2K_LOCAL_PORT": str(P2_PORT),
                       "FM2K_REMOTE_ADDR": guest_remote})
    p1_args = [str(s.LAUNCHER), "--host", game_arg, "--port", str(P1_PORT)]
    p2_args = [str(s.LAUNCHER), "--connect", guest_remote, game_arg,
               "--port", str(P2_PORT)]

    print(f"[churn] 2 instances, {secs}s of 1-round/{common['FM2K_TEST_ROUND_TIME']}s "
          f"matches, loss={loss} delay={delay}ms seed={seed} guest_rebind_s={rebind}")
    l1 = open(s.OUT_DIR / "churn_p1.log", "w")
    l2 = open(s.OUT_DIR / "churn_p2.log", "w")
    p1 = subprocess.Popen(p1_args, env=p1_env, stdout=l1, stderr=subprocess.STDOUT)
    time.sleep(1.5)
    p2 = subprocess.Popen(p2_args, env=p2_env, stdout=l2, stderr=subprocess.STDOUT)

    if proxy and int(rebind) > 0:
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
        print(f"[churn] {tag}: battle_sessions={created} barriers={len(epochs)} "
              f"beats={len(beats)} freezes={freezes} desync={desync} crash={crash}")
        return created, epochs, freezes, desync, crash

    dbg = sorted(glob.glob(str(log_dir / "FM2K_P*_Debug.log")), key=os.path.getmtime)
    if len(dbg) < 2:
        print("[churn] FAIL -- need both game logs"); sys.exit(1)
    r1 = analyze(dbg[-2], "A")
    r2 = analyze(dbg[-1], "B")

    ok = True
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
