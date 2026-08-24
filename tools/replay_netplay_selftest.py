#!/usr/bin/env python3
"""Real-netplay replay determinism test.

Spins up TWO launcher instances in 127.0.0.1 loopback (real GekkoSession,
predictive rollback, not stress). Both peers run autoplay so the match
plays out deterministically. After auto-terminate, the harness:

  1. Picks the host's .fm2krep (P1's recording — the canonical one)
  2. Re-launches that file in --replay mode
  3. Diffs record.pty (P1's parity stream) vs replay.pty
  4. Reports first divergent frame, if any

This is the FAITHFUL test for the "replays desync from matches" bug —
the stress-session harness skips real-netplay's predictive rollback,
which is what users actually hit.
"""

from __future__ import annotations
import argparse, os, secrets, shutil, subprocess, sys, threading, time
from pathlib import Path

LAUNCHER = Path("/mnt/c/games/FM2K_RollbackLauncher.exe")
RELAY_STUB = Path(__file__).parent / "relay_stub.py"
ONEWAY_PROXY = Path(__file__).parent / "oneway_proxy.py"
# Port the --oneway proxy binds. Both peers aim their remote here; it
# forwards P2 -> P1 and drops P1 -> P2, so P1 latches a dead mapping.
ONEWAY_PORT = 7005
# TEST-NET-1 (RFC 5737). Globally routable as "documentation/example" only;
# real networks blackhole it, so punch packets sent here vanish and the
# direct hole-punch provably fails -> the hub relay engages. We give each
# peer a DISTINCT blackhole port purely for log clarity; the address is
# unreachable either way.
RELAY_BLACKHOLE_IP = "192.0.2.1"
# Game override: FM2K_GAME_EXE targets a specific game (e.g. pkmncc)
# instead of the default WonderfulWorld. Accepts a Windows (D:\...) or
# WSL (/mnt/d/...) path. Pair with FM2K_TEST_CSS_CHAR to select a
# specific roster character mirror (e.g. Bewear=3 in pkmncc).
def _resolve_game_exe() -> Path:
    v = os.environ.get("FM2K_GAME_EXE")
    if not v:
        return Path("/mnt/c/games/2dfm/wanwan/WonderfulWorld_ver_0946.exe")
    if len(v) > 2 and v[1] == ":":  # Windows path -> /mnt/<drive>/
        v = "/mnt/" + v[0].lower() + v[2:].replace("\\", "/")
    return Path(v)
GAME_EXE = _resolve_game_exe()
OUT_DIR  = Path("/mnt/c/dev/wanwan/tools/.netplay_selftest")
PARITY_DIFF = Path(__file__).parent / "parity_diff.py"
P1_PORT = 7000
P2_PORT = 7001

def to_win(p: Path) -> str:
    s = str(p)
    if s.startswith("/mnt/") and len(s) > 6 and s[6] == "/":
        return s[5].upper() + ":" + s[6:]
    return s

def find_windows_python() -> str | None:
    # The relay stub MUST run as a Windows process so it binds the same
    # 127.0.0.1 loopback the Windows game sees. WSL2 ("nat" networking) puts
    # the Linux side on a separate stack -- a WSL-bound socket is unreachable
    # from the Windows game at 127.0.0.1. Prefer python.exe / py.exe on PATH.
    for cand in ("python.exe", "py.exe"):
        path = shutil.which(cand)
        if path:
            return path
    # Common fixed install location as a fallback.
    for p in ("/mnt/c/Program Files/Python313/python.exe",
              "/mnt/c/Program Files/Python312/python.exe",
              "/mnt/c/Program Files/Python311/python.exe"):
        if Path(p).exists():
            return p
    return None

def launch(label, args, env_extra, log_path, timeout, done_when=None):
    # WSL→Windows env vars don't cross via subprocess.Popen(env=) directly.
    # Use cmd.exe /C "set K=V && ... && exe args" so the Windows process
    # sees the env vars. Same trick the older replay_selftest.py uses.
    #
    # FM2K_TEST_BACKGROUND: ON BY DEFAULT (opt out with =0). Same single-funnel
    # placement as spec_selftest.launch -- every launcher/game this harness
    # opens comes up minimized-without-activation.
    env_extra = dict(env_extra)
    env_extra.setdefault("FM2K_TEST_BACKGROUND",
                         os.environ.get("FM2K_TEST_BACKGROUND", "1"))
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_f = open(log_path, "w")
    set_parts = [f"set {k}={v}" for k, v in env_extra.items()]
    win_args = []
    for a in args:
        if a.startswith("/mnt/") and len(a) > 6 and a[6] == "/":
            a = a[5].upper() + ":" + a[6:]
        win_args.append(f'"{a}"' if " " in a else a)
    cmd_inner = "&& ".join(set_parts + [" ".join(win_args)])
    cmd_args = ["cmd.exe", "/C", cmd_inner]
    print(f"[{label}] launching via cmd.exe: {' '.join(win_args)}")
    for k, v in env_extra.items():
        print(f"[{label}]   {k}={v}")
    print(f"[{label}]   log: {log_path}")
    proc = subprocess.Popen(cmd_args, stdout=log_f, stderr=subprocess.STDOUT)
    deadline = time.time() + timeout
    rc = None
    try:
        while True:
            if proc.poll() is not None:
                rc = proc.returncode
                print(f"[{label}] exited rc={rc}")
                break
            if done_when is not None and done_when():
                print(f"[{label}] done_when satisfied; killing")
                proc.terminate()
                try: proc.wait(timeout=3)
                except subprocess.TimeoutExpired: proc.kill()
                rc = 0
                break
            if time.time() > deadline:
                print(f"[{label}] TIMEOUT after {timeout}s; killing")
                proc.terminate()
                try: proc.wait(timeout=3)
                except subprocess.TimeoutExpired: proc.kill()
                rc = 124
                break
            time.sleep(0.5)
    finally:
        log_f.close()
    return rc

def find_latest_fm2krep(game_dir: Path, since: float, suffix="") -> Path | None:
    rep = game_dir / "replays"
    if not rep.is_dir(): return None
    cands = []
    for f in rep.glob(f"*{suffix}.fm2krep"):
        try:
            m = f.stat().st_mtime
            if m + 1.0 >= since: cands.append((m, f))
        except OSError: pass
    return max(cands)[1] if cands else None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--record-timeout", type=float, default=180.0)
    ap.add_argument("--replay-timeout", type=float, default=120.0)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--relay", action="store_true",
                    help="Relay-floor mode: start a local hub-relay data plane "
                         "(tools/relay_stub.py), inject FM2K_HUB_RELAY_ADDR + "
                         "FM2K_HUB_RELAY_SESSION into both peers, and point each "
                         "peer's remote at a TEST-NET-1 blackhole so the direct "
                         "hole-punch fails and the hub relay engages. PASS = both "
                         "logs show 'relay mode ENGAGED' then 'CONNECTED!', and "
                         "the match completes through CSS -> battle.")
    ap.add_argument("--relay-port", type=int, default=7712,
                    help="UDP port for the local relay stub (default 7712, "
                         "matches the hub's relay listen port).")
    ap.add_argument("--oneway", action="store_true",
                    help="Asymmetric-path mode (implies --relay): insert "
                         "tools/oneway_proxy.py between the peers so P2 -> P1 "
                         "delivers and P1 -> P2 is dropped. Reproduces the "
                         "2026-08-23 field deadlock, where P1 authenticates "
                         "P2's punch and commits to a direct path that only "
                         "works one way while P2 moves to the relay. PASS = "
                         "P1's log shows relay ENGAGED for the ONE-WAY reason "
                         "and both peers reach CONNECTED. Before the fix, P1 "
                         "never engages and both sides spin in the HELLO loop.")
    args = ap.parse_args()
    if args.oneway:
        # The deadlock is only interesting if there is a path to recover
        # ONTO. Without the relay both peers are simply unreachable and the
        # run proves nothing about the fix.
        args.relay = True

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    record_pty = OUT_DIR / "record.pty"
    replay_pty = OUT_DIR / "replay.pty"
    for f in (record_pty, replay_pty):
        if f.exists(): f.unlink()

    game_arg = to_win(GAME_EXE)
    game_dir = GAME_EXE.parent

    # Common env: autoplay drives title→CSS→battle, both peers compute
    # local input via Hook_ComputeAutoplayBattleInput(player_id, buf_idx).
    # FM2K_AUTO_TERMINATE_AT_FRAME fires at a confirmed battle frame on
    # each peer; both will hit ~same frame because gekko keeps them in
    # lockstep.
    common_env = {
        "FM2K_PARITY_AUTOPLAY": "1",
        "FM2K_PARITY_AUTOPLAY_BATTLE": "1",
        "FM2K_AUTO_TITLE_SKIP": "1",
        "FM2K_AUTO_TERMINATE_AT_FRAME": str(args.frames),
    }
    # Boot-to-battle mode (set FM2K_BTB_STAGE): both peers /F straight into
    # battle on the pinned stage, skipping CSS entirely. Needed for games
    # whose CSS crashes (Robot Heroes) AND to pin a specific (heavy) stage so
    # the netplay frame-skip catch-up gets exercised on a real heavy render.
    # Otherwise use CSS auto-confirm (char 0 / stage 0), the default for
    # well-behaved games like WonderfulWorld.
    if os.environ.get("FM2K_BTB_STAGE"):
        common_env["FM2K_BOOT_TO_BATTLE"] = "1"
        common_env["FM2K_BTB_STAGE"] = os.environ["FM2K_BTB_STAGE"]
        print(f"[harness] boot-to-battle mode: stage={os.environ['FM2K_BTB_STAGE']} "
              f"game={GAME_EXE.name}")
    else:
        # CSS auto-confirm — both peers pick char 0/color 0, stage 0, press
        # confirm. Must be IDENTICAL on both peers for the gekko CSS-delay
        # session to converge.
        common_env["FM2K_TEST_AUTO_CSS"] = "0,0,0,0,0"
    # Forward rollback-shaping env from the calling shell. Loopback at the
    # auto-negotiated delay produces ZERO rollbacks (rb_total=0 -- verified
    # 2026-06-11), so the recorder's under-rollback behavior goes untested
    # by default. FM2K_LOCAL_DELAY=0 forces real prediction misses (even
    # 0ms-ping scheduling jitter makes remote inputs late) -> genuine
    # rollbacks through the genuine path.
    for k in ("FM2K_LOCAL_DELAY", "FM2K_PRED_WINDOW", "FM2K_PREDICTION_WINDOW", "FM2K_RUNAHEAD",
              "FM2K_RENDER_STALL_US", "FM2K_NO_NETPLAY_CATCHUP", "FM2K_PERF_PROFILE",
              "FM2K_RENDER_PROFILE", "FM2K_DUMP_STAGES",
              "FM2K_DEV_MODE", "FM2K_BTB_P1_CHAR", "FM2K_BTB_P2_CHAR",
              "FM2K_RING_TRACE", "FM2K_CAM_DIAG", "FM2K_CSM_DIAG",
              "FM2K_STAGE_RANDOM_SEED", "FM2K_STAGE_RANDOM_MIN",
              "FM2K_STAGE_RANDOM_MAX", "FM2K_TEST_CSS_CHAR",
              "FM2K_RACE_DETECT"):
        if os.environ.get(k):
            common_env[k] = os.environ[k]
    # When a specific character mirror is requested, the cursor must NOT
    # confirm at the grid origin -- let CssAutoConfirm (armed by
    # FM2K_TEST_CSS_CHAR at CSS sync) drive the pick. The TEST_AUTO_CSS
    # pulse still provides the gekko confirm edge.
    if os.environ.get("FM2K_TEST_CSS_CHAR"):
        print(f"[harness] char mirror: FM2K_TEST_CSS_CHAR="
              f"{os.environ['FM2K_TEST_CSS_CHAR']}  game={GAME_EXE.name}")

    record_path_p1 = OUT_DIR / "p1_parity.pty"
    record_path_p2 = OUT_DIR / "p2_parity.pty"
    for f in (record_path_p1, record_path_p2):
        if f.exists(): f.unlink()

    start_ts = time.time()

    # ---- Relay-floor mode setup (Phase 0 NAT plan) ----
    # When --relay is set we stand up a local hub-relay data plane and force
    # BOTH peers through it: each peer's remote points at a TEST-NET-1
    # blackhole, so the direct hole-punch fails on both sides, and after the
    # ~2s post-burst grace the hook engages relay mode and routes all
    # gameplay (HELLO + GekkoNet) through the relay endpoint. This exercises
    # exactly the production relay floor, not a forced shortcut.
    relay_proc = None
    relay_token = None
    relay_env = {}
    if args.relay:
        # 32-hex match token. The SAME hex feeds both FM2K_HUB_MATCH_TOKEN
        # (so the hook's Netplay_Init runs StartPunch, whose post-burst
        # worker engages relay on failure) and FM2K_HUB_RELAY_SESSION (the
        # 0xCF envelope session id). The relay stub pre-registers the same
        # token so route() can pair the two peers. make_relay_session in the
        # hub and the stub derive the 16-byte id identically (hex -> bytes).
        relay_token = secrets.token_hex(16)  # 32 hex chars == 16 bytes
        relay_addr = f"127.0.0.1:{args.relay_port}"
        relay_env = {
            "FM2K_HUB_RELAY_ADDR":   relay_addr,
            "FM2K_HUB_RELAY_SESSION": relay_token,
            "FM2K_HUB_MATCH_TOKEN":  relay_token,
        }
        print(f"[harness] RELAY mode: stub on {relay_addr} "
              f"session={relay_token}")
        print(f"[harness]   both peers point remote at blackhole "
              f"{RELAY_BLACKHOLE_IP} so direct punch fails -> relay engages")
        # Start the relay stub (hub data-plane equivalent: 0xCF route + 0xCD
        # STUN ack). We prefer the real hub.py with --test-relay-session, but
        # hub.py also stands up WS/HTTP/auth + 4 ports; the stub is the
        # hermetic, dependency-free path and freezes the exact wire behavior
        # (copied from hub.py:1826-1878). Where `websockets` is installed,
        # re-verify this same scenario against the real hub.py at least once.
        #
        # CRITICAL: launch the stub as a WINDOWS process (python.exe). The
        # game is a Windows process; under WSL2 "nat" networking the Linux
        # and Windows loopbacks are separate stacks, so a WSL-bound stub at
        # 127.0.0.1:7712 is invisible to the game. Running the stub on
        # Windows python binds the SAME loopback the game's relay sendto
        # targets. Falls back to the WSL interpreter only if no Windows
        # python is found (test will then fail loudly with zero routes).
        relay_log = OUT_DIR / "relay_stub.log"
        if relay_log.exists(): relay_log.unlink()
        win_py = find_windows_python()
        relay_log_f = None
        if win_py:
            # Launch the Windows python DIRECTLY (relay_proc IS the python.exe
            # so terminate() reaps it). The stub owns its own --logfile write
            # because a WSL stdout pipe does not reliably capture a Windows
            # child's output; both sides read the same physical file on C:.
            stub_cmd = [win_py, "-u", to_win(RELAY_STUB),
                        "--host", "127.0.0.1", "--port", str(args.relay_port),
                        "--session", relay_token, "--verbose",
                        "--logfile", to_win(relay_log)]
            print(f"[harness]   relay stub via Windows python: {win_py}")
            relay_proc = subprocess.Popen(stub_cmd,
                                          stdout=subprocess.DEVNULL,
                                          stderr=subprocess.DEVNULL)
        else:
            # No Windows python -- fall back to WSL python. Under WSL2 nat
            # networking the game likely cannot reach it; the test will fail
            # loudly (zero routes / relay timeout) rather than silently.
            print("[harness]   WARNING: no Windows python found; running stub "
                  "under WSL python -- under WSL2 nat networking the game may "
                  "not reach it (expect zero routes / relay timeout).")
            relay_log_f = open(relay_log, "w")
            relay_proc = subprocess.Popen(
                [sys.executable, str(RELAY_STUB),
                 "--host", "127.0.0.1", "--port", str(args.relay_port),
                 "--session", relay_token, "--verbose"],
                stdout=relay_log_f, stderr=subprocess.STDOUT)
        # Wait for the stub's READY line so neither peer punches before the
        # relay socket is bound. Short bounded poll on the log file.
        ready_deadline = time.time() + 5.0
        while time.time() < ready_deadline:
            if relay_proc.poll() is not None:
                print(f"[harness] FAIL: relay stub exited early "
                      f"rc={relay_proc.returncode} (see {relay_log})")
                if relay_log_f is not None: relay_log_f.close()
                return 1
            try:
                if "[relay-stub] READY" in relay_log.read_text():
                    break
            except OSError:
                pass
            time.sleep(0.1)
        print(f"[harness]   relay stub ready (log: {relay_log})")

    # One-way proxy: sits between the peers and kills exactly one leg. Must
    # be a Windows process for the same reason the relay stub is (WSL2 NAT
    # puts a WSL-bound socket on a stack the game cannot reach at
    # 127.0.0.1). Started AFTER the stub so both are up before either peer.
    oneway_proc = None
    oneway_log = OUT_DIR / "oneway_proxy.log"
    if args.oneway:
        if oneway_log.exists(): oneway_log.unlink()
        win_py = find_windows_python()
        if not win_py:
            print("[harness] FAIL: --oneway needs a Windows python for the "
                  "proxy (same constraint as the relay stub)")
            return 1
        oneway_proc = subprocess.Popen(
            [win_py, to_win(ONEWAY_PROXY),
             "--host", "127.0.0.1", "--port", str(ONEWAY_PORT),
             "--forward-to", f"127.0.0.1:{P1_PORT}",
             "--logfile", to_win(oneway_log)],
            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        for _ in range(50):
            if oneway_proc.poll() is not None:
                print(f"[harness] FAIL: one-way proxy exited early "
                      f"rc={oneway_proc.returncode} (see {oneway_log})")
                return 1
            try:
                if "[oneway] READY" in oneway_log.read_text():
                    break
            except OSError:
                pass
            time.sleep(0.1)
        print(f"[harness] ONEWAY: proxy on 127.0.0.1:{ONEWAY_PORT} "
              f"forwards -> P1({P1_PORT}), drops P1 -> P2")

    # Launch P1 (host) and P2 (joiner) concurrently. Loopback 127.0.0.1.
    # P1 = --host with FM2K_LOCAL_PORT=7000, FM2K_REMOTE_ADDR=127.0.0.1:7001
    # P2 = --connect with FM2K_LOCAL_PORT=7001, FM2K_REMOTE_ADDR=127.0.0.1:7000
    # In --relay mode the remotes below are overridden to a blackhole (via
    # --remote for the host and the --connect arg for the guest); the env
    # FM2K_REMOTE_ADDR here is then ignored because StartOnlineSession
    # re-derives it from the CLI args.
    p1_env = {
        **common_env,
        **relay_env,
        "FM2K_LOCAL_PORT": str(P1_PORT),
        "FM2K_REMOTE_ADDR": f"127.0.0.1:{P2_PORT}",
        "FM2K_PARITY_RECORD_PATH": to_win(record_path_p1),
    }
    p2_env = {
        **common_env,
        **relay_env,
        "FM2K_LOCAL_PORT": str(P2_PORT),
        "FM2K_REMOTE_ADDR": f"127.0.0.1:{P1_PORT}",
        "FM2K_PARITY_RECORD_PATH": to_win(record_path_p2),
    }

    p1_log = OUT_DIR / "p1.log"
    p2_log = OUT_DIR / "p2.log"

    print("[harness] launching P1 (host) + P2 (join) in loopback")
    # Spawn both in parallel — host first to start listening.
    # --host <game-path> and --connect <addr> <game-path> are the new
    # direct-path bypasses (mirrors --stress's convention). Without the
    # path, the launcher picks the first discovered game in its registry
    # which is whichever 2DFM title ends up alphabetically first — NOT
    # necessarily wanwan.
    p1_args = [str(LAUNCHER), "--host", game_arg, "--port", str(P1_PORT)]
    p2_args = [str(LAUNCHER), "--connect", f"127.0.0.1:{P1_PORT}", game_arg,
               "--port", str(P2_PORT)]
    if args.relay:
        # Point BOTH peers at the blackhole so direct punch fails on both.
        # Host needs --remote (it otherwise listens-and-learns and never
        # runs StartPunch); guest's --connect target IS its remote, so we
        # just retarget it. The relay envelope then carries everything to
        # the actual peer via the stub. Distinct blackhole ports are
        # cosmetic (the IP is unreachable regardless) but make the punch
        # targets distinguishable in the logs.
        p1_args = [str(LAUNCHER), "--host", game_arg, "--port", str(P1_PORT),
                   "--remote", f"{RELAY_BLACKHOLE_IP}:{P2_PORT}"]
        p2_args = [str(LAUNCHER), "--connect", f"{RELAY_BLACKHOLE_IP}:{P1_PORT}",
                   game_arg, "--port", str(P2_PORT)]
    if args.oneway:
        # Both remotes aim at the proxy. P1's punches die there; P2's are
        # forwarded to P1 FROM the proxy socket, so when P1 latches the
        # source it latches the black hole -- the field failure exactly.
        # This overrides the --relay blackhole args set just above.
        p1_args = [str(LAUNCHER), "--host", game_arg, "--port", str(P1_PORT),
                   "--remote", f"127.0.0.1:{ONEWAY_PORT}"]
        p2_args = [str(LAUNCHER), "--connect", f"127.0.0.1:{ONEWAY_PORT}",
                   game_arg, "--port", str(P2_PORT)]

    # done_when: wait for the harness terminator inside the hook to flush
    # the .fm2krep slice. parity.pty's first byte appears at frame 1 — way
    # before MATCH_END — so gating on .pty size kills the run before the
    # actual match completes. Instead, wait for a new .fm2krep file
    # newer than the harness start timestamp.
    def has_new_replay(): return find_latest_fm2krep(game_dir, start_ts) is not None
    def p1_done(): return has_new_replay()
    def p2_done(): return has_new_replay()

    # Background-launch P1, then P2, wait for both.
    import threading
    rc1 = [None]; rc2 = [None]
    t1 = threading.Thread(target=lambda: rc1.__setitem__(0,
        launch("P1", p1_args, p1_env, p1_log, args.record_timeout, p1_done)))
    t2 = threading.Thread(target=lambda: rc2.__setitem__(0,
        launch("P2", p2_args, p2_env, p2_log, args.record_timeout, p2_done)))
    t1.start()
    time.sleep(1.0)  # let host bind first
    t2.start()
    t1.join(); t2.join()
    print(f"[harness] P1 rc={rc1[0]} P2 rc={rc2[0]}")

    # ---- Relay-floor verification ----
    # The peers each write FM2K_P<n>_Debug.log next to the game exe. Confirm
    # both engaged relay AND completed the handshake through it. We check the
    # exact hook strings: "relay mode ENGAGED" (nat_traversal.cpp) and "Full
    # handshake complete - CONNECTED!" (netplay.cpp). The stdout cmd.exe logs
    # (p1_log/p2_log) don't carry the SDL_Log lines, so read the game-side
    # debug logs. Tear the relay stub down regardless of outcome.
    relay_ok = True
    if args.relay:
        if relay_proc is not None:
            relay_proc.terminate()
            try: relay_proc.wait(timeout=3)
            except subprocess.TimeoutExpired: relay_proc.kill()
        def _peer_log(idx: int) -> str:
            # Hook writes logs\FM2K_P<idx+1>_Debug.log relative to the game's
            # CWD (Fm2k_BuildLogPath -> "logs\\<name>"). Fall back to the game
            # dir root in case the logs subdir path ever changes. Tolerate
            # absence / unreadable.
            for p in (game_dir / "logs" / f"FM2K_P{idx+1}_Debug.log",
                      game_dir / f"FM2K_P{idx+1}_Debug.log"):
                try:
                    if p.exists():
                        return p.read_text(errors="ignore")
                except OSError:
                    pass
            return ""
        for idx, label in ((0, "P1"), (1, "P2")):
            txt = _peer_log(idx)
            engaged   = "relay mode ENGAGED" in txt
            connected = "Full handshake complete - CONNECTED!" in txt
            print(f"[harness] RELAY {label}: ENGAGED={engaged} "
                  f"CONNECTED={connected}")
            if not (engaged and connected):
                relay_ok = False
        if relay_ok:
            print("[harness] RELAY: both peers ENGAGED relay + CONNECTED -- PASS")
        else:
            print("[harness] RELAY: FAIL -- a peer did not engage relay or did "
                  "not complete the handshake through it (see "
                  "FM2K_P{1,2}_Debug.log)")

        # ---- One-way verdict ----
        # The relay check above is necessary but NOT sufficient here: P2
        # engages relay in this scenario even on a broken build (its punch
        # gate expires normally), so "both engaged" alone could pass for the
        # wrong reason. What the fix owns is P1 -- the side that latched a
        # working-looking inbound punch. Require the specific reason string,
        # so a P1 that engaged via some other path does not score as a pass.
        if args.oneway:
            if oneway_proc is not None:
                oneway_proc.terminate()
                try: oneway_proc.wait(timeout=3)
                except subprocess.TimeoutExpired: oneway_proc.kill()
            p1_txt = _peer_log(0)
            latched   = "authenticated -- peer latched" in p1_txt
            oneway_hit = "one-way path" in p1_txt
            # Prove the dead leg was actually dead. Without this a proxy that
            # silently forwarded both ways would still "pass" -- the peers
            # would just connect directly and nobody would notice the test
            # stopped testing anything.
            drops = 0
            try:
                for line in oneway_log.read_text(errors="ignore").splitlines():
                    if line.startswith("[oneway] forwarded="):
                        drops = max(drops, int(line.rsplit("dropped=", 1)[1]))
            except (OSError, ValueError, IndexError):
                pass
            print(f"[harness] ONEWAY P1: LATCHED={latched} "
                  f"ENGAGED_ONEWAY={oneway_hit} P1_SENDS_DROPPED={drops}")
            if not latched:
                print("[harness] ONEWAY: INVALID -- P1 never latched the "
                      "proxy, so the asymmetric case was never entered "
                      "(this is a rig failure, not a product pass)")
                relay_ok = False
            elif drops == 0:
                print("[harness] ONEWAY: INVALID -- proxy dropped nothing, "
                      "so the path was not one-way (rig failure)")
                relay_ok = False
            elif not oneway_hit:
                print("[harness] ONEWAY: FAIL -- P1 latched a one-way path "
                      "and never re-opened the relay decision. This is the "
                      "field deadlock.")
                relay_ok = False
            else:
                print("[harness] ONEWAY: P1 detected the one-way path and "
                      "engaged relay -- PASS")

    # Find P1's .fm2krep (host is canonical recorder; the hook writes
    # per-player *_p<idx>_harness.fm2krep so P2's slice can't clobber it).
    # Retry for a few seconds: the file lands milliseconds before the
    # launcher exits and WSL's drvfs metadata cache can lag the Windows
    #-side write, so an immediate single check raced it and reported
    # "no .fm2krep" even though the file was on disk.
    p1_rep = None
    deadline = time.time() + 10.0
    while time.time() < deadline:
        p1_rep = (find_latest_fm2krep(game_dir, start_ts, suffix="_p0_harness")
                  or find_latest_fm2krep(game_dir, start_ts))
        if p1_rep:
            break
        time.sleep(0.5)
    if not p1_rep:
        print("[harness] FAIL: no .fm2krep found from netplay run")
        return 1
    print(f"[harness] P1 .fm2krep: {p1_rep}")

    # Replay phase
    print("[harness] launching REPLAY of P1's .fm2krep")
    rep_env = {
        "FM2K_PARITY_RECORD_PATH": to_win(replay_pty),
        "FM2K_AUTO_TERMINATE_AT_FRAME": str(args.frames + 20),
    }
    rep_args = [str(LAUNCHER), "--replay", to_win(p1_rep)]
    # Replay-side completion: in --replay mode there is no GekkoNet session,
    # so FM2K_AUTO_TERMINATE_AT_FRAME never fires -- the game instead
    # ExitProcess(0)s when the pb_queue drains. The old predicate
    # (size > 100) tripped on the FIRST stdio flush and killed the replay
    # a few frames in, collapsing the diff window. Instead wait for the
    # .pty to go quiescent: size unchanged for 6s after real content
    # appeared means the drain finished and the game exited.
    rep_state = {"size": -1, "since": 0.0}
    def rep_done():
        try:
            sz = replay_pty.stat().st_size
        except OSError:
            return False
        now = time.time()
        if sz != rep_state["size"]:
            rep_state["size"] = sz
            rep_state["since"] = now
            return False
        return sz > 32 + 260 * 10 and (now - rep_state["since"]) >= 6.0
    rep_rc = launch("REPLAY", rep_args, rep_env, OUT_DIR / "replay.log",
                    args.replay_timeout, rep_done)
    print(f"[harness] replay rc={rep_rc}")

    if not record_path_p1.exists():
        print(f"[harness] FAIL: P1 parity {record_path_p1} missing")
        return 1
    if not replay_pty.exists():
        print(f"[harness] FAIL: replay parity {replay_pty} missing")
        return 1

    # ---- PRIMARY desync gate: gekko's per-frame checksum (the live cross-peer
    # truth). Real divergence between the two PLAYING peers logs "DESYNC #" in
    # the per-player debug logs. The phantom zero-checksum guard logs "DESYNC
    # ignored (phantom" -- a gekko health-slot artifact, NOT a desync. This is
    # the authoritative "did the clients desync" signal; the parity diff below
    # is a secondary replay-FIDELITY check (and is rollback-robust, so it only
    # fails on a PERSISTENT divergence, never on transient speculative residue).
    def count_live_desyncs():
        total = 0
        for idx in (1, 2):
            for p in (game_dir / "logs" / f"FM2K_P{idx}_Debug.log",
                      game_dir / f"FM2K_P{idx}_Debug.log"):
                try:
                    if p.exists() and p.stat().st_mtime >= start_ts:
                        txt = p.read_text(errors="ignore")
                        total += sum(1 for ln in txt.splitlines()
                                     if "DESYNC #" in ln and "phantom" not in ln)
                        break
                except OSError:
                    pass
        return total
    live_desyncs = count_live_desyncs()
    print(f"[harness] LIVE desync gate (gekko DESYNC#, this run): {live_desyncs}")

    # Diff -- replay fidelity (rollback-robust, reconvergence-aware).
    print("[harness] diffing P1 parity vs replay parity")
    diff_rc = subprocess.call([sys.executable, str(PARITY_DIFF),
                               str(record_path_p1), str(replay_pty)])
    # In relay mode the run only PASSES if BOTH the parity diff is clean AND
    # both peers provably went through the relay (ENGAGED + CONNECTED). A
    # clean diff with no relay engagement would be a false green (the peers
    # might have connected directly), so fold relay_ok into the exit code.
    if args.relay and not relay_ok:
        print("[harness] OVERALL FAIL: relay engagement check failed "
              f"(parity diff_rc={diff_rc})")
        return 1
    if live_desyncs > 0:
        print(f"[harness] OVERALL FAIL: {live_desyncs} real live desync(s) detected "
              f"by gekko (DESYNC#) -- the playing peers diverged.")
        return 1
    if diff_rc != 0:
        print("[harness] OVERALL FAIL: persistent replay-fidelity divergence "
              "(see parity diff above).")
        return 1
    print("[harness] OVERALL PASS: no live desync (gekko) and replay is faithful.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
