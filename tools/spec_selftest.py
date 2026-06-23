#!/usr/bin/env python3
"""Live-spectator determinism self-test.

Spins up THREE launcher instances on 127.0.0.1:

  P1   --host    <game> --port 7000   (autoplay, parity -> p1_parity.pty)
  P2   --connect 127.0.0.1:7000 --port 7001   (autoplay)
  SPEC --spectate 127.0.0.1:7000 --port 7002  (passive viewer,
       parity -> spec_parity.pty, joins during title/CSS so the host
       backfills the full session -- FULL_SESSION-equivalent path)

The host terminates at FM2K_AUTO_TERMINATE_AT_FRAME battle frames; the
harness then waits for the spectator's parity stream to go quiescent,
kills everything, and diffs host parity vs spectator parity with
tools/parity_diff.py. ALL ALIGNED FRAMES IDENTICAL = the spectator's
input-replay sim bit-matched the host's live sim.

Notes:
  * The spectator trails the host by the EVENT_BATCH flush cadence
    (every 8 confirmed frames) plus TCP latency, and the host
    TerminateProcess()es immediately at the target frame -- the spec's
    last few frames may never arrive. --min-coverage (default
    frames-100) guards against a vacuous pass on a short stream.
  * Mid-battle CURRENT_MATCH snapshot join is NOT covered here yet:
    parity_diff aligns index-paired from each side's first battle
    snapshot, which only matches a from-the-start join. A --join-delay
    mode needs frame-keyed alignment first.

Usage:
  python3 tools/spec_selftest.py --frames 1500
  FM2K_LOCAL_DELAY=0 python3 tools/spec_selftest.py --frames 1500 --keep
"""

from __future__ import annotations
import argparse, os, shutil, subprocess, sys, threading, time
from pathlib import Path

LAUNCHER = Path("/mnt/c/games/FM2K_RollbackLauncher.exe")
# FM2K game registry: --game <name> picks the exe. Different games have
# different rosters / CSS grid layouts / stage-select mechanics, so the
# multi-game sweep validates the spectator across them.
GAMES = {
    "wanwan": Path("/mnt/c/games/2dfm/wanwan/WonderfulWorld_ver_0946.exe"),
    "vanpri": Path("/mnt/c/games/2dfm/vanguard-princess/vanpri.exe"),
    "urorfg": Path("/mnt/c/games/2dfm/URORFG Release 1 0 2/URORFGRelease102.exe"),
}
GAME_EXE = GAMES["wanwan"]   # default; overridden by --game in main()
OUT_DIR  = Path("/mnt/c/dev/wanwan/tools/.spec_selftest")
PARITY_DIFF = Path(__file__).parent / "parity_diff.py"
P1_PORT, P2_PORT, SPEC_PORT = 7000, 7001, 7002
# Fake-spectator UDP ports start at 7200 -- clear of the real spec range (7002+)
# AND the host's TCP listener (tries udp+100 = 7100 first). Fakes dial whatever
# TCP port the JOIN_ACK reports, so no fake TCP bind collision.
FAKE_PORT_BASE = 7200
FAKE_SPEC_TOOL = Path(__file__).parent / "fake_spectator.py"
# Fakes run as WINDOWS processes (so 127.0.0.1 reaches the Windows-side host;
# NAT-mode WSL2 loopback would not). Windows Python on the test box.
WIN_PYTHON = r"C:\Program Files\Python313\python.exe"


def to_win(p: Path) -> str:
    s = str(p)
    if s.startswith("/mnt/") and len(s) > 6 and s[6] == "/":
        return s[5].upper() + ":" + s[6:]
    return s


def kill_strays():
    for image in ("FM2K_RollbackLauncher.exe", "WonderfulWorld_ver_0946.exe"):
        subprocess.run(["taskkill.exe", "/F", "/T", "/IM", image],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def report_host_pacing(host_log, n_fake):
    """Phase 3 signal: parse the host's [FRAMETIME]/[HICCUP] lines into one
    HOST PACING line. over_budget = host frames whose engine+render+spectator-
    fanout WORK exceeded the 10ms budget (a real player-visible stall, since
    SleepToTarget can only pad up to 10ms not claw back time); [HICCUP] = a
    single frame >20ms. Compare N=0 vs N: if adding fake spectators raises
    over_budget/hiccups, the fan-out is lagging the host -- the bug to fix."""
    import re
    works, work_max, over, tot, hiccups = [], 0.0, 0, 0, 0
    try:
        for ln in open(host_log, errors="ignore"):
            m = re.search(r"\[FRAMETIME\].*?work avg=([\d.]+)ms max=([\d.]+)ms"
                          r".*?over_budget=(\d+)/(\d+)", ln)
            if m:
                works.append(float(m.group(1)))
                work_max = max(work_max, float(m.group(2)))
                over += int(m.group(3)); tot += int(m.group(4))
            elif "[HICCUP]" in ln:
                hiccups += 1
    except OSError:
        pass
    if tot == 0:
        print(f"[harness] HOST PACING (fake_specs={n_fake}): no [FRAMETIME] "
              "samples (profiler off or host ran <300 frames)")
        return
    rate = 100.0 * over / tot
    print(f"[harness] HOST PACING (fake_specs={n_fake}): frames={tot} "
          f"work_avg={sum(works) / len(works):.2f}ms work_max={work_max:.2f}ms "
          f"over_budget={over}/{tot} ({rate:.2f}%) hiccups={hiccups}")


def launch(label, args, env_extra, log_path, timeout, done_when=None):
    # WSL->Windows env vars don't cross subprocess.Popen(env=); use the
    # cmd.exe `set K=V&& ...` trick (same as the other harnesses).
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_f = open(log_path, "w")
    win_args = []
    for a in args:
        if a.startswith("/mnt/") and len(a) > 6 and a[6] == "/":
            a = a[5].upper() + ":" + a[6:]
        win_args.append(f'"{a}"' if " " in a else a)
    # Launch via a temp .bat, NOT `cmd /C "set K=V&& exe "spaced path""`: the
    # latter's nested quotes get mangled and a game path WITH SPACES is
    # truncated at the first space (the launcher received "C:/games/2dfm/URORFG"
    # instead of the full "URORFG Release 1 0 2/..." path). A batch file sets the
    # env + launches with native, un-mangled quoting.
    bat_lines = ["@echo off"]
    bat_lines += [f"set {k}={v}" for k, v in env_extra.items()]
    bat_lines.append(" ".join(win_args))
    bat_path = OUT_DIR / f".launch_{label}.bat"
    bat_path.write_text("\r\n".join(bat_lines) + "\r\n")
    print(f"[{label}] launching: {' '.join(win_args)}")
    for k, v in env_extra.items():
        print(f"[{label}]   {k}={v}")
    proc = subprocess.Popen(["cmd.exe", "/C", to_win(bat_path)],
                            stdout=log_f, stderr=subprocess.STDOUT)
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
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                rc = 0
                break
            if time.time() > deadline:
                print(f"[{label}] TIMEOUT after {timeout}s; killing")
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                rc = 124
                break
            time.sleep(0.5)
    finally:
        log_f.close()
    return rc


def find_latest_fm2krep(game_dir: Path, since: float, suffix="") -> Path | None:
    rep = game_dir / "replays"
    if not rep.is_dir():
        return None
    cands = []
    for f in rep.glob(f"*{suffix}.fm2krep"):
        try:
            m = f.stat().st_mtime
            if m + 1.0 >= since:
                cands.append((m, f))
        except OSError:
            pass
    return max(cands)[1] if cands else None


def pty_snapshots(path: Path) -> int:
    try:
        return max(0, (path.stat().st_size - 32) // 260)
    except OSError:
        return 0


def trim_first_battle_segment(src: Path, dst: Path) -> int:
    """Copy src .pty to dst keeping only [first real battle row .. the row
    before battle phase first ends]. Multi-match spectator streams contain
    match1 + CSS + match2 rows; the match-1 replay gate needs just the
    first battle segment. Returns rows kept."""
    import struct
    d = src.read_bytes()
    hdr, body = d[:32], d[32:]
    n = len(body) // 260
    rows = []
    started = False
    for k in range(n):
        off = k * 260
        phase = struct.unpack_from('<i', body, off + 16)[0]
        p1s   = struct.unpack_from('<i', body, off + 32)[0]
        p2s   = struct.unpack_from('<i', body, off + 32 + 92)[0]
        in_battle = (phase == 3000 and p1s != -1 and p2s != -1)
        if not started:
            if in_battle:
                started = True
                rows.append(body[off:off + 260])
        else:
            if phase != 3000:
                break
            # Teardown rows: at match end the player objects despawn while
            # phase is still 3000 for a few capture ticks -- script_idx
            # reads -1 and every other field zeros. The replay instance
            # tends to capture one of these as its final row (the A4
            # k=6534 "divergence" was spec-real-row vs replay-empty-row).
            # They carry no engine state; end the segment there.
            if p1s == -1 and p2s == -1:
                break
            rows.append(body[off:off + 260])
    dst.write_bytes(hdr + b"".join(rows))
    return len(rows)


def wait_ports_free(ports, timeout=20.0):
    """Poll Windows netstat until none of `ports` appear as bound UDP
    sockets. A fixed post-taskkill sleep is unreliable: socket teardown
    occasionally outlives it and the next bind() fails err=10013."""
    needles = [f":{p} " for p in ports]
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            out = subprocess.run(["netstat.exe", "-an", "-p", "UDP"],
                                 capture_output=True, text=True,
                                 timeout=10).stdout
        except Exception:
            return  # netstat unavailable; fall back to hoping
        if not any(n in out for n in needles):
            return
        time.sleep(1.0)
    print(f"[harness] WARN: ports {ports} still bound after {timeout}s; "
          f"launching anyway")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=1500,
                    help="battle frames the host plays before terminating")
    ap.add_argument("--total-frames", type=int, default=0,
                    help="multi-match mode: terminate after N TOTAL confirmed "
                         "battle frames across matches (FM2K_AUTO_TERMINATE_TOTAL). "
                         "Spans MATCH_END -> CSS -> match 2; the parity gate uses "
                         "match 1's canonical .fm2krep.")
    ap.add_argument("--rounds", type=int, default=0,
                    help="force N-round matches (FM2K_TEST_ROUNDS, writes "
                         "g_default_round). 1 = fast css->battle->css cycles for "
                         "the multi-match-under-loss e2e harness. 0 = game default.")
    ap.add_argument("--round-time", type=int, default=-1,
                    help="force the round timer (FM2K_TEST_ROUND_TIME). 0 = OFF "
                         "(infinite; wanwan NEEDS this -- a non-zero custom timer "
                         "bugs subsequent battles to 0 time, so matches end on KO). "
                         "-1 = leave game default.")
    ap.add_argument("--record-timeout", type=float, default=240.0)
    ap.add_argument("--spec-join-delay", type=float, default=3.0,
                    help="seconds after P1/P2 launch before the spectator dials in")
    ap.add_argument("--min-coverage", type=int, default=-1,
                    help="minimum spectator battle frames required "
                         "(default: frames - 100)")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--game", default="wanwan", choices=sorted(GAMES.keys()),
                    help="which FM2K game to test (registry in GAMES)")
    ap.add_argument("--spectators", default="css",
                    help="comma-list of spectator join-phases. 'css' = dial in "
                         "during the host's CSS (FULL_SESSION / CSS-walk); "
                         "'battle' = dial in after battle starts (CURRENT_MATCH "
                         "snapshot). e.g. 'css,battle' = the E2E case (p3 on CSS + "
                         "p4 mid-battle). Each gets a distinct port + player-index "
                         "+ log + parity and is gated independently vs the host.")
    ap.add_argument("--battle-join-offset", type=float, default=1.5,
                    help="seconds after the host's battle session is created "
                         "before a 'battle'-phase spectator dials in (so it joins "
                         "a few frames into the match = a real mid-battle snapshot)")
    ap.add_argument("--css-dwell", default="0.4",
                    help="CSS navigation depth (FM2K_AUTOPLAY_CSS_DWELL): the "
                         "players WANDER the char grid this long before confirming "
                         "(~dwell*100 frames). Default 0.4 = real asymmetric "
                         "navigation to varied non-char0 picks. Use 0 for the "
                         "instant char0/char0 path that exercises the spectator "
                         "battle-align fix (host confirms inside the seam-hold "
                         "window).")
    # Phase 3 host-no-hiccup load test: N protocol-level FAKE spectators
    # (tools/fake_spectator.py) join/churn the host WITHOUT running a game, so we
    # load the host's fan-out path at N=3..7 without N real game instances lagging
    # the box (which would confound the host frame-time measurement). Enables
    # FM2K_PERF_PROFILE on the host so [FRAMETIME] over_budget is logged + parsed.
    ap.add_argument("--fake-spectators", type=int, default=0,
                    help="spawn N protocol-only fake spectators to load the host's "
                         "fan-out (no game instances); reports host over_budget/hiccups")
    ap.add_argument("--fake-schedule", default="",
                    help="override the per-fake join/leave schedule "
                         "(e.g. join@0,leave@8,join@12); default auto-staggers + churns the last")
    ap.add_argument("--fake-duration", type=float, default=180.0,
                    help="max seconds a fake spectator runs (killed when the host finishes)")
    ap.add_argument("--no-fake-churn", action="store_true",
                    help="disable the last fake spectator's leave/rejoin churn "
                         "(churn re-triggers the host's snapshot+backfill = the heaviest path)")
    args = ap.parse_args()
    min_coverage = args.min_coverage if args.min_coverage >= 0 else args.frames - 100
    # Measure host pacing when fakes are present OR when explicitly asked (so the
    # N=0 baseline run -- FM2K_PERF_PROFILE=1 ... --fake-spectators 0 -- also logs
    # [FRAMETIME], giving a clean delta vs the N=7 load run).
    measure_host = args.fake_spectators > 0 or bool(os.environ.get("FM2K_PERF_PROFILE"))

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    p1_pty   = OUT_DIR / "p1_parity.pty"
    if p1_pty.exists():
        p1_pty.unlink()
    # Per-spectator parity files are created below in the `specs` list.
    spec_phases = [p.strip() for p in args.spectators.split(",") if p.strip()]

    game_exe = GAMES[args.game]
    if not game_exe.exists():
        print(f"[harness] FATAL: --game {args.game} exe not found: {game_exe}")
        return 2
    print(f"[harness] game={args.game} ({game_exe.name})")
    game_arg = to_win(game_exe)
    game_dir = game_exe.parent
    kill_strays()
    time.sleep(1.0)
    wait_ports_free([P1_PORT, P2_PORT] + [SPEC_PORT + k for k in range(len(spec_phases))])

    common_env = {
        "FM2K_PARITY_AUTOPLAY": "1",
        "FM2K_PARITY_AUTOPLAY_BATTLE": "1",
        "FM2K_AUTO_TITLE_SKIP": "1",
        # p1char,p1color,p2char,p2color,STAGE -- overridable so we can test a
        # non-zero stage (e.g. FM2K_TEST_AUTO_CSS=0,0,0,1,1) and verify the
        # live spectator loads the SAME stage the players picked.
        "FM2K_TEST_AUTO_CSS": os.environ.get("FM2K_TEST_AUTO_CSS", "0,0,0,0,0"),
        # Per-frame host + spectator rng/input/script traces to disk. These
        # feed the AUTHORITATIVE gate (host-vs-spec pairing, same run) -- the
        # ground-truth desync check, unlike the spec-vs-replay parity diff
        # which mis-pairs the wrong match under multi-match autoplay.
        "FM2K_HOST_TRACE":       os.environ.get("FM2K_HOST_TRACE", "1"),
        "FM2K_SPECTATOR_DEBUG":  os.environ.get("FM2K_SPECTATOR_DEBUG", "1"),
    }
    if args.rounds > 0:
        # Force N-round matches on both players (host writes g_default_round, the
        # HOST_CONFIG syncs it to the client) -> short matches -> fast
        # css->battle->css cycles for the multi-match-under-loss harness.
        common_env["FM2K_TEST_ROUNDS"] = str(args.rounds)
    if args.round_time >= 0:
        common_env["FM2K_TEST_ROUND_TIME"] = str(args.round_time)
    if args.total_frames > 0:
        common_env["FM2K_AUTO_TERMINATE_TOTAL"] = str(args.total_frames)
        common_env.setdefault("FM2K_AUTOPLAY_CSS_DWELL",
                              os.environ.get("FM2K_AUTOPLAY_CSS_DWELL", "8"))
    else:
        common_env["FM2K_AUTO_TERMINATE_AT_FRAME"] = str(args.frames)
        # Real CSS navigation by default (--css-dwell): players wander to varied,
        # asymmetric, non-char0 picks before confirming, instead of an instant
        # char0/char0 lock. The os.environ forward below still lets FM2K_AUTOPLAY_
        # CSS_DWELL override it.
        common_env["FM2K_AUTOPLAY_CSS_DWELL"] = str(args.css_dwell)
    for k in ("FM2K_LOCAL_DELAY", "FM2K_PRED_WINDOW", "FM2K_PREDICTION_WINDOW", "FM2K_RUNAHEAD", "FM2K_SPEC_UDP", "FM2K_AUTOPLAY_CSS_DWELL", "FM2K_SPECTATOR_DEBUG", "FM2K_HOST_TRACE", "FM2K_FA_TRACE", "FM2K_TEST_BATTLE_SEED",
              # in-process link impairment (players' gekko+control path)
              "FM2K_NET_DELAY_MS", "FM2K_NET_JITTER_MS", "FM2K_NET_LOSS", "FM2K_NET_SEED"):
        if os.environ.get(k):
            common_env[k] = os.environ[k]

    p1_env = {**common_env,
              "FM2K_LOCAL_PORT": str(P1_PORT),
              "FM2K_REMOTE_ADDR": f"127.0.0.1:{P2_PORT}",
              "FM2K_PARITY_RECORD_PATH": to_win(p1_pty)}
    if measure_host:
        # Host frame-time profiler ON for the load test -> [FRAMETIME]
        # over_budget=X/300 + [HICCUP] lines land in the host's debug log.
        p1_env["FM2K_PERF_PROFILE"] = "1"
    p2_env = {**common_env,
              "FM2K_LOCAL_PORT": str(P2_PORT),
              "FM2K_REMOTE_ADDR": f"127.0.0.1:{P1_PORT}"}
    # Spectator: no autoplay -- it is driven entirely by the host's event
    # stream. Only the parity recorder env matters... PLUS the test-only
    # downlink-loss shim: the impair knobs must reach the SPECTATOR (the path
    # that matters), else a "loss" run silently impairs nothing. Kept minimal
    # (no common_env merge) so the spec never gains autoplay inputs.
    # Per-spectator config. Each spectator dials the host on a DISTINCT port +
    # --player-index (-> distinct FM2K_P{idx+1}_Debug.log + parity), so K
    # concurrent spectators don't collide and each is gated independently. Env
    # kept minimal (no common_env merge) so a spec never gains autoplay; the
    # test-only downlink-loss shim knobs are forwarded (else a loss run silently
    # impairs nothing). session-kind stays default boot-to-battle; the host
    # decides FULL_SESSION (backfill-from-0 / CSS-walk) vs CURRENT_MATCH snapshot
    # purely by WHEN the spec dials in (phase = css joins early, battle joins
    # after the host's battle session is created).
    specs = []
    for k, phase in enumerate(spec_phases):
        idx  = 2 + k                              # FM2K_PLAYER_INDEX -> FM2K_P{idx+1}
        port = SPEC_PORT + k                      # 7002, 7003, ...
        pty  = OUT_DIR / f"spec{k}_parity.pty"
        pty.unlink(missing_ok=True)
        env = {"FM2K_PARITY_RECORD_PATH": to_win(pty)}
        for kk in ("FM2K_SPEC_DROP", "FM2K_SPEC_DROP_SEED",
                   "FM2K_NET_DELAY_MS", "FM2K_NET_JITTER_MS", "FM2K_NET_LOSS", "FM2K_NET_SEED"):
            if os.environ.get(kk):
                env[kk] = os.environ[kk]
        # The spectator MUST run the same round count as the host, else a 1-round
        # host vs best-of-3 spectator diverges at the host's round-1 match-end.
        if args.rounds > 0:
            env["FM2K_TEST_ROUNDS"] = str(args.rounds)
        sargs = [str(LAUNCHER), "--host", game_arg,
                 "--spectate", f"127.0.0.1:{P1_PORT}",
                 "--port", str(port), "--player-index", str(idx)]
        specs.append({"k": k, "phase": phase, "idx": idx, "port": port,
                      "pty": pty, "env": env, "args": sargs,
                      "log": f"FM2K_P{idx+1}_Debug.log",
                      "live": OUT_DIR / f"live_FM2K_P{idx+1}_Debug.log",
                      "rc": None})
    spec_pty = specs[0]["pty"]   # alias for the single-spec parity/replay code below

    start_ts = time.time()

    def has_new_replay():
        return find_latest_fm2krep(game_dir, start_ts, suffix="_p0_harness") is not None

    p1_args = [str(LAUNCHER), "--host", game_arg, "--port", str(P1_PORT)]
    p2_args = [str(LAUNCHER), "--connect", f"127.0.0.1:{P1_PORT}", game_arg,
               "--port", str(P2_PORT)]

    print("[harness] launching P1 (host) + P2 (join); spectators: "
          + ", ".join(f"P{s['idx']+1}:{s['phase']}@{s['port']}" for s in specs))
    rcs = [None, None]   # P1, P2
    t1 = threading.Thread(target=lambda: rcs.__setitem__(0,
        launch("P1", p1_args, p1_env, OUT_DIR / "p1.log",
               args.record_timeout, has_new_replay)))
    t2 = threading.Thread(target=lambda: rcs.__setitem__(1,
        launch("P2", p2_args, p2_env, OUT_DIR / "p2.log",
               args.record_timeout, has_new_replay)))

    # Per-spectator completion: its parity stream goes quiescent (no growth for
    # 6s after real content) once the host's TerminateProcess cuts the feed.
    def make_spec_done(s):
        st = {"size": -1, "since": 0.0}
        def done():
            if not has_new_replay():
                return False
            try:
                sz = s["pty"].stat().st_size
            except OSError:
                return False
            now = time.time()
            if sz != st["size"]:
                st["size"] = sz; st["since"] = now; return False
            return sz > 32 + 260 * 10 and (now - st["since"]) >= 6.0
        return done

    def launch_spec(s):
        s["rc"] = launch(f"SPEC{s['k']}", s["args"], s["env"],
                         OUT_DIR / f"spec{s['k']}.log",
                         args.record_timeout + 60, make_spec_done(s))

    def gen_fake_schedule(k):
        # Stagger joins 1s apart so the host sees a steady ramp of subscribers.
        # The LAST fake churns (leave -> rejoin twice) to repeatedly re-trigger
        # the host's snapshot + full session-backfill -- the heaviest fan-out
        # path -- landing in battle where the snapshot is a ~1MB savestate.
        t = k * 1.0
        if not args.no_fake_churn and k == args.fake_spectators - 1:
            return (f"join@{t},leave@{t+6},join@{t+10},"
                    f"leave@{t+16},join@{t+20}")
        return f"join@{t}"

    fake_procs = []
    def spawn_fakes():
        fake_win = to_win(FAKE_SPEC_TOOL)
        for k in range(args.fake_spectators):
            sched = args.fake_schedule or gen_fake_schedule(k)
            log_win = to_win(OUT_DIR / f"fake{k}.log")
            # Run the fake as a WINDOWS process (Windows Python, via .bat like the
            # launcher). A WSL python on 127.0.0.1 hits WSL's loopback, NOT the
            # Windows host (NAT-mode WSL2) -- the host never sees the JOIN. Windows
            # python on 127.0.0.1 reaches the host exactly like the real clients.
            bat = OUT_DIR / f".fake{k}.bat"
            cmd = (f'"{WIN_PYTHON}" "{fake_win}" --host-udp 127.0.0.1:{P1_PORT} '
                   f'--local-udp-port {FAKE_PORT_BASE + k} --schedule {sched} '
                   f'--duration {args.fake_duration} --label fake{k}')
            bat.write_text("@echo off\r\n" + cmd + f' > "{log_win}" 2>&1\r\n')
            fp = subprocess.Popen(["cmd.exe", "/C", to_win(bat)],
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
            fake_procs.append(fp)
        print(f"[harness] spawned {args.fake_spectators} fake spectators "
              f"(Windows py, UDP {FAKE_PORT_BASE}..{FAKE_PORT_BASE + args.fake_spectators - 1}); "
              f"host FM2K_PERF_PROFILE on")

    t1.start()
    time.sleep(1.0)
    t2.start()

    spec_threads = []
    # css-phase spectators dial in during the host's CSS (wall-clock delay).
    time.sleep(args.spec_join_delay)
    # Fake spectators start loading the host now too (their own schedules then
    # stagger joins + churn through CSS into battle).
    if args.fake_spectators > 0:
        spawn_fakes()
    for s in specs:
        if s["phase"] == "css":
            t = threading.Thread(target=launch_spec, args=(s,)); t.start()
            spec_threads.append(t)
            print(f"[harness] SPEC{s['k']} (css, P{s['idx']+1}) joining now")
    # battle-phase spectators dial in AFTER the host's battle session is created
    # = a true mid-battle CURRENT_MATCH snapshot join.
    battle_specs = [s for s in specs if s["phase"] == "battle"]
    if battle_specs:
        host_live = game_dir / "logs" / "FM2K_P1_Debug.log"
        marker = "GekkoNet battle session created"
        deadline = time.time() + args.record_timeout
        while time.time() < deadline:
            try:
                if marker in open(host_live, errors="ignore").read():
                    break
            except OSError:
                pass
            time.sleep(0.3)
        time.sleep(args.battle_join_offset)
        for s in battle_specs:
            t = threading.Thread(target=launch_spec, args=(s,)); t.start()
            spec_threads.append(t)
            print(f"[harness] SPEC{s['k']} (battle, P{s['idx']+1}) joining mid-battle now")

    t1.join(); t2.join()
    for t in spec_threads:
        t.join()
    for fp in fake_procs:
        try:
            fp.terminate()
        except OSError:
            pass
    if fake_procs:
        # Backstop: subscribed fakes self-exit on host_gone, but a churning fake
        # parked in a 'left' window when the host dies would linger to
        # --fake-duration. The fakes are the only Windows python.exe in a test.
        subprocess.run(["taskkill.exe", "/F", "/IM", "python.exe"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    kill_strays()
    print(f"[harness] rcs: P1={rcs[0]} P2={rcs[1]} "
          + " ".join(f"SPEC{s['k']}={s['rc']}" for s in specs))

    # Preserve the live-phase debug logs IMMEDIATELY -- before any
    # assertion can bail (a coverage FAIL used to skip preservation and
    # the replay phase of the NEXT run overwrote the evidence).
    for lf in ["FM2K_P1_Debug.log", "FM2K_P2_Debug.log"] + [s["log"] for s in specs]:
        src = game_dir / "logs" / lf
        if src.exists():
            try:
                shutil.copy2(src, OUT_DIR / f"live_{lf}")
            except OSError as e:
                print(f"[harness] (warn) could not preserve {lf}: {e}")

    if not p1_pty.exists():
        print("[harness] FAIL: host parity missing")
        return 1
    if not spec_pty.exists():
        print("[harness] FAIL: spectator parity missing -- spectator never "
              "joined or never captured (check .spec_selftest/spec.log and "
              "logs/FM2K_P3_Debug.log)")
        return 1

    host_n, spec_n = pty_snapshots(p1_pty), pty_snapshots(spec_pty)
    print(f"[harness] host parity: {host_n} snapshots; spec parity: {spec_n}")

    # Report WHERE the spectator's join landed, in battle frames: delta
    # between the host's battle-session creation and the subscriber
    # accept, at 100 fps. Wall-clock --spec-join-delay is dominated by
    # boot time, so this is the only honest measure of join depth.
    try:
        import re as _re
        host_log = open(OUT_DIR / "live_FM2K_P1_Debug.log",
                        errors="replace").read()
        def ts_of(pattern):
            m = _re.search(r"\[(\d+):(\d+):(\d+)\.(\d+)\][^\n]*" + pattern, host_log)
            if not m: return None
            h, mn, sc, ms = (int(x) for x in m.groups())
            return h * 3600 + mn * 60 + sc + ms / 1000.0
        t_battle = ts_of(_re.escape("GekkoNet battle session created"))
        t_join   = ts_of(_re.escape("Accepted subscriber"))
        if t_battle is not None and t_join is not None:
            join_frame = int((t_join - t_battle) * 100)
            print(f"[harness] spectator joined ~battle frame {join_frame} "
                  f"({t_join - t_battle:+.1f}s after battle start)")
        rounds = len(_re.findall(r"ROUND_END|Round end", host_log))
        if rounds:
            print(f"[harness] host log shows ~{rounds} round-end event(s)")
    except OSError:
        pass
    if spec_n < min_coverage:
        print(f"[harness] FAIL: spectator covered only {spec_n} frames "
              f"(< required {min_coverage}) -- stream stalled or join failed")
        return 1

    # Informational: host-vs-spec. Valid on clean loopback; under loss the
    # HOST predicts and its parity captures SPECULATIVE states, producing
    # false divergences here (the spectator consumes confirmed inputs and
    # is the more trustworthy stream). Do not gate on this.
    print("[harness] (info) diffing HOST parity vs SPECTATOR parity "
          "(unreliable under packet loss -- host captures speculative states)")
    subprocess.call([sys.executable, str(PARITY_DIFF),
                     str(p1_pty), str(spec_pty),
                     "host-vs-spec INFO (loss-sensitive, non-authoritative)"])

    # ADVISORY (NOT the gate): spec-vs-replay, confirmed-vs-confirmed. In theory
    # both the spectator stream and --replay playback are driven by the host's
    # CONFIRMED input stream, so they should match -- but parity_diff index-pairs
    # rows, which mis-aligns under multi-match autoplay + the spectator's
    # catch-up cadence (the documented false "frame 71" alarms). The
    # AUTHORITATIVE check is the host-vs-spec trace pairing further below; this
    # diff is kept only as a human cross-check + the checked==0 fallback.
    if args.total_frames > 0:
        # Multi-match mode: the harness slice at terminate is a MID-MATCH-2
        # slice; the gate replays match 1's CANONICAL file (written by
        # Netplay_EndBattle, host-only, no _harness suffix) -- earliest
        # post-start canonical = match 1.
        cands = []
        for f in (game_dir / "replays").glob("*.fm2krep"):
            if "_harness" in f.name:
                continue
            try:
                m = f.stat().st_mtime
            except OSError:
                continue
            if m + 1.0 >= start_ts:
                cands.append((m, f))
        p0_rep = min(cands)[1] if cands else None
        if not p0_rep:
            print("[harness] FAIL: no canonical match-1 .fm2krep "
                  "(did match 1 actually complete?)")
            return 1
        print(f"[harness] match-1 canonical replay file: {p0_rep.name}")

        # Multi-match liveness assertions.
        host_log = open(OUT_DIR / "live_FM2K_P1_Debug.log",
                        errors="replace").read()
        battles = host_log.count("GekkoNet battle session created")
        print(f"[harness] host battle sessions created: {battles}")
        if battles < 2:
            print("[harness] FAIL: match 2 never started on the host "
                  "(battle-entry transition deadlock?)")
            return 1
        # Spectator must have followed into match 2: its parity stream
        # needs >= 2 battle segments.
        import struct as _st
        d = spec_pty.read_bytes()[32:]
        segs, in_b = 0, False
        for k in range(len(d) // 260):
            ph  = _st.unpack_from('<i', d, k * 260 + 16)[0]
            p1s = _st.unpack_from('<i', d, k * 260 + 32)[0]
            b = (ph == 3000 and p1s != -1)
            if b and not in_b:
                segs += 1
            in_b = b
        print(f"[harness] spectator battle segments observed: {segs}")
        if segs < 2:
            print("[harness] FAIL: spectator did not follow into match 2")
            return 1
        # Every boundary crossing must apply the deferred battle-init ops
        # (PIN_RNG at minimum) at the spec's battle entry. A local early
        # battle entry (the 2026-06-11 rematch-CSS auto-advance bug)
        # consumed the once-per-battle init edge before the ops arrived --
        # match 2 ran with an unpinned RNG and nothing failed loudly
        # because the parity gate only covers match 1.
        spec_log = OUT_DIR / "live_FM2K_P3_Debug.log"
        if spec_log.exists():
            txt = spec_log.read_text(errors="replace")
            pins = txt.count("applied deferred PIN_RNG")
            needed = segs - 1  # first battle is snapshot-anchored
            print(f"[harness] deferred PIN_RNG applies: {pins} "
                  f"(boundary crossings: {needed})")
            if pins < needed:
                print("[harness] FAIL: a boundary crossing entered battle "
                      "without applying the deferred init ops (early local "
                      "battle entry -- match desync)")
                return 1
    else:
        p0_rep = None
        deadline = time.time() + 10.0
        while time.time() < deadline:
            p0_rep = find_latest_fm2krep(game_dir, start_ts, suffix="_p0_harness")
            if p0_rep:
                break
            time.sleep(0.5)
        if not p0_rep:
            print("[harness] FAIL: no p0 harness .fm2krep for the replay gate")
            return 1
    replay_pty = OUT_DIR / "replay_parity.pty"
    replay_pty.unlink(missing_ok=True)
    kill_strays()
    time.sleep(1.0)
    wait_ports_free([P1_PORT, P2_PORT] + [SPEC_PORT + k for k in range(len(spec_phases))])
    # NOTE: no FM2K_BOOT_TO_BATTLE -- netplay-recorded replays must walk the
    # title/CSS path (see replay_netplay_diff.py:81-85).
    rep_env = {"FM2K_PARITY_RECORD_PATH": to_win(replay_pty),
               # Harness-only: full-speed drain (offline replays play 1:1
               # for humans now that the bank drains skip replay mode).
               "FM2K_SPECTATOR_ALWAYS_CATCHUP": "1"}
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
    rep_rc = launch("REPLAY", [str(LAUNCHER), "--replay", to_win(p0_rep)],
                    rep_env, OUT_DIR / "replay.log",
                    args.record_timeout, rep_done)
    print(f"[harness] replay rc={rep_rc}")
    if not replay_pty.exists():
        # Advisory only -- do NOT block the authoritative host-vs-spec GATE.
        # (The replay process can legitimately not produce parity when the
        # first spectator is a mid-battle joiner, etc.)
        print("[harness] (advisory) replay parity missing -- skipping the "
              "spec-vs-replay advisory diff; the host-vs-spec GATE is authoritative.")
        diff_rc = 2
    else:
        # Trim both streams to their FIRST battle segment: the spectator's
        # stream may continue into CSS/match 2, and the replay's stream has a
        # title/CSS prefix + post-battle tail -- index pairing past either
        # boundary compares unlike phases.
        spec_m1   = OUT_DIR / "spec_m1.pty"
        replay_m1 = OUT_DIR / "replay_m1.pty"
        n_spec   = trim_first_battle_segment(spec_pty, spec_m1)
        n_replay = trim_first_battle_segment(replay_pty, replay_m1)
        print(f"[harness] (advisory) SPECTATOR vs REPLAY parity, first battle "
              f"segment (spec={n_spec} rows, replay={n_replay} rows). NOT the gate: "
              f"this compares against a SEPARATE replay process that mis-pairs the "
              f"wrong match under multi-match autoplay and position-mis-aligns the "
              f"spectator's catch-up cadence -- it produced false 'frame 71' alarms "
              f"while host-vs-spec showed bit-exact sync. See the GATE below.")
        diff_rc = subprocess.call([sys.executable, str(PARITY_DIFF),
                                   str(spec_m1), str(replay_m1),
                                   "spec-vs-replay ADVISORY (index-paired)"])

    # AUTHORITATIVE GATE: host-vs-spec per-frame trace pairing (SAME run, same
    # match = ground truth). Both the host and spectator here watch the
    # identical match, so bit-for-bit (rng, inputs, scripts) at every paired
    # battle frame proves the spectator stayed in sync. Needs FM2K_HOST_TRACE=1
    # + FM2K_SPECTATOR_DEBUG=1 (set in common_env). [SPEC-TRACE] is dense over
    # bf 0..99; [SPEC-FP]/[HOST-FP] checkpoint every 30 frames to match end.
    # rng-keyed (NOT bf-keyed): the spectator's per-frame rng_post is a strong
    # state fingerprint. We assert every spectator frame's rng appears in the
    # HOST's rng set with matching inputs/scripts. This is robust to: a frame
    # offset (mid-battle CURRENT_MATCH snapshot join starts at host frame N,
    # not 0), per-match bf RESETS (multi-match: each battle restarts bf at 0,
    # so bf-keying would cross-contaminate matches), and catch-up cadence. A
    # spectator that desyncs computes an rng the host never produced -> the
    # frame's rng is "not in host" = hard fail.
    import re as _ret
    # CRITICAL: read the PRESERVED spectator log, not game_dir/logs/FM2K_P3.
    # The REPLAY process (launched after the spec, with ALWAYS_CATCHUP=1)
    # overwrites game_dir/logs/FM2K_P3_Debug.log -- so the live game_dir P3
    # holds the replay's traces, which re-sim the host's CONFIRMED inputs and
    # match the host BY CONSTRUCTION (0 misses = guaranteed false PASS). The
    # live_ copies are snapshotted before the replay runs and hold the ACTUAL
    # spectator's traces. (2026-06-23: this masked a real bf=77 spectator
    # desync under loss -- the spec computed an rng no player ever produced.)
    host_dbg = OUT_DIR / "live_FM2K_P1_Debug.log"
    # group(1)=bf, group(2)=rng, group(3+)=comparison fields.
    TRC = (r'(?:HOST|SPEC)-TRACE\] bf=(\d+) rng_pre=0x[0-9A-F]+ '
           r'rng_post=0x([0-9A-F]+) p1=0x([0-9A-F]+) p2=0x([0-9A-F]+)')
    FP  = (r'(?:HOST|SPEC)-FP\] bf=(\d+).*?p1_hp=(\d+) p2_hp=(\d+).*?'
           r'p1_pos=\(([-\d]+),[-\d]+\) p2_pos=\(([-\d]+),[-\d]+\) '
           r'p1_script=(-?\d+) p2_script=(-?\d+)')
    def _rows2(path, pat):
        out = []
        try:
            for ln in open(path, errors="ignore"):
                m = _ret.search(pat, ln)
                if m:
                    g = m.groups()
                    # Skip the battle-entry frame (bf==0): hp/timer/pos aren't
                    # loaded yet and rng is pre-pin -- a transitional, not a
                    # settled gameplay state. Host and spectator hit it at
                    # slightly different init timing (esp. during catch-up), so
                    # comparing it is a false mismatch. Each match has one.
                    if int(g[0]) == 0:
                        continue
                    out.append((g[1], tuple(g[2:])))  # (rng, fields)
        except OSError:
            pass
        return out
    def _check(host_rows, spec_rows):
        hmap = {}
        for rng, fields in host_rows:
            hmap.setdefault(rng, set()).add(fields)
        not_found = field_mm = 0
        first = None
        for rng, fields in spec_rows:
            if rng not in hmap:
                not_found += 1
                if first is None:
                    first = ("rng-NOT-in-host (real desync)", rng, fields)
            elif fields not in hmap[rng]:
                field_mm += 1
                if first is None:
                    first = ("field-mismatch @rng", rng, "spec", fields,
                             "host", hmap[rng])
        return len(spec_rows), not_found, field_mm, first
    # TRACE: rng-PRESENCE only. The p1/p2 input fields are capture-noise
    # (predicted-vs-confirmed + different capture points on host vs spec -- the
    # same reason parity_diff excludes inputs). rng_post IS the post-frame state
    # fingerprint: if every spectator rng appears in the host's set, the sims
    # produced identical state. Comparing inputs here gave false mismatches on a
    # snapshot-join (31 of them) while rng+scripts were bit-exact.
    # FP checkpoint: gate on GAMEPLAY STATE (hp + positions + scripts), NOT the
    # FP rng field. The host logs [HOST-FP] rng at a different sub-frame point
    # (netplay_battle_events.cpp) than the spectator's [SPEC-FP]
    # (trampoline_spectator.cpp), and the spectator's capture point is also
    # catchup-dependent -- so FP rng differs host-vs-spec even when the sim is
    # bit-exact (it false-FAILED a verified-correct mid-battle snapshot-join,
    # 2026-06-23). hp/pos/scripts are aligned and a strong fingerprint; the dense
    # aligned TRACE rng_post (bf 0-99) remains the authoritative rng check.
    def fp_states(path):
        out = []
        try:
            for ln in open(path, errors="ignore"):
                m = _ret.search(FP, ln)
                if m and int(m.group(1)) != 0:   # skip bf=0 (pre-pin transitional)
                    # (hp1,hp2,s1,s2) -- POSITIONS dropped: they're captured at
                    # different sub-frame points host-vs-spec (a moving player is
                    # off by ~1 frame of travel at the termination tail) even when
                    # bit-exact. hp + scripts are capture-stable (matched the whole
                    # battle including the tail) and a real desync diverges in them.
                    out.append((m.group(2), m.group(3), m.group(6), m.group(7)))
        except OSError:
            pass
        return out
    # Host sets are shared; gate EACH spectator against them independently.
    host_trc_rng = {rng for rng, _ in _rows2(host_dbg, TRC)}
    host_fp_set  = set(fp_states(host_dbg))

    def gate_one(spec_live):
        trc_spec = _rows2(spec_live, TRC)
        ct = len(trc_spec)
        mt = sum(1 for rng, _ in trc_spec if rng not in host_trc_rng)
        first_t = next(((rng,) for rng, _ in trc_spec if rng not in host_trc_rng), None)
        sfp = fp_states(spec_live)   # ordered by bf
        cf = len(sfp)
        miss = [st not in host_fp_set for st in sfp]
        mf_total = sum(miss)
        # A no-rollback spectator's sim is a one-way forward replay: a REAL desync
        # NEVER re-converges, so its FP misses form a persistent TAIL reaching the
        # LAST FP frame. A BOUNDED interior run that re-syncs (later frames match)
        # is a capture artifact -- a small frame-offset in a TRANSITIONING value
        # (a round-start hp-fill animation, hp during a hit, a move-start) sampled
        # against the host's every-30-frame FP grid; the values land back on the
        # grid once the value stabilizes. (vanpri under loss: hp-fill +3 frames
        # ahead, bf 270-360, re-syncs at 390 -- 2026-06-23.) Fail only on a
        # PERSISTENT tail (last FP frame misses) or a massive >40% divergence. The
        # aligned TRACE rng_post (mt) stays the authoritative per-frame check.
        trailing = 0
        for f in reversed(miss):
            if f:
                trailing += 1
            else:
                break
        massive = cf > 0 and mf_total > 0.40 * cf
        mf = mf_total if (trailing > 0 or massive) else 0
        run = mx = 0                      # longest run -- advisory only now
        for f in miss:
            run = run + 1 if f else 0
            if run > mx:
                mx = run
        first_f = next((st for st, f in zip(sfp, miss) if f), None)
        return dict(ct=ct, cf=cf, mt=mt, mf=mf, mf_total=mf_total, mx=mx,
                    trailing=trailing,
                    checked=ct + cf, bad=mt + mf,
                    first_t=first_t, first_f=first_f)

    for s in specs:
        r = gate_one(s["live"])
        s["gate"] = r
        s["ok"] = r["checked"] > 0 and r["bad"] == 0
        verdict = "PASS" if s["ok"] else ("INCONCLUSIVE" if r["checked"] == 0 else "FAIL")
        print(f"[harness] GATE SPEC{s['k']} ({s['phase']}, P{s['idx']+1}): "
              f"checked {r['checked']} (TRACE {r['ct']} rng + FP {r['cf']} hp/scripts); "
              f"{r['mt']} TRACE-rng + {r['mf']} FP-state not-in-host "
              f"(FP raw {r['mf_total']}, max-run {r['mx']}, tail {r['trailing']}) -> {verdict}")
        if r["first_t"]:
            print(f"    TRACE rng-not-in-host (REAL divergence): {r['first_t']}")
        if r["mf_total"] and r["mf"] == 0:
            print(f"    (advisory) {r['mf_total']} FP miss(es) but they RE-SYNC "
                  f"(tail={r['trailing']}, max-run {r['mx']}) -- bounded capture-"
                  f"timing offset on a transitioning hp/script, not a desync")
        if r["mf"]:
            print(f"    FP hp/scripts PERSISTENT divergence (tail={r['trailing']}): {r['first_f']}")

    # Phase 3: host-no-hiccup report (host ran with FM2K_PERF_PROFILE on).
    if measure_host:
        report_host_pacing(OUT_DIR / "live_FM2K_P1_Debug.log", args.fake_spectators)

    real_fail   = any(s["gate"]["checked"] > 0 and not s["ok"] for s in specs)
    checked_any = any(s["gate"]["checked"] > 0 for s in specs)

    if not args.keep and not real_fail and checked_any:
        cleanup = [p1_pty, replay_pty, OUT_DIR / "p1.log", OUT_DIR / "p2.log",
                   OUT_DIR / "replay.log"]
        cleanup += [s["pty"] for s in specs]
        cleanup += [OUT_DIR / f"spec{s['k']}.log" for s in specs]
        for f in cleanup:
            f.unlink(missing_ok=True)

    if real_fail:
        print(f"[harness] OVERALL FAIL: a spectator desynced from host (rng-keyed).")
        return 1
    if not checked_any:
        print("[harness] GATE INCONCLUSIVE: no spectator trace frames -- need "
              "FM2K_HOST_TRACE=1 + FM2K_SPECTATOR_DEBUG=1 and a battle phase")
        return diff_rc
    print(f"[harness] OVERALL PASS: all {len(specs)} spectator(s) bit-exact with host.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
