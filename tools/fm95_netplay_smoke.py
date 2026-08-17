#!/usr/bin/env python3
"""FM95 2-instance netplay smoke — Phase 3.

Two CPW instances over loopback (--host / --connect), both autoplay, both
under FM95_TRAMPOLINE (auto-set by StartOnlineSession for FM95). gekko runs
real predictive rollback between them. The FM95 sim is proven rollback-
deterministic (Phase 2c stress gate), so a clean loopback match = FM95 netplay
works. Verdict per instance: reached battle, no crash, gekko desync=0.

Launches direct via WSL interop (argv/env cross as UTF-16/WSLENV-clean) — the
fullwidth ＣＰＷ path breaks the spec_selftest .bat launcher, and a plain
Popen env= is dropped by WSL unless listed in WSLENV.

Usage:  python3 tools/fm95_netplay_smoke.py [frames]
"""
import sys, os, time, glob, subprocess, re
sys.path.insert(0, "tools")
import spec_selftest as s

if __name__ == "__main__":
    frames = sys.argv[1] if len(sys.argv) > 1 else "1500"
    P1_PORT, P2_PORT = 7000, 7001

    cpw = s.GAMES["cpw"]
    if not cpw.exists():
        print(f"CPW not found: {cpw}"); sys.exit(2)
    game_arg = s.to_win(cpw)
    log_dir = cpw.parent / "logs"

    for f in glob.glob(str(cpw.parent / "FM95_P*_desync_f*.log")) + \
             glob.glob(str(log_dir / "FM95_P*_desync_f*.log")):
        try: os.remove(f)
        except OSError: pass
    s.kill_strays(); time.sleep(1.0)

    common = {
        "FM2K_PARITY_AUTOPLAY":         "1",
        "FM2K_PARITY_AUTOPLAY_BATTLE":  "1",
        "FM2K_AUTO_TITLE_SKIP":         "1",
        "FM2K_AUTO_TERMINATE_AT_FRAME": str(frames),
        "FM2K_PRODUCTION_MODE":         "0",
        # FM2K_TEST_BACKGROUND: ON BY DEFAULT (opt out with =0). Sits in
        # `common` so make_env() puts it in WSLENV, which is what carries it
        # across the WSL->Windows boundary for these direct-Popen launches.
        "FM2K_TEST_BACKGROUND": os.environ.get("FM2K_TEST_BACKGROUND", "1"),
    }

    def make_env(extra):
        env = dict(os.environ); env.update(common); env.update(extra)
        keys = list(common.keys()) + list(extra.keys())
        prev = env.get("WSLENV", "")
        env["WSLENV"] = ":".join(keys) + ((":" + prev) if prev else "")
        return env

    p1_env = make_env({"FM2K_LOCAL_PORT": str(P1_PORT),
                       "FM2K_REMOTE_ADDR": f"127.0.0.1:{P2_PORT}"})
    p2_env = make_env({"FM2K_LOCAL_PORT": str(P2_PORT),
                       "FM2K_REMOTE_ADDR": f"127.0.0.1:{P1_PORT}"})
    p1_args = [str(s.LAUNCHER), "--host", game_arg, "--port", str(P1_PORT)]
    p2_args = [str(s.LAUNCHER), "--connect", f"127.0.0.1:{P1_PORT}", game_arg,
               "--port", str(P2_PORT)]

    print(f"[fm95-netplay] 2 CPW instances loopback, frames={frames} "
          f"(FM95_TRAMPOLINE auto-set by StartOnlineSession)")
    l1 = open(s.OUT_DIR / "fm95_np_p1.log", "w")
    l2 = open(s.OUT_DIR / "fm95_np_p2.log", "w")
    p1 = subprocess.Popen(p1_args, env=p1_env, stdout=l1, stderr=subprocess.STDOUT)
    time.sleep(1.5)
    p2 = subprocess.Popen(p2_args, env=p2_env, stdout=l2, stderr=subprocess.STDOUT)

    time.sleep(100)
    for p in (p1, p2):
        try: p.terminate()
        except Exception: pass
    s.kill_strays()
    subprocess.run(["powershell.exe", "-c",
                    "Get-Process ＣＰＷ,CPW -ErrorAction SilentlyContinue | Stop-Process -Force"],
                   capture_output=True)
    time.sleep(0.5)

    # ---- verdict from both game logs ----
    dbg = sorted(glob.glob(str(log_dir / "FM2K_P*_Debug.log")), key=os.path.getmtime)
    if len(dbg) < 1:
        print("[fm95-netplay] FAIL — no game debug logs"); sys.exit(1)
    overall_ok = True
    for lg in dbg[-2:]:
        text = open(lg, encoding="utf-8", errors="replace").read()
        tag = "P?" ; m = re.search(r"FM2K_(P\d)_Debug", lg)
        if m: tag = m.group(1)
        # "battle session created" + a BATTLE STATUS heartbeat prove we entered and
        # ran a real gekko battle. (The old "trampoline tick phase = BATTLE" line
        # only fired on the legacy host-driven path -- gone now that the FM95 loop is
        # owned, so grepping for it false-negatived a healthy run.)
        frm = [int(x) for x in re.findall(r"BATTLE STATUS: frame=(\d+)", text)]
        rb = [int(x) for x in re.findall(r"BATTLE STATUS:.*? rb=(\d+)", text)]
        reached = ("GekkoNet battle session created" in text) or bool(frm)
        ran = bool(frm) and max(frm) >= 500   # advanced past the heartbeat threshold
        desync = len(re.findall(r"desync=[1-9]|DESYNC", text))
        crash = ("[CRASH]" in text or "[TERMINATE]" in text or "=== CRASH" in text)
        ok = reached and ran and desync == 0 and not crash
        overall_ok &= ok
        print(f"[fm95-netplay] {tag}: reached_battle={reached} ran={ran} "
              f"max_rb={max(rb) if rb else 0} max_frame={max(frm) if frm else 0} "
              f"desync={desync} crash={crash} -> {'OK' if ok else 'BAD'}")
    dumps = glob.glob(str(cpw.parent / "FM95_P*_desync_f*.log")) + \
            glob.glob(str(log_dir / "FM95_P*_desync_f*.log"))
    if dumps:
        print("[fm95-netplay] DESYNC dumps:", dumps); overall_ok = False
    print("[fm95-netplay]", "PASS — FM95 2-INSTANCE NETPLAY WORKS." if overall_ok
          else "FAIL/INCOMPLETE — see logs.")
    sys.exit(0 if overall_ok else 4)
