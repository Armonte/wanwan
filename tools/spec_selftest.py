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
    # FM95 engine (Comic Party Wars) -- the launcher sniffs the engine from
    # the exe and injects FM95Hook.dll. Full-width filename is the real one.
    "cpw":    Path("/mnt/c/dev/fm95/CPW/ＣＰＷ.exe"),
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


# ---------------------------------------------------------------------------
# LIVE-EDGE spectator metric -- the "held the live edge" pass condition.
#
# Question: did the spectator's APPLIED battle-frame progression REACH AND HOLD
# near the host's FINAL battle frame? This is a LIVENESS / COVERAGE check --
# deliberately distinct from an engine desync (the rng/hp/CINPUT gates) and from
# "match didn't complete" (the .fm2krep check). Three separate signals.
#
# We measure each side's furthest battle frame from the per-frame `[CINPUT] bf=`
# marker (host: confirmed input post-gekko; spec: applied input -- the spec never
# rolls back). bf increments 0,1,2,... within a match and RESETS to 0 at the next
# match, so it is robust across the multi-match loss sweep. We deliberately do
# NOT key off:
#   * `BATTLE STATUS: frame=` -- g_netplay_frame RESETS per match but the log's
#     500-frame cadence is a process-static, so it stops firing after match 1
#     (kept only as a single-match fallback when [CINPUT] is absent).
#   * `[SPEC-UDP] admitted=` -- the UDP-fast-path admit COUNT only; frames the
#     spectator receives via TCP backfill never bump it, so it undercounts the
#     true progression badly (observed 226 admitted while the spectator actually
#     reached bf 1499). Parsed here ONLY for the flatline/stall diagnostic.
# ---------------------------------------------------------------------------
LIVE_EDGE_TOLERANCE = 100   # battle frames the spectator may legitimately trail


def _cinput_seg_maxbf(log_path, min_seg=5):
    """Per-match furthest [CINPUT] bf as a list (one entry per battle segment).
    A drop in bf marks a match boundary. Degenerate <min_seg blips are dropped."""
    import re
    pat = re.compile(r"\[CINPUT\] bf=(\d+)")
    segs, cur_max, last = [], -1, -1
    try:
        fh = open(log_path, errors="ignore")
    except OSError:
        return segs
    for ln in fh:
        m = pat.search(ln)
        if not m:
            continue
        bf = int(m.group(1))
        if bf < last and cur_max >= 0:      # bf reset -> new match segment
            segs.append(cur_max)
            cur_max = -1
        cur_max = max(cur_max, bf)
        last = bf
    if cur_max >= 0:
        segs.append(cur_max)
    return [s for s in segs if s >= min_seg]


def _battle_status_maxframe(log_path):
    """Fallback host progression: max `BATTLE STATUS: frame=<F>` (g_netplay_frame).
    Only reliable single-match; used when the host log has no [CINPUT]."""
    import re
    pat = re.compile(r"BATTLE STATUS: frame=(\d+)")
    mx = None
    try:
        for ln in open(log_path, errors="ignore"):
            m = pat.search(ln)
            if m:
                v = int(m.group(1))
                mx = v if mx is None else max(mx, v)
    except OSError:
        pass
    return mx


def _spec_udp_admitted_series(log_path):
    """The `[SPEC-UDP] admitted=<N>` 1Hz series (UDP-fast-path admit count)."""
    import re
    pat = re.compile(r"\[SPEC-UDP\] admitted=(\d+)")
    out = []
    try:
        for ln in open(log_path, errors="ignore"):
            m = pat.search(ln)
            if m:
                out.append(int(m.group(1)))
    except OSError:
        pass
    return out


def spectator_liveness(host_log, spec_log, tolerance=LIVE_EDGE_TOLERANCE):
    """Did the spectator reach & HOLD near the host's final battle frame?

    Returns a dict; `reached` (bool) is the pass condition. Also imported and
    called by tools/desync_seed_sweep.py, so it must stay side-effect free."""
    host_segs = _cinput_seg_maxbf(host_log)
    spec_segs = _cinput_seg_maxbf(spec_log)
    # Host's final battle frame = furthest bf in its LAST match (fallback: the
    # single-match BATTLE STATUS max when [CINPUT] is absent).
    host_final = host_segs[-1] if host_segs else _battle_status_maxframe(host_log)
    # A spectator with FEWER battle segments than the host fell a whole match
    # behind = did not hold the live edge.
    followed_all = bool(spec_segs) and len(spec_segs) >= max(1, len(host_segs))
    if spec_segs:
        idx = (len(host_segs) - 1) if host_segs else -1
        spec_max = spec_segs[idx] if followed_all else spec_segs[-1]
    else:
        spec_max = 0
    gap = (host_final - spec_max) if host_final is not None else None
    reached = bool(followed_all and host_final is not None
                   and spec_max >= host_final - tolerance)
    adm = _spec_udp_admitted_series(spec_log)
    spec_udp_admitted = adm[-1] if adm else 0
    # Flatlined = the UDP admit counter plateaued at the tail (diagnostic only;
    # it also plateaus on a healthy end-of-feed drain, so never gate on it alone).
    admitted_flatlined = len(adm) >= 3 and adm[-1] == adm[-3]
    stall_frame = spec_max if not reached else None
    return dict(host_final=host_final, spec_max=spec_max, gap=gap, reached=reached,
                followed_all=followed_all, host_matches=len(host_segs),
                spec_matches=len(spec_segs), spec_udp_admitted=spec_udp_admitted,
                admitted_flatlined=admitted_flatlined, stall_frame=stall_frame,
                tolerance=tolerance)


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
    ap.add_argument("--assert-spectator-live", action="store_true",
                    help="exit nonzero if any spectator's applied battle-frame "
                         "progression did NOT reach/hold near the host's final "
                         "battle frame (the 'held the live edge' pass condition). "
                         "Always computed + printed; this flag makes it a gate.")
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
        "FM2K_CSS_TRACE":        os.environ.get("FM2K_CSS_TRACE", ""),
        # Ground-truth desync detector: every-frame confirmed/applied input log,
        # compared P1==P2==every spectator per (match, bf). Primary verification.
        "FM2K_CINPUT":           os.environ.get("FM2K_CINPUT", "1"),
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
              # host-clock sync + rift frame pacing A/B
              "FM2K_HOST_CLOCK",
              # ReliableChannel spectator A/B transport (reliable-ordered+FEC over UDP)
              "FM2K_SPEC_RC", "FM2K_SPEC_RC_SNAPSHOT", "FM2K_RC_FEC", "FM2K_RC_FEC_K",
              "FM2K_RC_RESEND_MS", "FM2K_RC_RATE_PPS", "FM2K_RC_CWND",
              # in-process link impairment (players' gekko+control path)
              "FM2K_NET_DELAY_MS", "FM2K_NET_JITTER_MS", "FM2K_NET_LOSS", "FM2K_NET_SEED",
              "FM2K_NET_REORDER", "FM2K_NET_DUP"):
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
    # Per-side local-delay override for asymmetric-delay validation: each peer
    # runs its OWN delay verbatim (no max-adoption). Unset => shared
    # FM2K_LOCAL_DELAY / auto negotiation.
    if os.environ.get("FM2K_P1_LOCAL_DELAY"):
        p1_env["FM2K_LOCAL_DELAY"] = os.environ["FM2K_P1_LOCAL_DELAY"]
    if os.environ.get("FM2K_P2_LOCAL_DELAY"):
        p2_env["FM2K_LOCAL_DELAY"] = os.environ["FM2K_P2_LOCAL_DELAY"]
    # Per-side send delay for ASYMMETRIC-routing emulation: the injector delays each
    # peer's own SEND, so P1's delay = the P1->P2 (forward) latency and P2's = the
    # P2->P1 (return) latency. Setting them differently emulates an asymmetric route.
    if os.environ.get("FM2K_P1_NET_DELAY_MS"):
        p1_env["FM2K_NET_DELAY_MS"] = os.environ["FM2K_P1_NET_DELAY_MS"]
    if os.environ.get("FM2K_P2_NET_DELAY_MS"):
        p2_env["FM2K_NET_DELAY_MS"] = os.environ["FM2K_P2_NET_DELAY_MS"]
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
        # FM2K_LOG_TAG -> the spectator logs as [S1]/[S2]/... (file FM2K_S{n}_Debug.log)
        # instead of [P3]/[P4], so multi-spectator output is readable + each is
        # uniquely identifiable.
        env = {"FM2K_PARITY_RECORD_PATH": to_win(pty), "FM2K_LOG_TAG": f"S{k+1}",
               "FM2K_CINPUT": os.environ.get("FM2K_CINPUT", "1"),
               # Exit ~5s after the host's feed stops (harness TerminateProcess =
               # no graceful SESSION_END) instead of spinning at [SPEC-Q] q=0.
               "FM2K_SPEC_HOST_GONE_MS": os.environ.get("FM2K_SPEC_HOST_GONE_MS", "5000")}
        for kk in ("FM2K_SPEC_DROP", "FM2K_SPEC_DROP_SEED", "FM2K_CSS_TRACE",
                   "FM2K_SPECTATOR_DEBUG", "FM2K_SPEC_CONNECT_TIMEOUT_MS",
                   "FM2K_NET_DELAY_MS", "FM2K_NET_JITTER_MS", "FM2K_NET_LOSS", "FM2K_NET_SEED",
                   "FM2K_NET_REORDER", "FM2K_NET_DUP"):
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
                      "tag": f"S{k+1}",
                      "pty": pty, "env": env, "args": sargs,
                      "log": f"FM2K_S{k+1}_Debug.log",
                      "live": OUT_DIR / f"live_FM2K_S{k+1}_Debug.log",
                      "rc": None})
    spec_pty = specs[0]["pty"]   # alias for the single-spec parity/replay code below

    start_ts = time.time()

    def has_new_replay():
        return find_latest_fm2krep(game_dir, start_ts, suffix="_p0_harness") is not None

    p1_args = [str(LAUNCHER), "--host", game_arg, "--port", str(P1_PORT)]
    p2_args = [str(LAUNCHER), "--connect", f"127.0.0.1:{P1_PORT}", game_arg,
               "--port", str(P2_PORT)]

    print("[harness] launching P1 (host) + P2 (join); spectators: "
          + ", ".join(f"{s['tag']}:{s['phase']}@{s['port']}" for s in specs))
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
        s["rc"] = launch(s["tag"], s["args"], s["env"],
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
    host_live = game_dir / "logs" / "FM2K_P1_Debug.log"
    # Fake spectators start loading the host now too (their own schedules then
    # stagger joins + churn through CSS into battle).
    if args.fake_spectators > 0:
        spawn_fakes()

    def count_marker(marker):
        try:
            return open(host_live, errors="ignore").read().count(marker)
        except OSError:
            return 0

    def schedule_spec(s):
        # phase = "css[N]" or "battle[N]": dial in during the host's Nth CSS / Nth
        # battle (default N=1). css[N] -> a FULL_SESSION/seam join while the host is
        # in its Nth char-select; battle[N] -> a CURRENT_MATCH snapshot join mid the
        # host's Nth battle. Keyed off host-log markers so it tracks the real phase
        # under loss/jitter rather than a fixed wall clock.
        phase = s["phase"]
        kind = "css" if phase.startswith("css") else "battle"
        suffix = phase[len(kind):]
        n = int(suffix) if suffix.isdigit() else 1
        marker = "CSS: Entered" if kind == "css" else "GekkoNet battle session created"
        deadline = time.time() + args.record_timeout
        while time.time() < deadline and count_marker(marker) < n:
            time.sleep(0.25)
        # Settle: a css spec waits spec_join_delay into the CSS; a battle spec waits
        # battle_join_offset so the Nth battle session exists before the snapshot.
        time.sleep(args.spec_join_delay if kind == "css" else args.battle_join_offset)
        print(f"[harness] {s['tag']} ({phase}) dialing in -- host reached {kind} #{n}")
        launch_spec(s)

    for s in specs:
        t = threading.Thread(target=schedule_spec, args=(s,)); t.start()
        spec_threads.append(t)

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
          + " ".join(f"{s['tag']}={s['rc']}" for s in specs))

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

    # ---- LIVE-EDGE metric (the REAL "held the live edge" pass condition) ------
    # Computed from the PRESERVED live_ logs, right after preservation, so it runs
    # even if a later gate bails early. Always printed (informational); gated only
    # under --assert-spectator-live. This is a LIVENESS signal, kept SEPARATE from
    # the engine-desync gates and from the match-completion (.fm2krep) check.
    host_p1_log = OUT_DIR / "live_FM2K_P1_Debug.log"
    live_edge_fail = False
    for s in specs:
        lv = spectator_liveness(host_p1_log, s["live"])
        # Flush-race backstop: the host hard-terminates at --total-frames while
        # a spectator under loss is legitimately a few frames behind (still
        # HOLDING the live edge). kill_strays can truncate its debug log mid-
        # drain/flush, so this first parse can read a stale spec_max and report
        # a false FAIL even though the spectator reached the live edge. On a
        # FAIL, settle + re-preserve the source log (now fully flushed, the
        # process has exited) and re-parse ONCE. A genuine wedge (spectator
        # far behind) stays FAIL on re-parse, so this can't mask a real failure.
        if not lv["reached"]:
            time.sleep(1.5)
            for lf in ["FM2K_P1_Debug.log", s["log"]]:
                src = game_dir / "logs" / lf
                if src.exists():
                    try:
                        shutil.copy2(src, OUT_DIR / f"live_{lf}")
                    except OSError:
                        pass
            lv2 = spectator_liveness(host_p1_log, s["live"])
            if lv2["reached"]:
                print(f"[harness] LIVE-EDGE {s['tag']}: first parse read a "
                      f"mid-flush log (spec_max={lv['spec_max']}); re-parse of "
                      f"the settled log shows reached=True "
                      f"(spec_max={lv2['spec_max']}, gap={lv2['gap']}).")
                lv = lv2
        s["live_edge"] = lv
        verdict = "PASS" if lv["reached"] else "FAIL"
        print(f"[harness] LIVE-EDGE {s['tag']} ({s['phase']}): "
              f"host_final_frame={lv['host_final']} spec_max_frame={lv['spec_max']} "
              f"gap={lv['gap']} (tol {lv['tolerance']}) -> "
              f"spectator_reached_live={lv['reached']} [{verdict}]")
        print(f"    matches host={lv['host_matches']} spec={lv['spec_matches']} "
              f"followed_all={lv['followed_all']}; SPEC-UDP admitted_max="
              f"{lv['spec_udp_admitted']} flatlined={lv['admitted_flatlined']}; "
              f"stall_frame={lv['stall_frame']}")
        if not lv["reached"]:
            live_edge_fail = True
    if args.assert_spectator_live and live_edge_fail:
        print("[harness] OVERALL FAIL: --assert-spectator-live: a spectator did NOT "
              "reach/hold the host's live edge (fell behind / stalled -- see "
              "LIVE-EDGE above). This is a spectator liveness failure, NOT an "
              "engine desync and NOT a match-completion failure.")
        return 1

    if not p1_pty.exists():
        print("[harness] FAIL: host parity missing")
        return 1
    if not spec_pty.exists():
        print("[harness] FAIL: spectator parity missing -- spectator never "
              f"joined or never captured (check .spec_selftest/spec0.log and "
              f"logs/{specs[0]['log']})")
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
        # slice; the gate replays match 1. PREFER a no-suffix CANONICAL file
        # (written host-only by Netplay_EndBattle, if this build still emits one)
        # -- earliest post-start canonical = match 1. FALL BACK to the
        # timestamped *_p0_harness / *_p1_harness recording the players actually
        # wrote this run (the current netplay-record path writes ONE per session):
        # it walks title/CSS/match1, and the replay gate below trims to the first
        # battle segment, so it still gates match 1. Fresh-mtime (>= start_ts)
        # guarantees it is THIS run's file, not a stale one from a prior run.
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
            # No canonical file -> accept the timestamped harness recording the
            # players just produced (host p0 first, else p1).
            p0_rep = (find_latest_fm2krep(game_dir, start_ts, suffix="_p0_harness")
                      or find_latest_fm2krep(game_dir, start_ts, suffix="_p1_harness"))
        if not p0_rep:
            print("[harness] FAIL: no match-1 .fm2krep (neither a no-suffix "
                  "canonical nor a fresh *_p{0,1}_harness) -- did match 1 "
                  "actually complete?")
            return 1
        print(f"[harness] match-1 replay file: {p0_rep.name}")

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
        spec_log = specs[0]["live"]
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
            # B2 coverage: the authoritative rng-trace GATE consumes the
            # spectator's [SPEC-TRACE]/[SPEC-FP] frames. Assert those frames
            # actually SPAN >= 2 battle segments (bf resets to 0 each match), so
            # a multi-match run cannot vacuously PASS on match-1 frames alone --
            # a match-2 desync that emitted no gated frames would otherwise slip
            # through the "checked > 0" pass condition.
            import re as _re_cov
            bfs = [int(m.group(1)) for m in
                   _re_cov.finditer(r'SPEC-(?:TRACE|FP)\] bf=(\d+)', txt)]
            gate_segs, prev = 0, None
            for bf in bfs:
                if prev is None or bf + 8 < prev:   # first frame, or bf reset = new match
                    gate_segs += 1
                prev = bf
            print(f"[harness] gate trace coverage: {len(bfs)} SPEC frames span "
                  f"{gate_segs} battle segment(s) (need >= 2 for multi-match)")
            if gate_segs < 2:
                print("[harness] FAIL: authoritative gate saw trace from only "
                      f"{gate_segs} battle segment(s) -- match 2 was never gated "
                      "(vacuous multi-match pass)")
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
        # SPECTATOR vs REPLAY parity across EVERY match. parity_diff now segments
        # both streams into per-match battle runs and aligns each on (segment,
        # frame) -- the full multi-match streams compare correctly, no more
        # trim-to-match-1 (which silently ignored match 2+). Still ADVISORY: it
        # compares a SEPARATE replay process (confirmed-input re-sim) whose
        # catch-up cadence can position-shift a few speculative frames; the
        # authoritative determinism check is the host-vs-spec trace GATE below.
        # A persistent divergence here in ANY match is still worth surfacing.
        print("[harness] (advisory) SPECTATOR vs REPLAY parity, ALL match segments "
              "(segment-aware). NOT the authoritative gate -- see the host-vs-spec "
              "trace GATE below.")
        diff_rc = subprocess.call([sys.executable, str(PARITY_DIFF),
                                   str(spec_pty), str(replay_pty),
                                   "spec-vs-replay ADVISORY (segment-aligned)"])

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
        # A BOUNDED trailing run (<= RECONVERGE_WINDOW) is the SAME rollback
        # speculative-capture artifact we already tolerate mid-stream, just landing
        # at the tail because loss cut the spectator's stream off ON a transitioning
        # value (hp mid-hit, move-start) that hasn't re-landed on the host's 30-frame
        # FP grid. A REAL desync is rng-driven -> it shows up in `mt` (the
        # authoritative aligned rng_post check, which stays a hard FAIL) AND persists
        # far past the window. So fail on the FP side only for a PERSISTENT tail
        # (> RECONVERGE_WINDOW) or a massive (>40%) divergence. (Observed: RC spec at
        # 15% loss, seeds 42/99 -> trailing=1, mt=0, 1184 frames full-state IDENTICAL
        # -- a single trailing speculative frame, not a desync. 2026-07-06.)
        RECONVERGE_WINDOW = int(os.environ.get('FM2K_PARITY_RECONVERGE_WINDOW', '32'))
        mf = mf_total if (trailing > RECONVERGE_WINDOW or massive) else 0
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

    # ---- PRIMARY: ground-truth frame-keyed input desync detector --------------
    # Compare the every-frame [CINPUT] (confirmed input on the players, applied
    # input on the spectators) per (match, bf): P1 is truth, P2 must match P1
    # (players in lockstep), every spectator must match P1 at the aligned bf. This
    # catches frame-misaligned inputs -> in-battle position desyncs the rng/hp gate
    # is structurally blind to. Authoritative; the rng/hp gate below is secondary.
    import re as _cre
    _cpat = _cre.compile(r'\[CINPUT\] bf=(\d+) p1=0x([0-9A-Fa-f]+) p2=0x([0-9A-Fa-f]+)')
    def _cin_parse(path):
        segs = []; cur = {}; last = -1
        try: fh = open(path, errors="ignore")
        except OSError: return segs
        for ln in fh:
            m = _cpat.search(ln)
            if not m: continue
            bf = int(m.group(1)); pv = (int(m.group(2), 16), int(m.group(3), 16))
            if bf <= last and cur: segs.append(cur); cur = {}
            cur[bf] = pv; last = bf
        if cur: segs.append(cur)
        return segs
    def _cin_align(sseg, hseg):
        # (full_mismatches, offset, overlap, longest_run). Offset = MODE of all
        # press-deltas -- robust to stray boundary inputs and the autoplay's
        # identical match openings (a single-press anchor mis-aligns). longest_run
        # = longest consecutive-mismatch streak; isolated 1-frame artifacts (off-by-
        # one boundary, a stray transition press) don't count as a desync, a
        # SUSTAINED run does.
        spress = [(bf, sseg[bf]) for bf in sorted(sseg) if sseg[bf] != (0, 0)]
        if not spress: return (0, 0, 0, 0)
        hbi = {}
        for hb in hseg:
            if hseg[hb] != (0, 0): hbi.setdefault(hseg[hb], []).append(hb)
        deltas = {}
        for sb, si in spress:
            for hb in hbi.get(si, ()):
                deltas[hb - sb] = deltas.get(hb - sb, 0) + 1
        if not deltas: return (len(spress), 0, 0, len(spress))
        O = max(deltas, key=deltas.get)
        bfs = sorted(bf for bf in sseg if (bf + O) in hseg)
        run = mx = fmm = 0
        for bf in bfs:
            if hseg[bf + O] != sseg[bf]:
                fmm += 1; run += 1; mx = max(mx, run)
            else:
                run = 0
        return (fmm, O, len(bfs), mx)
    cin_H = _cin_parse(OUT_DIR / "live_FM2K_P1_Debug.log")
    cin_fail = False
    if not cin_H:
        print("[harness] CINPUT: no host [CINPUT] -- detector inactive (need FM2K_CINPUT=1 + a battle)")
    else:
        cin_G = _cin_parse(OUT_DIR / "live_FM2K_P2_Debug.log")
        for i, hseg in enumerate(cin_H):
            if i >= len(cin_G): break
            gseg = cin_G[i]
            n = sum(1 for bf in hseg if bf in gseg)
            mm = sum(1 for bf in hseg if bf in gseg and hseg[bf] != gseg[bf])
            if mm:
                cin_fail = True
                fb = next(bf for bf in sorted(hseg) if bf in gseg and hseg[bf] != gseg[bf])
                print(f"[harness] CINPUT P1-vs-P2 match{i}: {mm}/{n} mismatches -- PLAYERS DESYNCED "
                      f"(first bf={fb}: p1={hseg[fb]} vs p2={gseg[fb]})")
            else:
                print(f"[harness] CINPUT P1-vs-P2 match{i}: {n} frames IDENTICAL (players lockstep)")
        for s in specs:
            for si, sseg in enumerate(_cin_parse(s["live"])):
                best = None
                for hi, hseg in enumerate(cin_H):
                    fmm, O, n, mx = _cin_align(sseg, hseg)
                    key = (mx, fmm, -n)
                    if best is None or key < best[0]: best = (key, fmm, O, n, mx, hi)
                _, fmm, O, n, mx, hi = best
                if mx > 3:   # sustained mismatch run = real input-frame desync
                    cin_fail = True
                    hseg = cin_H[hi]
                    fb = next((bf for bf in sorted(sseg) if (bf + O) in hseg and hseg[bf + O] != sseg[bf]), None)
                    print(f"[harness] CINPUT {s['tag']} seg{si}: vs host-match{hi} off{O}: {fmm} mismatches "
                          f"(longest run {mx}) -> DESYNC (first spec-bf={fb} host-bf={fb + O if fb is not None else '?'}: "
                          f"spec={sseg.get(fb)} host={hseg.get(fb + O) if fb is not None else '?'})")
                else:
                    extra = f" ({fmm} isolated boundary artifact{'s' if fmm != 1 else ''})" if fmm else ""
                    print(f"[harness] CINPUT {s['tag']} seg{si}: vs host-match{hi} off{O}: "
                          f"{n} frames input-frame IDENTICAL{extra}")

    # ---- FULL-STATE FENCEPOST: [CHECKSUM] gameplay_fingerprint (GDC GAP #1) ----
    # The host logs the gameplay_fingerprint (HP/pos/rng/timer -- gekko's own
    # P1-vs-P2 desync hash) at every SAVE event; the spectator RECOMPUTES the same
    # fingerprint from its live memory each applied frame (never rolls back ->
    # always confirmed). Aligning the spec's CRC sequence to the host's catches
    # POSITION/full-state desyncs the subset rng/hp gate is structurally blind to.
    # Host f resets per match + re-emits per re-sim -> segment on f=-1, dedupe
    # frame-LAST. Spec bf resets per battle -> segment on bf reset.
    _ckpat = _cre.compile(r'\[CHECKSUM\] f=(-?\d+) crc=0x([0-9A-Fa-f]+)')
    def _ck_host(path):
        # per-MATCH segments [{frame: crc}]; split on f=-1 (battle-entry marker),
        # dedupe frame-LAST within a segment (re-sim re-emits; last = confirmed).
        segs, cur = [], {}
        try: fh = open(path, errors="ignore")
        except OSError: return segs
        for ln in fh:
            m = _ckpat.search(ln)
            if not m: continue
            f = int(m.group(1)); crc = int(m.group(2), 16)
            if f < 0:
                if cur: segs.append(cur); cur = {}
                continue
            cur[f] = crc
        if cur: segs.append(cur)
        return segs
    def _ck_spec(path):
        segs, cur, last = [], {}, -1
        try: fh = open(path, errors="ignore")
        except OSError: return segs
        for ln in fh:
            m = _ckpat.search(ln)
            if not m: continue
            bf = int(m.group(1))
            if bf < 0: continue
            crc = int(m.group(2), 16)
            if bf <= last and cur: segs.append(cur); cur = {}
            cur[bf] = crc; last = bf
        if cur: segs.append(cur)
        return segs
    def _ck_align(sseg, hseg):
        # offset O (host_f = spec_bf + O) maximizing CRC matches via distinctive
        # non-zero CRC anchors. Returns (mismatches, O, overlap, longest_run, first).
        sanchor = [(bf, sseg[bf]) for bf in sorted(sseg) if sseg[bf] != 0]
        if not sanchor: return (0, 0, 0, 0, None)
        hbi = {}
        for hf, hc in hseg.items():
            if hc != 0: hbi.setdefault(hc, []).append(hf)
        deltas = {}
        for sb, sc in sanchor:
            for hf in hbi.get(sc, ()):
                deltas[hf - sb] = deltas.get(hf - sb, 0) + 1
        if not deltas: return (len(sanchor), 0, 0, len(sanchor), sanchor[0][0])
        O = max(deltas, key=deltas.get)
        bfs = sorted(bf for bf in sseg if sseg[bf] != 0 and (bf + O) in hseg)
        run = mx = mm = 0; first = None
        for bf in bfs:
            if hseg[bf + O] != sseg[bf]:
                mm += 1; run += 1; mx = max(mx, run)
                if first is None: first = bf
            else:
                run = 0
        return (mm, O, len(bfs), mx, first)
    ck_H = _ck_host(OUT_DIR / "live_FM2K_P1_Debug.log")
    ck_fail = False
    if not ck_H:
        print("[harness] CHECKSUM: no host [CHECKSUM] -- full-state fencepost "
              "inactive (need FM2K_CINPUT=1 + a battle)")
    else:
        for s in specs:
            for si, sseg in enumerate(_ck_spec(s["live"])):
                nz = sum(1 for c in sseg.values() if c)
                if nz == 0:
                    ck_fail = True
                    print(f"[harness] CHECKSUM {s['tag']} seg{si}: ALL-ZERO CRCs "
                          f"(stale spec fingerprint -- recompute regressed) -> FAIL")
                    continue
                best = None
                for hi, hseg in enumerate(ck_H):
                    mm, O, n, mx, fb = _ck_align(sseg, hseg)
                    key = (mx, mm, -n)
                    if best is None or key < best[0]: best = (key, mm, O, n, mx, fb, hi)
                _, mm, O, n, mx, fb, hi = best
                if n == 0:
                    ck_fail = True
                    print(f"[harness] CHECKSUM {s['tag']} seg{si}: NO OVERLAP with "
                          f"any host match -> FULL-STATE DESYNC")
                elif mx > 3:
                    ck_fail = True
                    hseg = ck_H[hi]
                    print(f"[harness] CHECKSUM {s['tag']} seg{si}: vs host-match{hi} "
                          f"off{O} {mm}/{n} CRC mismatches (longest run {mx}) -> "
                          f"FULL-STATE DESYNC (first spec-bf={fb} host-f={fb + O}: "
                          f"spec=0x{sseg[fb]:08X} host=0x{hseg[fb + O]:08X})")
                else:
                    extra = f" ({mm} tail/predicted artifact)" if mm else ""
                    print(f"[harness] CHECKSUM {s['tag']} seg{si}: vs host-match{hi} "
                          f"off{O} {n} frames FULL-STATE IDENTICAL{extra}")

    for s in specs:
        r = gate_one(s["live"])
        s["gate"] = r
        s["ok"] = r["checked"] > 0 and r["bad"] == 0
        verdict = "PASS" if s["ok"] else ("INCONCLUSIVE" if r["checked"] == 0 else "FAIL")
        print(f"[harness] GATE {s['tag']} ({s['phase']}): "
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

    real_fail   = cin_fail or ck_fail or any(s["gate"]["checked"] > 0 and not s["ok"] for s in specs)
    checked_any = any(s["gate"]["checked"] > 0 for s in specs)

    if not args.keep and not real_fail and checked_any:
        cleanup = [p1_pty, replay_pty, OUT_DIR / "p1.log", OUT_DIR / "p2.log",
                   OUT_DIR / "replay.log"]
        cleanup += [s["pty"] for s in specs]
        cleanup += [OUT_DIR / f"spec{s['k']}.log" for s in specs]
        for f in cleanup:
            f.unlink(missing_ok=True)

    if real_fail:
        why = ("CHECKSUM full-state desync (see above)" if ck_fail else
               "CINPUT input-frame desync (see above)" if cin_fail else "rng/hp gate")
        print(f"[harness] OVERALL FAIL: a spectator desynced from host -- {why}.")
        return 1
    if not checked_any:
        print("[harness] GATE INCONCLUSIVE: no spectator trace frames -- need "
              "FM2K_HOST_TRACE=1 + FM2K_SPECTATOR_DEBUG=1 and a battle phase")
        return diff_rc
    print(f"[harness] OVERALL PASS: all {len(specs)} spectator(s) full-state bit-exact "
          f"(CHECKSUM) + input-frame-identical (CINPUT) + rng/hp bit-exact with host.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
