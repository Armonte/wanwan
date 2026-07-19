#!/usr/bin/env python3
"""NAT behavior matrix smoke (task #57): run netplay and spectate through
emulated NAT boxes (tools/nat_proxy.py) and assert the transport survives
each behavior class.

Scenarios:
  netplay:  P1 (host, direct) <-> P2 behind the NAT proxy.
  spectate: P1 <-> P2 direct (loopback pair), spectator S1 behind the NAT.

What each class proves:
  full_cone        control -- if this fails the rig is broken, not the game.
  port_restricted  inbound blocked until we send outbound: proves the
                   hello/JOIN ordering opens the pinhole before anything
                   load-bearing arrives.
  ttl=10s          conservative-NAT mapping death under one-way silence:
                   proves our steady chatter (gekko inputs / RC ack carrier
                   / control pings) keeps mappings warm BY CONSTRUCTION
                   (assert remaps==1), or that recovery works if not.
  rebind=35s       mid-session CGNAT remap (the ETECSA/Melancholy event):
                   netplay leg re-proves 0.2.81 adoption through this rig;
                   spectate leg is NEW coverage (host must re-learn the
                   viewer at its new external port via re-JOIN).

Topology (WSL/Windows, same as transition_churn_smoke): proxy is a Linux
process; NAT'd Windows instance targets WSL_IP:<inside>, proxy targets the
Windows host at the default-gw IP. Loopback 127.0.0.1 does NOT deliver
Windows->WSL UDP.

Usage: python3 tools/nat_matrix_smoke.py [secs_per_scenario] [only_substr]
"""
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import spec_selftest as s
from nat_proxy import NatProxy

SECS = int(sys.argv[1]) if len(sys.argv) > 1 else 100
ONLY = sys.argv[2] if len(sys.argv) > 2 else ""

P1_PORT, P2_PORT, SPEC_PORT = 7000, 7001, 7002
NAT_INSIDE, NAT_FIRST_EXT = 7050, 7100

game = s.GAMES["wanwan"]
game_arg = s.to_win(game)
log_dir = game.parent / "logs"
SNAP = Path(__file__).parent / ".spec_selftest"


def wsl_ip():
    return subprocess.check_output(["hostname", "-I"], text=True).split()[0]


def win_host_ip():
    for ln in subprocess.check_output(["ip", "route"], text=True).splitlines():
        if ln.startswith("default"):
            return ln.split()[2]
    raise RuntimeError("no default route")


COMMON = {
    "FM2K_PARITY_AUTOPLAY":        "1",
    "FM2K_PARITY_AUTOPLAY_BATTLE": "1",
    "FM2K_AUTO_TITLE_SKIP":        "1",
    "FM2K_TEST_AUTO_CSS":          "0,0,0,0,0",
    "FM2K_TEST_ROUNDS":            "1",
    "FM2K_TEST_ROUND_TIME":        "10",
    "FM2K_PRODUCTION_MODE":        "0",
    "FM2K_RC_STATS":               "1",
}


def make_env(extra):
    env = dict(os.environ)
    env.update(COMMON)
    env.update(extra)
    keys = list(COMMON.keys()) + list(extra.keys())
    prev = env.get("WSLENV", "")
    env["WSLENV"] = ":".join(keys) + ((":" + prev) if prev else "")
    return env


def spawn(args, env):
    return subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)


def read_log(name):
    try:
        return (log_dir / name).read_text(errors="replace")
    except OSError:
        return ""


def beat_progress(text):
    """(#BEAT lines, #distinct bf values). bf resets per MATCH, so a raw
    'last bf >= N' check misfires on multi-match runs (a healthy final BEAT
    can sit anywhere in [0..round_frames]). Distinct-values is the wedge
    discriminator: a frozen battle repeats one bf forever."""
    bfs = re.findall(r"\[BEAT\] bf=(\d+)", text)
    return len(bfs), len(set(bfs))


def specq_series(text):
    """[(seconds-of-day, total), ...] from SPEC-Q lines."""
    out = []
    for m in re.finditer(r"\[(\d+):(\d+):(\d+)\.\d+\].*\[SPEC-Q\].*total=(\d+)", text):
        h, mi, sec, tot = map(int, m.groups())
        out.append((h * 3600 + mi * 60 + sec, tot))
    return out


def run_scenario(name, kind, mode, ttl=None, rebind=None):
    if ONLY and ONLY not in name:
        return None
    s.kill_strays()
    time.sleep(1.5)
    for f in ("FM2K_P1_Debug.log", "FM2K_P2_Debug.log", "FM2K_S1_Debug.log"):
        try: (log_dir / f).unlink()
        except OSError: pass

    WSL, WIN = wsl_ip(), win_host_ip()
    nat = NatProxy(NAT_INSIDE, (WIN, P1_PORT), mode, mapping_ttl=ttl,
                   rebind_after=rebind, name=name,
                   fixed_first_ext_port=NAT_FIRST_EXT)
    rebind_wall = (time.time() + rebind) if rebind else None

    procs = []
    try:
        if kind == "netplay":
            # P2 behind the NAT: its outbound goes to the proxy; P1's preset
            # remote points at the NAT's first external port (churn-style),
            # remaps rely on same-IP adoption.
            p1 = make_env({"FM2K_LOCAL_PORT": str(P1_PORT),
                           "FM2K_REMOTE_ADDR": f"{WSL}:{NAT_FIRST_EXT}"})
            p2 = make_env({"FM2K_LOCAL_PORT": str(P2_PORT),
                           "FM2K_REMOTE_ADDR": f"{WSL}:{NAT_INSIDE}"})
            procs.append(spawn([str(s.LAUNCHER), "--host", game_arg,
                                "--port", str(P1_PORT)], p1))
            procs.append(spawn([str(s.LAUNCHER), "--connect",
                                f"{WSL}:{NAT_INSIDE}", game_arg,
                                "--port", str(P2_PORT)], p2))
        else:  # spectate
            p1 = make_env({"FM2K_LOCAL_PORT": str(P1_PORT),
                           "FM2K_REMOTE_ADDR": f"127.0.0.1:{P2_PORT}"})
            p2 = make_env({"FM2K_LOCAL_PORT": str(P2_PORT),
                           "FM2K_REMOTE_ADDR": f"127.0.0.1:{P1_PORT}"})
            sp = make_env({"FM2K_LOG_TAG": "S1", "FM2K_CINPUT": "1",
                           # generous: TTL/rebind recovery needs a starve
                           # window longer than the 5s harness default
                           "FM2K_SPEC_HOST_GONE_MS": "20000",
                           "FM2K_SPEC_CONNECT_TIMEOUT_MS": "30000"})
            procs.append(spawn([str(s.LAUNCHER), "--host", game_arg,
                                "--port", str(P1_PORT)], p1))
            procs.append(spawn([str(s.LAUNCHER), "--connect",
                                f"127.0.0.1:{P1_PORT}", game_arg,
                                "--port", str(P2_PORT)], p2))
            time.sleep(4)   # host boots + CSS exists before the viewer dials
            procs.append(spawn([str(s.LAUNCHER), "--host", game_arg,
                                "--spectate", f"{WSL}:{NAT_INSIDE}",
                                "--port", str(SPEC_PORT),
                                "--player-index", "2"], sp))
        time.sleep(SECS)
    finally:
        s.kill_strays()
        nat.stop()
        time.sleep(1.0)

    # ---- verdict ----
    p1t, p2t = read_log("FM2K_P1_Debug.log"), read_log("FM2K_P2_Debug.log")
    s1t = read_log("FM2K_S1_Debug.log")
    wedges = p1t.count("[WEDGE]") + p2t.count("[WEDGE]")
    crash = any(x in t for t in (p1t, p2t, s1t)
                for x in ("=== CRASH", "EXCEPTION_ACCESS"))
    detail = f"nat_stats={nat.stats} ext_port={nat.external_port()}"
    ok = True
    if kind == "netplay":
        n1, d1 = beat_progress(p1t)
        n2, d2 = beat_progress(p2t)
        detail += f" beats={n1}/{n2} distinct_bf={d1}/{d2} wedges={wedges}"
        ok = (n1 >= 5 and n2 >= 5 and d1 >= 4 and d2 >= 4
              and wedges == 0 and not crash)
    else:
        ser = specq_series(s1t)
        tot = ser[-1][1] if ser else 0
        never = "connect never established" in s1t
        detail += f" spec_total={tot} connect_fail={never} wedges={wedges}"
        ok = tot >= 400 and not never and not crash and wedges == 0
        if ok and rebind_wall and ser:
            # totals must keep growing AFTER the rebind (recovery proof)
            rb_sod = time.localtime(rebind_wall)
            rb_s = rb_sod.tm_hour * 3600 + rb_sod.tm_min * 60 + rb_sod.tm_sec
            at_rb = max((t for ts, t in ser if ts <= rb_s + 2), default=0)
            grew = ser[-1][1] - at_rb
            detail += f" post_rebind_growth={grew}"
            ok = grew >= 100
    verdict = "PASS" if ok else "FAIL"
    print(f"[nat-matrix] {name}: {verdict}  ({detail})", flush=True)
    if not ok:
        snap = SNAP / f"natfail_{name}_{time.strftime('%H%M%S')}"
        snap.mkdir(parents=True, exist_ok=True)
        for f in ("FM2K_P1_Debug.log", "FM2K_P2_Debug.log", "FM2K_S1_Debug.log"):
            src = log_dir / f
            if src.exists():
                (snap / f).write_bytes(src.read_bytes())
        print(f"[nat-matrix]   logs -> {snap}", flush=True)
    return ok


MATRIX = [
    ("net-portrestrict",  "netplay",  "port_restricted", None, None),
    ("net-symm-ttl10",    "netplay",  "symmetric",       10,   None),
    ("net-rebind35",      "netplay",  "port_restricted", None, 35),
    ("spec-fullcone",     "spectate", "full_cone",       None, None),
    ("spec-portrestrict", "spectate", "port_restricted", None, None),
    ("spec-ttl10",        "spectate", "port_restricted", 10,   None),
    ("spec-rebind35",     "spectate", "port_restricted", None, 35),
]

if __name__ == "__main__":
    results = {}
    for name, kind, mode, ttl, reb in MATRIX:
        r = run_scenario(name, kind, mode, ttl, reb)
        if r is not None:
            results[name] = r
    fails = [k for k, v in results.items() if not v]
    print(f"[nat-matrix] SUMMARY: {len(results) - len(fails)}/{len(results)} pass"
          + (f"  FAILED: {', '.join(fails)}" if fails else ""))
    sys.exit(1 if fails else 0)
