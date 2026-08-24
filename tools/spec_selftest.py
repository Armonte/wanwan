#!/usr/bin/env python3
"""Live-spectator determinism self-test.

Spins up THREE launcher instances on 127.0.0.1:

  P1   --host    <game> --port 7000   (autoplay, parity -> p1_parity.pty)
  P2   --connect 127.0.0.1:7000 --port 7001   (autoplay)
  SPEC --spectate 127.0.0.1:7000 --port 7002  (passive viewer,
       parity -> spec_parity.pty, joins during title/CSS so the host
       backfills from the session start -- the no-prior-match case)

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
    # Parallel-run clone (robocopy of wanwan; per-dir logs let a second run
    # coexist -- pair with FM2K_TEST_PORT_BASE + FM2K_TEST_NO_KILL=1 +
    # FM2K_TEST_OUT_DIR).
    "wanwanb": Path("/mnt/c/games/2dfm/wanwan_b/WonderfulWorld_ver_0946.exe"),
    "vanpri": Path("/mnt/c/games/2dfm/vanguard-princess/vanpri.exe"),
    "urorfg": Path("/mnt/c/games/2dfm/URORFG Release 1 0 2/URORFGRelease102.exe"),
    # FM95 engine (Comic Party Wars) -- the launcher sniffs the engine from
    # the exe and injects FM95Hook.dll. Full-width filename is the real one.
    "cpw":    Path("/mnt/c/dev/fm95/CPW/ＣＰＷ.exe"),
    # Lives in the user's D: library. Useful for random-stage work: unlike
    # wanwan (2 stages) it has a real stage table, so rolls are distinguishable.
    "pkmncc": Path("/mnt/d/Games/fm2k/_NODEV/pkmncc/pkmncc.exe"),
}
GAME_EXE = GAMES["wanwan"]   # default; overridden by --game in main()
# Engine detection, harness-side (Phase 4e, review A4a(ii)). Only the FM95/CPW
# registry entry builds against ENGINE_FM95, where ParityPool has no FM2K object
# pool to scan and ComputeTopology() returns the documented `0` not-computed
# sentinel by construction. On FM2K that sentinel means the gate did not run and
# is a FAIL; on FM95 it is expected, so the POOL terms advisory-skip there with a
# loud line. Set in main() from --game; the default is FM2K.
FM95_GAMES  = {"cpw"}
IS_FM95_RUN = False
OUT_DIR  = Path(os.environ.get("FM2K_TEST_OUT_DIR",
                               "/mnt/c/dev/wanwan/tools/.spec_selftest"))
PARITY_DIFF = Path(__file__).parent / "parity_diff.py"
# Parallel-run support: FM2K_TEST_PORT_BASE relocates every port this run
# uses (spectator TCP listeners derive from FM2K_LOCAL_PORT hook-side, so
# they follow automatically). Two concurrent runs need distinct bases AND
# distinct --game dirs (per-dir logs) AND FM2K_TEST_NO_KILL=1 on both
# (kill_strays kills by IMAGE NAME and would murder the sibling run).
_PORT_BASE = int(os.environ.get("FM2K_TEST_PORT_BASE", "7000"))
P1_PORT, P2_PORT, SPEC_PORT = _PORT_BASE, _PORT_BASE + 1, _PORT_BASE + 2
# Fake-spectator UDP ports start at 7200 -- clear of the real spec range (7002+)
# AND the host's TCP listener (tries udp+100 = 7100 first). Fakes dial whatever
# TCP port the JOIN_ACK reports, so no fake TCP bind collision.
# Fakes run as WINDOWS processes (so 127.0.0.1 reaches the Windows-side host;
# NAT-mode WSL2 loopback would not). Windows Python on the test box.
WIN_PYTHON = r"C:\Program Files\Python313\python.exe"


def to_win(p: Path) -> str:
    s = str(p)
    if s.startswith("/mnt/") and len(s) > 6 and s[6] == "/":
        return s[5].upper() + ":" + s[6:]
    return s


def kill_strays():
    # Parallel runs: image-name taskkill cannot distinguish sibling runs.
    if os.environ.get("FM2K_TEST_NO_KILL") == "1":
        return
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
    #
    # FM2K_TEST_BACKGROUND: ON BY DEFAULT FOR EVERY HARNESS PROCESS. A gate run
    # spawns 2-5 launcher windows plus 2-5 game windows; each one taking the
    # foreground makes the machine unusable for the length of the run. Injected
    # HERE rather than in the per-role env dicts on purpose -- this is the single
    # funnel every P1/P2/spectator/REPLAY launch goes through, so no role can be
    # forgotten. The launcher reads it for its own SDL window AND for the
    # STARTUPINFO it spawns the game with; the injected hook reads the same
    # variable for its user32 detours.
    #   Watch a run:  FM2K_TEST_BACKGROUND=0 python3 tools/spec_selftest.py ...
    env_extra = dict(env_extra)
    env_extra.setdefault("FM2K_TEST_BACKGROUND",
                         os.environ.get("FM2K_TEST_BACKGROUND", "1"))
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
# Battle frames the spectator may legitimately trail -- and, symmetrically, may
# appear AHEAD by (see spectator_liveness for why the bound is two-sided).
# Overridable so a profile whose host hard-terminates while a viewer is still
# draining a legitimately deep delay bank can be gated at a looser bound
# (the deep-join stage of run_all_tests does exactly that) without every other
# caller losing the tight default.
LIVE_EDGE_TOLERANCE = int(os.environ.get("FM2K_LIVE_EDGE_TOLERANCE", "100"))


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


def spectator_liveness(host_log, spec_log, tolerance=LIVE_EDGE_TOLERANCE,
                       first_host_match=0):
    """Did the spectator reach & HOLD near the host's final battle frame?

    `first_host_match` = index of the EARLIEST host match this viewer could
    possibly have observed (0 = it was in the session from the start). It exists
    for the BOUNDED between-matches deep join: a viewer that dials in
    BETWEEN matches is served a backfill anchored at the CURRENT char-select and
    therefore has legitimately FEWER battle segments than the host -- that is the
    entire point of the feature. The old rule
    `len(spec_segs) >= len(host_segs)` failed such a viewer BY CONSTRUCTION
    whatever its measured gap (wave-3 report 5.1: gap 8/12/8 against a tolerance
    of 100, all three marked FAIL), which made --assert-spectator-live unusable
    on the very path it most needs to watch. A bounded joiner is judged instead
    on the segments it could observe: host matches [first_host_match ..].

    The "fell a whole match behind" case that `followed_all` used to catch is now
    caught by an AHEAD bound. A viewer one match behind is sitting on an EARLIER
    host match, whose length is unrelated to (and in practice much larger than)
    the host's final match, so its bf OVERSHOOTS the host's final bf: measured
    gap -883..-891 for the from-frame-0 joiner that never reached the match it
    was sent to watch, vs +7..+141 for a bounded joiner riding the live edge.

    That bound is applied ONLY when being a match behind is actually possible --
    the viewer produced FEWER segments than the host. Over 222 archived
    spectator logs, 6 legitimately-passing single-match runs sit 182-193 frames
    "ahead" because the host's debug log was truncated mid-flush by
    TerminateProcess while the viewer had already applied the frames it was fed;
    with one host match there is no earlier match to be stuck on, so those must
    not fail. (The existing re-preserve-and-re-parse backstop in main() is the
    other half of that defence.)

    Returns a dict; `reached` (bool) is the pass condition. Also imported and
    called by tools/desync_seed_sweep.py, so it must stay side-effect free."""
    host_segs = _cinput_seg_maxbf(host_log)
    spec_segs = _cinput_seg_maxbf(spec_log)
    # Host's final battle frame = furthest bf in its LAST match (fallback: the
    # single-match BATTLE STATUS max when [CINPUT] is absent).
    host_final = host_segs[-1] if host_segs else _battle_status_maxframe(host_log)
    # Clamp the baseline: it is derived from host-log markers at dial-in time and
    # can overshoot (the host may enter the next match while the viewer boots).
    # Never demand fewer than one segment, and never more than the host produced.
    base = max(0, min(int(first_host_match), max(0, len(host_segs) - 1)))
    expected_segs = max(1, len(host_segs) - base)
    followed_all = bool(spec_segs) and len(spec_segs) >= expected_segs
    # Align from the END: the viewer watched a contiguous SUFFIX of the host's
    # matches, so its LAST segment pairs with the host's LAST match. Identical to
    # the old front-indexed choice whenever the counts agree, i.e. for every
    # full-session joiner the gate's stages 2/2b run.
    spec_max = spec_segs[-1] if spec_segs else 0
    gap = (host_final - spec_max) if host_final is not None else None
    # Behind by more than the tolerance = did not hold the live edge. AHEAD of
    # the host's final bf only matters when the viewer has fewer segments than
    # the host, i.e. when its last segment could be an EARLIER, longer match.
    could_be_a_match_behind = len(spec_segs) < len(host_segs)
    reached = bool(followed_all and gap is not None and gap <= tolerance
                   and (gap >= -tolerance or not could_be_a_match_behind))
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
                tolerance=tolerance, expected_segs=expected_segs,
                first_host_match=base)


# ---------------------------------------------------------------------------
# CATCHUP metric -- wall clock from a spectator process's first log line to the
# first frame it actually PLAYS. This is the number the deep-join rollout
# reports to humans ("I clicked spectate and I was watching in N seconds",
# docs/dev/spectate_deep_join_rollout.md phases 2/3), and it is deliberately
# NOT the live-edge gap: the gap says how far behind live the viewer settles,
# this says how long the user stared at a black window first.
#
# first played frame = the first [SPEC-Q] 1 Hz heartbeat carrying total>0 (the
# cumulative pop counter -- the viewer has applied at least one event). That
# heartbeat is unconditional (trampoline_spectator.cpp), so the metric needs no
# extra env. Fallback when it is absent: the first [CINPUT] line, which is
# BATTLE-only and therefore reads much later (a deep joiner measured 2.3s to
# first played frame but 13.1s to battle) -- reported as source="cinput" and
# NOT gated, so a threshold is never silently applied to the wrong metric.
# ---------------------------------------------------------------------------
DEEP_JOIN_CATCHUP_SECS = 10.0   # lenient default: measured deep joins are 2.0-3.1s


def spectator_catchup(spec_log):
    """boot -> first-played-frame wall clock for one spectator log.

    Returns dict(boot, first_frame, seconds, source, applied_seconds); every
    value is None when the log does not carry the corresponding marker. Side-
    effect free (same contract as spectator_liveness)."""
    import re
    ts_pat = re.compile(r'^\[(\d+):(\d+):(\d+)\.(\d+)\]')
    q_pat  = re.compile(r'\[SPEC-Q\] .*total=(\d+)')

    def _ts(ln):
        m = ts_pat.match(ln)
        if not m:
            return None
        h, mi, sc, ms = (int(x) for x in m.groups())
        return h * 3600 + mi * 60 + sc + ms / 1000.0

    boot = first = applied = first_cinput = None
    source = None
    try:
        fh = open(spec_log, errors="ignore")
    except OSError:
        return dict(boot=None, first_frame=None, seconds=None, source=None,
                    applied_seconds=None)
    for ln in fh:
        t = _ts(ln)
        if t is None:
            continue                      # the log's un-timestamped banner
        if boot is None:
            boot = t
        if first is None:
            m = q_pat.search(ln)
            if m and int(m.group(1)) > 0:
                first, source = t, "spec-q"
        if first_cinput is None and "[CINPUT] bf=" in ln:
            first_cinput = t
        if applied is None and "[SPEC-DEEPJOIN] snapshot APPLIED at anchor=" in ln:
            applied = t
    if first is None and first_cinput is not None:
        first, source = first_cinput, "cinput"

    def _delta(t):
        if t is None or boot is None:
            return None
        d = t - boot
        return d + 86400.0 if d < 0 else d        # midnight wrap
    return dict(boot=boot, first_frame=first, seconds=_delta(first),
                source=source, applied_seconds=_delta(applied))


# ---------------------------------------------------------------------------
# MACHINE-STALL DETECTOR + the VOID verdict (H2, 2026-08-18).
#
# THE HONESTY RULE THIS EXISTS FOR. On 2026-08-18 a soak leg was scored as a
# product FAIL ("the viewers starved") when what actually happened was that a
# ~7.3 second whole-machine stall froze ALL FOUR game processes simultaneously:
# P1 and P2 stopped logging at 03:02:21.149 and 03:02:21.150 -- one millisecond
# apart, both for 6.26s, both at the same CSS frame -- while a viewer holding
# 562 buffered events went 7.33s between consecutive [CHECKSUM] lines. Network
# starvation cannot produce that; only "the process did not run" can. The soak
# must be able to say "this run measured nothing" without lying in either
# direction, so a run whose processes did not run is VOID: not a PASS, not a
# FAIL.
#
# THE DETECTOR IS FAIL-CLOSED AT BOTH ENDS and it is deliberately NOT silent on
# a clean run -- this campaign has learned to distrust a counter that only
# speaks when it is angry, so the MACHINE line prints on every run, green or
# red. The cycle-3 control (same recipe, same binaries, same machine, 27 minutes
# earlier, PASS) prints `MACHINE: 0 stall(s)`.
#
# A gap is only a MACHINE stall if it overlaps a gap in AT LEAST 3 processes.
# The four processes are independent OS processes with independent sockets and
# independent sim loops; there is no product mechanism by which three of them
# stop logging in the same 60ms window. One process going quiet is a product
# question and is left to the product terms.
MACHINE_GAP_MIN_S = 1.5      # per-process log-silence gap worth recording
MACHINE_MIN_PROCS = 3        # gaps must coincide across >= this many processes
MACHINE_VOID_PCT  = 3.0      # cumulative machine-stall time above this % -> VOID
# Max spread between the ONSETS of the gaps in one cluster. A machine stall hits
# every process at once (the 2026-08-18 capture: P1 and P2 one millisecond
# apart); a causal chain through the product is staggered by the downstream
# processes' buffers, which for a viewer is its whole delay bank -- seconds.
# 0.25s is 250x the measured coincidence and 12x smaller than the smallest
# bank-driven stagger a 300-frame bank can produce. See machine_stall_report.
MACHINE_ONSET_MAX_S = 0.25


def _log_ts_series(path):
    """(first_ts, last_ts, [gaps]) for one process log, seconds-since-midnight.
    gaps = [(start, dur)] for every log-silence gap > MACHINE_GAP_MIN_S."""
    import re
    pat = re.compile(r'^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]')
    first = last = None
    gaps = []
    prev = None
    try:
        fh = open(path, errors="replace")
    except OSError:
        return None, None, gaps
    for ln in fh:
        m = pat.match(ln)
        if not m:
            continue
        h, mi, s, ms = (int(x) for x in m.groups())
        t = h * 3600 + mi * 60 + s + ms / 1000.0
        if prev is not None:
            if t < prev - 43200:      # midnight wrap
                t += 86400.0
            d = t - prev
            if d > MACHINE_GAP_MIN_S:
                gaps.append((prev, d))
        if first is None:
            first = t
        prev = last = t
    return first, last, gaps


def _viewer_queue_before(path, t):
    """Last `[SPEC-Q] ... q=N` depth logged at or before time `t` (seconds since
    midnight). Returns None when the viewer never printed one before `t`."""
    import re
    ts = re.compile(r'^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]')
    qp = re.compile(r'\[SPEC-Q\] mode=\d+ q=(\d+)')
    q = None
    try:
        fh = open(path, errors="replace")
    except OSError:
        return None
    for ln in fh:
        m = ts.match(ln)
        if not m:
            continue
        h, mi, s, ms = (int(x) for x in m.groups())
        lt = h * 3600 + mi * 60 + s + ms / 1000.0
        if lt > t:
            break
        mq = qp.search(ln)
        if mq:
            q = int(mq.group(1))
    return q


def machine_stall_report(out_dir, host_gone_ms=None):
    """Detect whole-machine stalls across every preserved per-process log.

    Returns a dict. `void` is the verdict: True means the run measured nothing
    and must be scored neither PASS nor FAIL. Side-effect free (offline-usable
    against any archived dir of live_FM2K_*_Debug.log).

    WHAT SEPARATES A MACHINE STALL FROM A PRODUCT HANG (adversarial review H5,
    2026-08-18). The first version of this detector justified itself with "four
    independent OS processes with independent sockets and independent sim loops
    cannot stop logging together for any product reason". THAT PREMISE IS FALSE
    for this harness's topology: P2 is input-coupled to P1 (a host that stops
    producing inputs stalls the guest by construction) and both viewers are FED
    by P1, so a single host-side wedge -- a broken barrier, a CSS lockstep
    rendezvous, a deadlock -- silences all four planes in a CAUSAL CHAIN. With
    only an overlap test and a transitive interval merge, that chain looked
    exactly like a machine stall: a real, reproducible product deadlock would
    have VOIDed (hiding CINPUT, CHECKSUM, PVP, CSS-SPEC and every other term,
    because the VOID check runs before any of them) and, on the second VOID,
    ended a soak with the message "your hardware is broken".

    Three rules implement the distinction, and each is keyed to a measured
    property of the 2026-08-18 capture rather than to an assertion:

      1. ONSET COINCIDENCE. A machine stall descends on every process at the
         same instant -- the capture's P1/P2 onsets were ONE MILLISECOND apart.
         A causal chain is staggered by however long each downstream process
         can keep going on what it already holds, which for a viewer is its
         delay bank: SECONDS. Cluster members must therefore start within
         MACHINE_ONSET_MAX_S of each other.
      2. NO TRANSITIVE MERGE. Chained overlap -- A overlaps B, B overlaps C,
         A and C do not -- is the causal-chain signature itself, and the old
         merge accumulated exactly that into one cluster with three tags.
         Clusters are now built around a seed gap and accept only members whose
         ONSET is inside the seed's onset window.
      3. THE VIEWER-CONTENT DISCRIMINATOR. Network starvation cannot silence a
         viewer that is holding buffered content, and neither can a host wedge
         (such a viewer keeps playing out what it has and keeps logging). A
         machine stall can, and did: the capture's S2 went quiet holding 563
         buffered events. So for every viewer in a cluster we read the last
         [SPEC-Q] queue depth before it went silent. q>0 CONFIRMS the machine;
         q==0 on every viewer member says the viewers had simply drained, which
         is what a starve or a wedge looks like, and the cluster is NOT a
         machine stall. Missing [SPEC-Q] data leaves the discriminator
         NOT COMPUTED: the cluster still counts (a run this ragged did not
         measure anything either way) but `confirmed` is False, and the soak
         driver must not call the machine unfit on that evidence.
    """
    from pathlib import Path as _P
    d = _P(out_dir)
    procs = {}
    for p in sorted(d.glob("live_FM2K_*_Debug.log")):
        tag = p.name[len("live_FM2K_"):-len("_Debug.log")]
        first, last, gaps = _log_ts_series(p)
        if first is None:
            continue
        procs[tag] = dict(first=first, last=last, gaps=gaps, path=p)
    if len(procs) < MACHINE_MIN_PROCS:
        # NOT-COMPUTED must never read as a clean bill of health.
        return dict(computed=False, procs=procs, stalls=[], n=0, worst=0.0,
                    total=0.0, wall=0.0, pct=0.0, void=False, confirmed=False,
                    reason=(f"only {len(procs)} process log(s) present -- need "
                            f"{MACHINE_MIN_PROCS} to tell a machine stall from a "
                            f"process-local one"))
    wall = max(p["last"] for p in procs.values()) - \
           min(p["first"] for p in procs.values())
    # Rules 1+2: ONSET-COINCIDENT clustering. Seeded, never transitive.
    flat = sorted(((s, s + dur, tag, dur)
                   for tag, p in procs.items() for (s, dur) in p["gaps"]),
                  key=lambda g: g[0])
    clusters, used = [], [False] * len(flat)
    for i, seed in enumerate(flat):
        if used[i]:
            continue
        members, tags = [seed], {seed[2]}
        used[i] = True
        for j in range(i + 1, len(flat)):
            if used[j]:
                continue
            g = flat[j]
            if g[0] - seed[0] > MACHINE_ONSET_MAX_S:
                break                      # flat is onset-sorted
            if g[2] in tags:
                continue                   # one gap per process per cluster
            members.append(g); tags.add(g[2]); used[j] = True
        clusters.append(dict(start=seed[0], members=members))
    stalls = []
    for c in clusters:
        tags = sorted({m[2] for m in c["members"]})
        if len(tags) < MACHINE_MIN_PROCS:
            continue
        # Rule 3: the viewer-content discriminator.
        held, drained, unknown = [], [], []
        for m in c["members"]:
            tag = m[2]
            if not tag.startswith("S"):
                continue                   # players have no playback queue
            q = _viewer_queue_before(procs[tag]["path"], m[0])
            (unknown if q is None else (held if q > 0 else drained)).append(
                (tag, q))
        if held:
            disc = ("MACHINE-CONFIRMED: " +
                    ", ".join(f"{t} still held {q} queued frames when it went "
                              f"silent" for t, q in held))
            confirmed = True
        elif drained and not unknown:
            # Every viewer in the cluster had already drained: a starve or a
            # product wedge looks exactly like this, a machine stall does not.
            continue
        else:
            disc = ("discriminator NOT COMPUTED: no [SPEC-Q] depth available "
                    "for the viewer member(s) before the gap"
                    if unknown or not drained else "")
            confirmed = False
        onsets = [m[0] for m in c["members"]]
        stalls.append(dict(start=c["start"], tags=tags,
                           dur=max(m[3] for m in c["members"]),
                           onset_spread=max(onsets) - min(onsets),
                           confirmed=confirmed, discriminator=disc,
                           members=sorted(c["members"], key=lambda m: m[0])))
    total = sum(s["dur"] for s in stalls)
    worst = max((s["dur"] for s in stalls), default=0.0)
    pct = (100.0 * total / wall) if wall > 0 else 0.0
    reasons = []
    # Fail-closed rule 1: a single stall long enough to have KILLED a viewer.
    # Keyed to the give-up budget the run actually ran with, minus a 1s margin.
    if host_gone_ms:
        lethal = (host_gone_ms - 1000) / 1000.0
        if worst > lethal:
            reasons.append(f"worst stall {worst:.2f}s exceeds the run's "
                           f"host-gone budget minus 1s ({lethal:.1f}s)")
    # Fail-closed rule 2: the machine is not fit to measure with, even if no
    # single stall was lethal.
    if pct > MACHINE_VOID_PCT:
        reasons.append(f"cumulative machine-stall time {total:.1f}s is "
                       f"{pct:.1f}% of the {wall:.1f}s run (> {MACHINE_VOID_PCT}%)")
    return dict(computed=True, procs=procs, stalls=stalls, n=len(stalls),
                worst=worst, total=total, wall=wall, pct=pct,
                confirmed=any(s["confirmed"] for s in stalls),
                void=bool(reasons), reason="; ".join(reasons))


def machine_stall_lines(rep):
    """The ALWAYS-PRINTED [harness] MACHINE line(s) for a report."""
    out = []
    if not rep["computed"]:
        out.append(f"[harness] MACHINE: NOT COMPUTED -- {rep['reason']}")
        return out
    # Per-process silence totals, so the reader sees the same numbers the
    # diagnosis quotes ("P1 lost 16.1s of 264.7s") next to the cluster verdict.
    per = []
    for tag in sorted(rep["procs"]):
        g = rep["procs"][tag]["gaps"]
        if g:
            per.append(f"{tag} {len(g)} gap(s)/{sum(d for _, d in g):.1f}s")
    out.append(f"[harness] MACHINE: {rep['n']} stall(s) >{MACHINE_GAP_MIN_S}s "
               f"across >={MACHINE_MIN_PROCS} of {len(rep['procs'])} processes, "
               f"total {rep['total']:.1f}s ({rep['pct']:.1f}% of "
               f"{rep['wall']:.1f}s run), worst {rep['worst']:.2f}s"
               + (f"; per-process log silence: {', '.join(per)}" if per else ""))
    for s in rep["stalls"]:
        hh = int(s["start"] // 3600) % 24
        mm = int(s["start"] // 60) % 60
        ss = s["start"] % 60
        out.append(f"[harness]   stall at {hh:02d}:{mm:02d}:{ss:06.3f} "
                   f"{s['dur']:.2f}s across {'/'.join(s['tags'])} "
                   f"(onset spread {s['onset_spread'] * 1000:.0f}ms)")
        if s["discriminator"]:
            out.append(f"[harness]     {s['discriminator']}")
    return out


# ---------------------------------------------------------------------------
# LIVE-EDGE AXIS (H3, 2026-08-18).
#
# The LIVE-EDGE term compared the viewer's max battle frame against the host's
# FINAL battle frame. When the viewer exits FIRST that is a category error, and
# it fabricated the number a whole triage was then reasoned from:
# `host_final_frame=1263 spec_max_frame=4639 gap=-3376 -> FAIL`, where 1263 was
# the host's FIFTH match and 4639 the viewer's last frame of the host's SECOND,
# taken 136 seconds apart. The two numbers were never on a comparable axis.
def viewer_exit_axis(host_log, spec_log, early_s=5.0):
    """Was the viewer gone well before the host, and what was the host doing
    when it left? Returns dict(early, viewer_last, host_last, host_ran_more_s)."""
    _, s_last, _ = _log_ts_series(spec_log)
    _, h_last, _ = _log_ts_series(host_log)
    if s_last is None or h_last is None:
        return dict(early=False, viewer_last=s_last, host_last=h_last,
                    host_ran_more_s=None)
    more = h_last - s_last
    return dict(early=(more > early_s), viewer_last=s_last, host_last=h_last,
                host_ran_more_s=more)


def _hms(t):
    if t is None:
        return "?"
    return f"{int(t//3600)%24:02d}:{int(t//60)%60:02d}:{int(t%60):02d}"


def _css_parity_gate(out_dir, specs):
    """CSS-phase parity safety net (#66). Returns (fail: bool, lines: list[str]).

    CSS determinism was previously unverified -- every other gate is battle-bf-
    keyed. Each role emits `[CSS-FP] fr= in= cur= sel= act=` per CSS frame;
    act=p1/p2 is each player's confirm-latch. Two invariants:

      CSS-DET  host == guest, FULL payload (in/cur/sel/act), transition-exact.
               Both players run the same CSS sim on the same confirmed inputs,
               so their per-frame state must be bit-identical. THIS is the
               load-bearing check: a CSS-rollback nondeterminism shows here
               first (players diverge before any spectator can).

      CSS-SPEC per spectator, per CSS session (segment): the LOGICAL selection
               path (sel = grid cell) must agree with the host, and the locked
               char (sel at the both-confirmed act=1/1 latch) must be identical.
               The spectator applies inputs through the seam/snapshot path, so
               its raw cursor PIXELS legitimately differ (and a late snapshot-
               joiner sees only a suffix of a session) -- we compare on the sel
               cell and accept the shorter nav embedding in the longer (LCS),
               with a small seam tolerance. A real stream corruption changes the
               locked char (hard-fail) or diverges the nav path (LCS drop).
    """
    import re
    fail, lines = False, []
    _full = re.compile(r'\[CSS-FP\] fr=\d+ (in=\S+ cur=\S+ sel=\S+ act=\d+/\d+)')
    _sel  = re.compile(r'\[CSS-FP\] fr=\d+ in=\S+ cur=\S+ sel=(\S+) act=(\d+)/(\d+)')

    def full_transitions(path):
        out, prev = [], None
        try: fh = open(path, errors="ignore")
        except OSError: return out
        for ln in fh:
            m = _full.search(ln)
            if m and m.group(1) != prev:
                out.append(m.group(1)); prev = m.group(1)
        return out

    _ts = re.compile(r'^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]')

    def sessions(path):
        # per CSS session: (sel-cell nav before both-confirm, locked sel,
        # open-timestamp, LAST-[CSS-FP]-timestamp). A session ends at the first
        # act==(1,1) latch; the frozen post-confirm tail is dropped; a reset to
        # (0,0) after a latch opens the next session.
        #
        # `open_ts` and `last_ts` (seconds since midnight, None if the lines
        # carried no stamp) exist for the TRUNCATED classification below: "this
        # session opened and then the log ENDED" is a fact about the stream
        # stopping, not about the sim, and it can only be told from a real
        # desync by the clock.
        #
        # THE DISCRIMINATOR IS `last_ts`, NOT `open_ts` (adversarial review H4,
        # 2026-08-18). Keyed on the OPEN, the test asks "how long did this
        # window run?" and a viewer whose stream died 3 seconds INTO a
        # character-select window classified DEGENERATE and failed fatally --
        # a false red of exactly the class this classification exists to
        # remove. The question that separates truncation from divergence is
        # "did any more content arrive after this session's last frame?", which
        # is the distance from the LAST [CSS-FP] line to the end of the log.
        # (The archived cycle-4 corpus happens to satisfy both because its
        # stream died 75ms after the open -- which is why the original self-test
        # could not see the difference. See test_css_gate.py's late-truncation
        # injection, added with this fix.)
        segs, cur, locked, in_tail, prev = [], [], None, False, None
        open_ts = None
        last_ts = None
        try: fh = open(path, errors="ignore")
        except OSError: return segs
        for ln in fh:
            m = _sel.search(ln)
            if not m: continue
            sel, act = m.group(1), (int(m.group(2)), int(m.group(3)))
            key = (sel, act)
            if key == prev: continue
            prev = key
            mt = _ts.match(ln)
            t = None
            if mt:
                h, mi, s, ms = (int(x) for x in mt.groups())
                t = h * 3600 + mi * 60 + s + ms / 1000.0
            if t is not None: last_ts = t
            if act == (1, 1):
                if not in_tail:
                    locked = sel; in_tail = True
            elif act == (0, 0) and in_tail:
                segs.append((cur, locked, open_ts, last_ts))
                cur, locked, in_tail, open_ts = [], None, False, None
                last_ts = t
                cur.append(sel)
                if open_ts is None: open_ts = t
            elif not in_tail:
                cur.append(sel)
                if open_ts is None: open_ts = t
        segs.append((cur, locked, open_ts, last_ts))
        return [s for s in segs if s[0]]

    def lcs_len(a, b):
        if not a or not b: return 0
        prev = [0] * (len(b) + 1)
        for x in a:
            row = [0]
            for j, y in enumerate(b):
                row.append(prev[j] + 1 if x == y else max(prev[j + 1], row[j]))
            prev = row
        return prev[-1]

    H = full_transitions(out_dir / "live_FM2K_P1_Debug.log")
    if not H:
        return fail, lines   # no CSS netplay observed -- nothing to gate

    # CSS-DET: host vs guest, full-payload bit-exact.
    G = full_transitions(out_dir / "live_FM2K_P2_Debug.log")
    if G:
        if G == H:
            lines.append(f"[harness] CSS-DET host==guest bit-exact ({len(H)} transitions)")
        else:
            j = next((i for i in range(min(len(H), len(G))) if H[i] != G[i]),
                     min(len(H), len(G)))
            fail = True
            hv = H[j] if j < len(H) else "<end>"
            gv = G[j] if j < len(G) else "<end>"
            lines.append(f"[harness] CSS-DET host vs guest DIVERGED at transition {j} "
                         f"-> CSS NONDETERMINISM\n    host={hv}\n    guest={gv}")

    # CSS-SPEC: each spectator's sel-path + locked char vs the host's.
    #
    # G1 (2026-08-18) -- MADE DETERMINISTIC. What this block used to do could
    # pass by luck and fail by luck, and did both inside one campaign:
    #
    #   * It scored a TRUNCATED viewer session -- one whose process exited 75ms
    #     after the CSS window opened, so it never reached the both-confirm
    #     latch -- against a COMPLETE host session, and printed
    #     `CSS DESYNC (LOCKED CHAR host=10/5 spec=None)` about a viewer that was
    #     bit-exact on every frame it actually held. `spec=None` is not a
    #     character; it is the absence of a measurement.
    #   * It re-shopped the host-session pairing with `min()` on EVERY viewer
    #     session independently, so a viewer whose sessions had slipped by one
    #     could re-pair each one to whatever embedded best and come back green.
    #
    # The four rules below: classify before pairing, score complete-vs-complete
    # only, PIN the pairing offset once per viewer, and fail closed when nothing
    # was scorable. Coverage is printed unconditionally.
    #
    # ONE FACT REDS IN ONE PLACE. "The viewer holds fewer sessions than the
    # host" is LIVE-EDGE's verdict (H3 owns the axis and the early-exit
    # outcome). This term reports its coverage and does NOT red a second time
    # for the same fact -- a pile of correlated reds is not a diagnosis.
    Hs = sessions(out_dir / "live_FM2K_P1_Debug.log")
    for s in specs:
        Ss = sessions(s["live"])
        if not Ss:
            # V == 0 is a DIFFERENT fact from "V sessions, none scorable": this
            # viewer never entered a character-select window at all (a
            # battle-phase joiner legitimately never does). That fact belongs to
            # LIVE-EDGE / the segment counts, not here -- see the one-place rule
            # above. Rule 4's fail-closed floor covers V >= 1.
            lines.append(f"[harness] CSS-SPEC {s['tag']}: no CSS sessions observed -- skipped")
            continue
        # --- rule 1: classify every viewer session BEFORE any pairing ---------
        _, s_log_end, _ = _log_ts_series(s["live"])
        kinds = []
        for si, (snav, slk, sts, sle) in enumerate(Ss):
            if slk is not None:
                kinds.append("COMPLETE")
            elif si == len(Ss) - 1 and sle is not None and s_log_end is not None \
                    and (s_log_end - sle) <= 2.0:
                # Keyed on the session's LAST [CSS-FP], i.e. "no more content
                # arrived and then the log ended" -- see sessions() for why the
                # open timestamp was the wrong end.
                kinds.append("TRUNCATED")   # the stream stopped mid-session
            else:
                kinds.append("DEGENERATE")  # entered and LEFT without confirming
        n_trunc = kinds.count("TRUNCATED")
        n_degen = kinds.count("DEGENERATE")
        # --- rules 2+3: score complete-vs-complete, offset PINNED once --------
        pinned = None      # (viewer session ordinal, host session ordinal)
        scored = 0
        host_used = []
        for si, (snav, slk, _sts, _sle) in enumerate(Ss):
            if kinds[si] == "TRUNCATED":
                lines.append(f"[harness] CSS-SPEC {s['tag']} sess{si}: TRUNCATED "
                             f"(viewer stream ended "
                             f"{(s_log_end - Ss[si][3]) * 1000:.0f}ms after this "
                             f"window's LAST character-select frame, "
                             f"{(s_log_end - Ss[si][2]) * 1000:.0f}ms after it "
                             f"opened, before the both-confirm latch) -- "
                             f"NO VERDICT")
                continue
            if kinds[si] == "DEGENERATE":
                fail = True
                lines.append(f"[harness] CSS-SPEC {s['tag']} sess{si}: DEGENERATE "
                             f"-- the viewer entered this character-select window "
                             f"and LEFT it without ever reaching the both-confirm "
                             f"latch, mid-stream (session {si} of {len(Ss)}). That "
                             f"is a stream defect, not a truncation.")
                continue
            # WHICH host session is this spectator session? Ranked by, in order:
            # fewest non-embedded cells (the mismatch term), then longest LCS,
            # then longest common PREFIX -- all evidence terms. The old key was
            # non_embed alone, and non_embed TIES AT 0 for every host session
            # whenever the spectator segment is short (a trailing char-select
            # the host hard-terminated through embeds in anything), so min()
            # silently handed back host-sess0. That is how a run with TWENTY
            # char-selects reported "sess19 ... vs host-sess0 -> CSS DESYNC
            # (LOCKED CHAR host=2/17 spec=None)" and pointed every reader at
            # the wrong end of the session (campaign
            # docs/dev/matchend_seam_campaign.md, Phase 2c run V3-c).
            # When the evidence terms STILL tie, the last resort is ordinal
            # proximity -- CSS sessions are strictly time-ordered on both sides,
            # so sess19 belongs next to host-sess18, never host-sess0 -- and the
            # line says AMBIGUOUS out loud rather than implying a real pairing.
            cands = []
            for hi, (hnav, hlk, _hts, _hle) in enumerate(Hs):
                L = lcs_len(hnav, snav)
                non_embed = min(len(hnav), len(snav)) - L
                pfx = 0
                for a, b in zip(hnav, snav):
                    if a != b: break
                    pfx += 1
                cands.append(((non_embed, -L, -pfx), hi, L, len(hnav), hlk))
            bkey = min(c[0] for c in cands)
            tied = [c for c in cands if c[0] == bkey]
            best = min(tied, key=lambda c: (abs(c[1] - si), c[1]))
            # RULE 3 -- PIN THE OFFSET ONCE PER VIEWER. The first COMPLETE
            # session chooses the alignment; every later one is REQUIRED to sit
            # at first_host + (si - si0). Re-shopping per session is exactly how
            # a wrong alignment could launder itself into a pass, so a
            # disagreement is now the finding, not an input to a search.
            if pinned is None:
                pinned = (si, best[1])
                hi, L, hlen, hlk = best[1], best[2], best[3], best[4]
                non_embed = bkey[0]
            else:
                want = pinned[1] + (si - pinned[0])
                if not (0 <= want < len(Hs)):
                    # The viewer holds MORE sessions past the pinned alignment
                    # than the host does. Coverage, not a desync -- reported
                    # here and owned by LIVE-EDGE. Checked BEFORE the drift test
                    # so an out-of-range expectation cannot masquerade as one.
                    lines.append(f"[harness] CSS-SPEC {s['tag']} sess{si}: host has "
                                 f"no session {want} ({len(Hs)} total) -- NO VERDICT "
                                 f"(coverage, see LIVE-EDGE)")
                    continue
                if best[1] != want:
                    fail = True
                    lines.append(f"[harness] CSS-SPEC {s['tag']}: PAIRING DRIFT: "
                                 f"sess{si} expected host-sess{want} (offset pinned "
                                 f"on sess{pinned[0]} -> host-sess{pinned[1]}) but "
                                 f"best-match is host-sess{best[1]}. The two session "
                                 f"lists are strictly time-ordered; a disagreement "
                                 f"means one side gained or lost a window, which is "
                                 f"a finding -- NOT something to re-pair around.")
                    continue
                hnav, hlk, _, _ = Hs[want]
                hi, hlen = want, len(hnav)
                L = lcs_len(hnav, snav)
                non_embed = min(hlen, len(snav)) - L
            # RULE 2 -- the host session must itself be COMPLETE. Scoring a
            # locked char against a host that never locked one is the same
            # not-a-measurement the `spec=None` branch used to print, mirrored.
            if hlk is None:
                lines.append(f"[harness] CSS-SPEC {s['tag']} sess{si}: paired "
                             f"host-sess{hi} never reached the both-confirm latch "
                             f"-- NO VERDICT (host side incomplete)")
                continue
            # RULE 5 -- a spec=None can no longer reach the DESYNC branch. The
            # classification above is the only thing standing between "the
            # viewer confirmed a different character" and "the viewer was not
            # there"; if it ever lets a None through, that is a harness bug and
            # it must raise, not print a verdict about the product.
            assert slk is not None, (
                f"CSS-SPEC {s['tag']} sess{si}: a session classified COMPLETE "
                f"has no locked char -- harness bug")
            ambig = (f" [AMBIGUOUS: {len(tied)} host sessions tie on this "
                     f"{len(snav)}-cell segment]" if len(tied) > 1 else "")
            shorter = min(hlen, len(snav))
            tol = max(2, (shorter + 49) // 50)   # ~2% seam slack (snapshot join)
            lock_ok, nav_ok = (slk == hlk), (non_embed <= tol)
            scored += 1
            host_used.append(hi)
            if lock_ok and nav_ok:
                lines.append(f"[harness] CSS-SPEC {s['tag']} sess{si}: sel-path matches "
                             f"host-sess{hi} (LCS {L}/{shorter}, lock {slk}) OK{ambig}")
            else:
                fail = True
                why = []
                if not lock_ok: why.append(f"LOCKED CHAR host={hlk} spec={slk}")
                if not nav_ok:  why.append(f"sel-path diverged ({non_embed} cells off, "
                                           f"LCS {L}/{shorter})")
                lines.append(f"[harness] CSS-SPEC {s['tag']} sess{si}: vs host-sess{hi} "
                             f"-> CSS DESYNC ({'; '.join(why)}){ambig}")
        # --- rule 4: coverage, printed ALWAYS and fail-closed at zero ---------
        if scored == 0:
            fail = True
            lines.append(f"[harness] CSS-SPEC {s['tag']}: 0 of {len(Ss)} viewer "
                         f"session(s) scorable ({n_trunc} truncated, {n_degen} "
                         f"degenerate) -- NO VERDICT COMPUTED. A term that measured "
                         f"nothing must never report a pass.")
        else:
            lines.append(f"[harness] CSS-SPEC {s['tag']}: scored {scored}/{len(Ss)} "
                         f"viewer session(s) against host sessions "
                         f"[{min(host_used)}..{max(host_used)}] of {len(Hs)} "
                         f"({n_trunc} truncated, {n_degen} degenerate)")
    return fail, lines


# The CSS-window gate is FATAL as of Wave 2 (2026-08-15).
#
# It was ADVISORY while the falling-object bug it measures was known-present and
# unfixed -- a fatal term would have reddened every run on every build and
# nobody could have told a regression from the backlog. The fix landed in
# FM2KHook/src/hooks/css_autoconfirm.cpp (the CssAutoConfirm pin now calls the
# engine's own Css_UnloadPlayerPreview before writing selected = -1, so it stops
# orphaning the outgoing preview over a char slot whose script blob is then
# replaced under it), so the diversion is gone: FALL and CSSPOOL failures, and
# the not-computed-must-fail rule, now set the run verdict like every other term.
#
# The red-proof was taken twice BEFORE the flip: offline against six archived
# runs (exactly 2 windows red per vanpri run, 0 on wanwan) and live in a vanpri
# run that produced 4 falls while every pre-existing gate called it clean.
# FM2K_CSSWIN_FATAL=0 restores the advisory behaviour for triage.
CSS_WIN_FATAL = os.environ.get("FM2K_CSSWIN_FATAL", "1") not in ("0", "")

# Tolerance, in pixels, on the FALL term's resting-level comparison. The
# measured values are exact and stable (a vanguard-princess host rests at
# 535.0 px in every window of every run; the fall peaks at 920.9-931.5 px), so
# this only guards a sub-pixel sample and costs nothing against a 385 px signal.
CSS_FALL_TOL_PX = 2.0

# What counts as a RESTING LEVEL: a pos_y the host's object holds for at least
# this fraction of the window's resolved frames (with a small absolute floor).
# The measured separation is enormous -- a resting level is held for 900+ of
# ~950 frames, and every value on the fall curve is held for exactly one -- so
# this threshold is nowhere near any observed boundary.
CSS_REST_FRACTION = 0.10
CSS_REST_MIN_FRAMES = 10

# MINIMUM WINDOW LENGTH for a FALL verdict, in resolved frames, on EACH plane.
#
# Measured artifact (Wave 2 V2a, 2026-08-15): a run whose HOST died mid-match-1
# on an unrelated player desync left the spectator with a **2-frame**
# character-select window (its truncated tail). The host's own window was long
# enough to have a resting level of {480.0}, one of the spectator's two frames
# read 535.0, and the term declared a falling object -- on a session that never
# reached a between-match character-select window at all. With the term FATAL
# that is exactly the tail-truncation noise the seamdesync gate's header warns
# about, arriving in a different gate.
#
# The floor is set from the measured separation, not guessed: every REAL window
# in the corpus is 1025-1040 frames on the spectator and 930-950 on the host,
# and the falls last 28-93 frames inside them. 120 is ~8x below the shortest
# real window and ~60x above the artifact, so it cannot mask a fall.
# A window below the floor yields NO VERDICT, exactly like a window whose host
# object never rests; the "not one pair produced a verdict" rule below still
# turns a run in which NOTHING was measured into a FAIL.
CSS_WIN_MIN_FRAMES = 120

# HOW FAR INTO A PLANE'S CAPTURE its FIRST character-select window may open and
# still count as that plane's BOOT window.
#
# Why the FALL term needs to know. The windows are paired ordinal-from-the-end,
# which is right for between-match windows but wrong when a viewer's window list
# STARTS with its boot window: the boot character-select and the between-match
# character-select are DIFFERENT FLOWS on some content, running different preview
# intro scripts at different resting levels, and the host's between-match window
# cannot bound the boot flow.
#
# Measured on kensei2023 (2026-08-17, tools/.spec_rotate_kensei2023): the css2
# viewer's only window is its boot window and got paired against the host's
# between-match window. The "fall" it produced -- 980.0 px on 29/915 frames --
# is script 109 item 1241/1244/1245, which the HOST plays at the SAME 980.0 px
# in its OWN boot window (verified frame by frame in both .pty captures); the
# host's between-match window runs script 106 and never leaves 910-930, so its
# resting level was never a valid ceiling for that window. Same shape measured
# on pkmncc: its css2 viewer's boot window sits at 770.0 px on script 52 item
# 671/672, which the host also plays at 770.0 px.
#
# 240 is ~4x above every observed boot open (kensei host 1 / spec 2, vanpri
# spec 8, wanwan spec 10, pkmncc host 16 / spec 10) and ~10x below the earliest
# observed BETWEEN-MATCH window open (2306-2345), so it separates the two
# classes with a wide margin and cannot silently reclassify a real window.
CSS_BOOT_OPEN_MAX = 240

# How far a [CSS-ANIM] record's pos_y must descend INSIDE one character-select
# window to count as a falling object, in pixels. 40 px is the value the
# orphaned-preview lane scored its whole per-game table with: the real falls are
# 480 -> 920 (vanpri) and 440 px in the smallest case, while the largest
# legitimate in-window descent of a char-bound object measured anywhere in the
# kept corpus (4 games, 12 runs, both planes) is a few px of placement settling.
# The measured margin is an order of magnitude, so the constant is not delicate.
CSS_ANIM_FALL_PX = 40


def _css_windows(snaps):
    """Contiguous runs of match_phase == 2000 in a .pty, as (lo, hi) inclusive.

    2000 is g_game_mode's character-select / results window; 3000 is battle.
    The .pty recorder captures EVERY frame -- battle-only filtering happens in
    parity_diff, not in the capture -- so this data has always been there.
    """
    out, cur = [], None
    for i, s in enumerate(snaps):
        if s["match_phase"] == 2000:
            cur = [i, i] if cur is None else [cur[0], i]
        elif cur is not None:
            out.append((cur[0], cur[1])); cur = None
    if cur is not None:
        out.append((cur[0], cur[1]))
    return out


def _css_window_gate(out_dir, specs, host_pty, multi_match_recipe=True):
    """The character-select window gate. Returns (fail: bool, lines: list[str]).

    `multi_match_recipe` is the RECIPE'S OWN INTENT (--total-frames > 0, i.e.
    FM2K_AUTO_TERMINATE_TOTAL across matches), not an observation of the run.
    It is what re-keys the NOT-APPLICABLE hatch below; see the block comment
    there for why observing the host's window count is not enough.

    Every other correctness gate in this file is battle-only: CINPUT, CHECKSUM
    and its 4c nobj=/top=/bind= POOL terms, and parity_diff all filter to
    `match_phase == 3000` segments, and [CSS-FP] (above) covers the cursor and
    the selection, not the object pool. So the window BETWEEN two matches was
    never measured on either plane -- which is where the owner-reported
    "player objects fall down on the character-select screen" bug lives.

    Two terms, one per data source:

    FALL (primary; source = the .pty captures, always present in a harness run)
        Per paired window, per player: the host's object holds a RESTING LEVEL
        -- a pos_y it sits at for most of the window (535.0 px on
        vanguard-princess, 960.0 px on wanwan) -- and the spectator's object
        must not go below it. Under the bug the spectator's leaves that level
        over the last ~29 frames of the window on an acceleration curve and
        reaches 920.9-931.5 px, i.e. ~385 px past the host's resting position
        and off the bottom of the screen.

        WHY RESTING LEVELS AND NOT A PLAIN MAXIMUM. The obvious form of this
        term -- "no spectator frame exceeds the host's maximum pos_y" (the shape
        specced in seam_p4c_fix.md 6.5) -- was implemented first and measured
        against the whole kept corpus. It is WRONG, and wanwan is where it
        breaks: in run 4b/R2 the host's OWN character-select window contains an
        object at x=0 descending at a constant 15 px/frame under script 40 for
        the entire window, reaching 13807 px. The spectator has the identical
        object, but its window is 95 records longer, so it reaches 15231 px --
        1424 px lower, which is exactly 95 frames of descent. A maximum-based
        term reds on that pure window-length artifact. Resting levels are
        length-invariant, so it does not.

        The term is ALIGNMENT-FREE by construction, which is what makes it
        trustworthy here: the windows are 940-1060 records long and differ in
        length between the planes, and rng is far too degenerate during
        character-select to key on (60-200 distinct values across ~1000 frames,
        measured -- an rng-keyed pairing of this window is noise). Windows are
        paired by ORDINAL FROM THE END (a viewer that joined mid-run lacks the
        earlier windows; the trailing ones correspond), and the open-frame rng
        is printed as an independent corroboration of that pairing.

        When the host's own object never rests -- the wanwan case above -- the
        term reports NO VERDICT for that window/player and says so, with both
        planes' maxima, rather than inventing a pass. That is a real hole, so
        it is bounded: a run in which NOTHING produced a verdict is a FAIL.

        Measured on the kept corpus (6 runs, ~40 window/player pairs): red on
        exactly the 6 known-broken windows -- V1r6 S1/S2 (920.9 px), 4b R1
        S1/S2 (931.5 px), 4b R5 S1/S2 (929.9 px) -- and no red anywhere else,
        including all three wanwan runs.

    CSSPOOL (source = the hook's [CSS-WIN] lines, FM2K_CSS_WIN=1)
        Population (nobj=) and the process-independent slot->type map digest
        (map=) sampled every 30 in-window frames on both planes, paired window
        by window from each plane's LAST window backwards (a viewer that joined
        mid-run has fewer windows than the host, and it is the trailing ones
        that correspond), then index-aligned on the in-window frame counter.
        This one IS approximate by construction, and 4c measured it at 25-29%
        divergent on a build whose battle windows were bit-identical, so it is
        reported as a MEASUREMENT, not asserted at zero: it fails only on a
        total structural absence (see below), and its mismatch counts are the
        number the fix has to move.

    NOT-COMPUTED MUST FAIL (the A4a(ii) rule, applied to both terms in their
    fatal form): a term that could not run is RED, never a vacuous green.
      * no host .pty, or no character-select frames in it -> nothing to compare
      * a spectator .pty with no character-select frames -> nothing measured
      * no window/player pair anywhere in the run produced a verdict -> the
        FALL term ran and saw nothing; that is the wanwan-shaped hole above and
        it must not read as a pass
      * no [CSS-WIN] lines on a plane -> FM2K_CSS_WIN never reached the game
      * map=0x00000000 -> ParityPool's documented "not computed" sentinel
        (ENGINE_FM95, or FM2K_CK_TOPOLOGY=0), the exact hole the POOL terms
        were shipped with; a real digest can never be 0.
      * a MULTI-MATCH recipe (--total-frames) whose HOST produced fewer than 2
        character-select windows -> the run was truncated before the window this
        gate measures could exist (see the NOT-APPLICABLE block below).
    The two host-side instrument checks (no [CSS-WIN] lines at all; every host
    sample at the map= sentinel) run on EVERY run, including one the pairing is
    not applicable to.
    """
    import io
    import re as _re
    from contextlib import redirect_stdout
    fail, lines = False, []
    adv = "" if CSS_WIN_FATAL else " [ADVISORY]"

    def _fail(msg):
        nonlocal fail
        if CSS_WIN_FATAL:
            fail = True
        lines.append(msg + adv)

    _td = str(Path(__file__).resolve().parent)
    if _td not in sys.path:
        sys.path.insert(0, _td)
    try:
        import parity_diff as _pd
    except Exception as e:                                   # pragma: no cover
        lines.append(f"[harness] CSS-WIN: parity_diff unavailable ({e}) -- "
                     "gate not run" + adv)
        return fail, lines

    def _load(p):
        # parity_diff.load prints a header line per file; keep the gate's own
        # output readable and let a missing/short capture be a None, not a raise
        # (SystemExit on a truncated .pty must not take the whole harness down
        # after a run has already produced its other verdicts).
        try:
            with redirect_stdout(io.StringIO()):
                return _pd.load(str(p))
        except (OSError, SystemExit):
            return None

    # ---- FALL ---------------------------------------------------------------
    from collections import Counter
    TOL = int(CSS_FALL_TOL_PX * 65536)

    def _ys(snaps, lo, hi, k):
        """One player's resolved pos_y over [lo,hi], raw 16.16 fixed.

        script_idx == -1 is FillPlayerSnapshot's not-found sentinel (no
        character object this frame -- normal early in a window). Those frames
        carry a zeroed player block; scoring their 0 as a position would drag
        both the resting level and the peak toward 0 and quietly blunt the term.
        """
        return [snaps[i][k]["pos_y"] for i in range(lo, hi + 1)
                if snaps[i][k]["script_idx"] != -1]

    def _rest_levels(v):
        """The pos_y values held for a large share of the window.

        y == 0 is excluded: an object that exists but has not been placed sits
        at the origin, and treating the origin as a resting level made a host
        window whose object was never placed (host rest {0.0}) red every
        spectator that HAD placed it -- a false red measured on the corpus.
        """
        thr = max(CSS_REST_MIN_FRAMES, int(CSS_REST_FRACTION * len(v)))
        return {y for y, c in Counter(y for y in v if y != 0).items() if c >= thr}

    def _is_boot(wins, j):
        """Is window `j` of this plane's list that plane's BOOT window?

        Index 0 AND opening within the first CSS_BOOT_OPEN_MAX captured frames.
        Both halves are needed: a viewer that dialled in mid-battle also has an
        index-0 window, but it opens thousands of frames in and IS a
        between-match window.
        """
        return j == 0 and wins and wins[0][0] <= CSS_BOOT_OPEN_MAX

    def _content_set(snaps, wins):
        """Every (script_idx, item_idx, pos_y) this plane rendered IN `wins`.

        The exemption key is deliberately TIGHT -- all three fields, exact
        16.16 pos_y. Measured against the corpus (2026-08-17): it exempts
        100% of the pkmncc over-ceiling frames and 16/29 of kensei's (both
        false positives), and 0 of 121 over-ceiling frames across the four
        known-broken vanpri windows, whose falling object executes script 24
        at pos_y values the host never renders. A looser (script, pos_y) key
        was tested and rejected: it eats 39 of those 93 real frames.

        `wins` scopes it to the host's own character-select windows: what a
        character-select frame may be excused by is what the host does at
        character-select, never what it does mid-battle.
        """
        out = set()
        for lo, hi in wins:
            for i in range(lo, hi + 1):
                for k in ("p1", "p2"):
                    p = snaps[i][k]
                    if p["script_idx"] != -1:
                        out.add((p["script_idx"], p["item_idx"], p["pos_y"]))
        return out

    H = _load(host_pty)
    hwin = []
    if H is None:
        _fail("[harness] CSS-WIN FALL: host parity capture missing/unreadable "
              f"({host_pty.name}) -- NOT COMPUTED, nothing to compare against")
    else:
        hwin = _css_windows(H)
        if not hwin:
            _fail("[harness] CSS-WIN FALL: host capture has no character-select "
                  "frames -- NOT COMPUTED")

    # NOT APPLICABLE vs NOT COMPUTED. A run whose HOST only ever had ONE
    # character-select window has no between-match window in it, which is the
    # only window this gate is about, and a mid-battle joiner never sees the
    # boot one. When that is a property of the RECIPE it is NOT APPLICABLE and
    # must not touch the verdict; when it is a property of the RUN -- the host
    # desynced, AVed or was killed during match 1 -- it is NOT COMPUTED and must
    # stay fatal.
    #
    # Measured 2026-08-15 (Wave 2 V4): with the term flipped fatal, base stage 2
    # (`--frames 1500`, one match, spectator dials in mid-battle) went red 4/4
    # on nothing but this rule, while stages 2b / 2d / 2d-ks / 2g -- every
    # multi-match recipe in the gate -- computed it and passed with 0 falls. A
    # term that reddens a stage which structurally cannot contain the thing it
    # measures is exactly the "redden every run so nobody can tell a regression
    # from the backlog" failure the advisory period existed to avoid.
    #
    # THE FIRST KEY WAS WRONG (Wave-2 review B4a, fixed here). It was the HOST
    # WINDOW COUNT alone, defended as "no spectator-side defect can lower it" --
    # true, and beside the point, because a HOST-side failure lowers it
    # trivially. V2a and V2d are exactly that: the host desynced in match 1 of a
    # 16000-frame multi-match recipe, so it saw one (boot) window, and the whole
    # gate would have declared itself not applicable on the runs where the host
    # died -- 3 runs in 5 at the recipe this campaign lives in. A gate that
    # switches itself off when the host fails is the shape this campaign exists
    # to kill.
    #
    # THE KEY IS NOW THE RECIPE'S OWN INTENT: --total-frames (multi-match) vs
    # --frames (single match). It is known before the run starts and no failure
    # can move it. A multi-match recipe that produced < 2 host windows is a
    # TRUNCATED RUN and reds on the not-computed rule.
    #
    # NOT APPLICABLE IS ALSO NARROWER THAN IT WAS: it now suppresses only the
    # PAIRED terms (the FALL comparison and the CSSPOOL per-window pairing),
    # which are the ones that need a between-match window. The instrument checks
    # below -- "no [CSS-WIN] lines on the host at all" and the map=0x00000000
    # sentinel -- run on EVERY run, because they answer "is this gate wired up
    # and computing anything", which is a question a single-match recipe can
    # still answer. Before this change they sat after the early return, so a
    # build with css_window.cpp missing entirely went green on any single-window
    # run.
    not_applicable = False
    if hwin and len(hwin) < 2:
        if multi_match_recipe:
            _fail(f"[harness] CSS-WIN: host has {len(hwin)} character-select "
                  f"window(s) in a MULTI-MATCH recipe (--total-frames) -- NOT "
                  f"COMPUTED. The run was truncated before a between-match "
                  f"character-select window existed (host desync / crash / kill "
                  f"in match 1); the term could not run and must not read as a "
                  f"pass")
        else:
            not_applicable = True
            lines.append(f"[harness] CSS-WIN: host has {len(hwin)} "
                         f"character-select window(s) -- FALL/CSSPOOL PAIRING "
                         f"NOT APPLICABLE (single-match recipe: no between-match "
                         f"character-select window exists in this run). The "
                         f"instrument checks below still run")

    verdicts = 0          # window/player pairs that actually produced one
    boot_suppressed = 0   # pairs refused by the boot-flavour rule
    if hwin and not not_applicable:
        lines.append(f"[harness] CSS-WIN: host has {len(hwin)} character-select "
                     f"window(s)")
        # CONTENT KEY SCOPE (review F6): the exemption set is built from the
        # host's CHARACTER-SELECT frames only, not from its whole capture. A
        # battle frame can never legitimately exempt a character-select frame,
        # and the run-wide key was 5.7x-27x broader than it needed to be.
        # Measured before tightening (2026-08-17, four corpora): restricting the
        # set changes NOTHING -- r3_pkmncc 863 -> 150 triples with exempt counts
        # 34/37/36 either way, r5_pkmncc 862 -> 150 with 39/41/40, r4_pkmncc
        # 700 -> 137 with 0/29/28, vanpri2 4658 -> 174 with 0/0/0/0. So this is
        # a free narrowing of the key, taken with the proof that it costs no
        # existing exemption.
        hcontent = _content_set(H, hwin)
        for s in specs:
            S = _load(s["pty"])
            if S is None:
                _fail(f"[harness] CSS-WIN FALL {s['tag']}: parity capture "
                      f"missing/unreadable -- NOT COMPUTED")
                continue
            swin = _css_windows(S)
            if not swin:
                _fail(f"[harness] CSS-WIN FALL {s['tag']}: no character-select "
                      f"frames captured -- NOT COMPUTED")
                continue
            npair = min(len(hwin), len(swin))
            bad = 0
            pv = 0        # verdicts THIS plane produced (review A3)
            prefused = 0  # pairs THIS plane had refused by the boot rule
            for j in range(npair):
                hj = len(hwin) - npair + j
                sj = len(swin) - npair + j
                hlo, hhi = hwin[hj]
                lo, hi   = swin[sj]
                # FLAVOUR GUARD (see CSS_BOOT_OPEN_MAX). Ordinal-from-the-end
                # pairing is correct for between-match windows, but a viewer
                # whose window list STARTS at its boot window gets that boot
                # window paired against a host window that is not the host's
                # boot -- two different character-select FLOWS, whose resting
                # levels do not bound each other. Refuse rather than invent.
                if _is_boot(swin, sj) and not _is_boot(hwin, hj):
                    boot_suppressed += 1
                    prefused += 1
                    lines.append(
                        f"[harness] CSS-WIN FALL {s['tag']} win{j} ({lo}-{hi}): "
                        f"spectator BOOT window paired against host window "
                        f"{hj} ({hlo}-{hhi}), which is not the host's boot "
                        f"window -- NO VERDICT. Boot and between-match "
                        f"character-select are different flows (different "
                        f"preview intro scripts and resting levels), so the "
                        f"host's between-match resting level cannot bound this "
                        f"one")
                    continue
                # Independent corroboration of the ordinal pairing: both planes
                # sim the same frame, so a correctly paired window opens on the
                # same rng. Annotated, never used AS the pairing -- rng is too
                # degenerate here to key on, and the verdict does not need it.
                pnote = "" if H[hlo]["rng"] == S[lo]["rng"] else " [pairing unconfirmed: open rng differs]"
                for k in ("p1", "p2"):
                    hv, sv = _ys(H, hlo, hhi, k), _ys(S, lo, hi, k)
                    if not hv or not sv:
                        lines.append(f"[harness] CSS-WIN FALL {s['tag']} win{j} "
                                     f"{k}: no resolved object on one plane "
                                     f"(host {len(hv)} / spec {len(sv)} frames) "
                                     f"-- no verdict{pnote}")
                        continue
                    if len(hv) < CSS_WIN_MIN_FRAMES or len(sv) < CSS_WIN_MIN_FRAMES:
                        # Truncated window (see CSS_WIN_MIN_FRAMES). Reported so
                        # a corpus of nothing but short windows is visible, but
                        # never a verdict in either direction.
                        lines.append(f"[harness] CSS-WIN FALL {s['tag']} win{j} "
                                     f"{k}: window too short to judge "
                                     f"(host {len(hv)} / spec {len(sv)} resolved "
                                     f"frames, floor {CSS_WIN_MIN_FRAMES}) -- no "
                                     f"verdict, truncated session{pnote}")
                        continue
                    hl = _rest_levels(hv)
                    if not hl:
                        lines.append(
                            f"[harness] CSS-WIN FALL {s['tag']} win{j} {k}: the "
                            f"HOST's own object never rests in this window "
                            f"(host max {max(hv)/65536.0:.1f} px, spec max "
                            f"{max(sv)/65536.0:.1f} px) -- no verdict, the "
                            f"resting-level term cannot run here{pnote}")
                        continue
                    verdicts += 1
                    pv += 1
                    ceil = max(hl)
                    rest = ",".join(f"{y/65536.0:.1f}" for y in sorted(hl))
                    # CONTENT EXEMPTION (see _content_set). An over-ceiling
                    # spectator frame whose exact (script_idx, item_idx, pos_y)
                    # the HOST also renders in one of its OWN character-select
                    # windows is the SAME animation dwelled on longer, not a
                    # falling object. A real fall is a script executing to
                    # positions the host never reaches, so it exempts nothing.
                    over_all, over_ex = 0, 0
                    for i in range(lo, hi + 1):
                        p = S[i][k]
                        if p["script_idx"] == -1 or p["pos_y"] <= ceil + TOL:
                            continue
                        over_all += 1
                        if (p["script_idx"], p["item_idx"],
                                p["pos_y"]) in hcontent:
                            over_ex += 1
                    over = over_all - over_ex
                    if over_ex and not over:
                        lines.append(
                            f"[harness] CSS-WIN FALL {s['tag']} win{j} "
                            f"({lo}-{hi}) {k}: host rests at {{{rest}}} px, "
                            f"spectator max {max(sv)/65536.0:.1f} px on "
                            f"{over_all} over-ceiling frame(s) -- ALL are "
                            f"(script,item,pos_y) triples the HOST also renders "
                            f"in its OWN character-select windows, i.e. the "
                            f"same animation dwelled on longer -> OK "
                            f"(content-exempt){pnote}")
                        continue
                    if over:
                        bad += 1
                        if CSS_WIN_FATAL:
                            fail = True
                        peak = max(sv)
                        at = next(i for i in range(lo, hi + 1)
                                  if S[i][k]["script_idx"] != -1
                                  and S[i][k]["pos_y"] == peak)
                        lines.append(
                            f"[harness] CSS-WIN FALL {s['tag']} win{j} ({lo}-{hi}) "
                            f"{k}: host rests at {{{rest}}} px, spectator peaks "
                            f"{peak/65536.0:.1f} px on {over}/{len(sv)} frames "
                            f"({over_ex} further over-ceiling frame(s) "
                            f"content-exempt) -> CSS FALLING OBJECT (near spec "
                            f"idx {at}){pnote}" + adv)
                    else:
                        lines.append(
                            f"[harness] CSS-WIN FALL {s['tag']} win{j} ({lo}-{hi}) "
                            f"{k}: host rests at {{{rest}}} px, spectator max "
                            f"{max(sv)/65536.0:.1f} px -> OK{pnote}")
            # PER-PLANE NOT-COMPUTED (review A3). The global `verdicts == 0`
            # guard below never fires when ANOTHER plane produced verdicts, so a
            # viewer whose every window was refused used to print "N paired
            # window(s), 0 with a falling object -> PASS" -- a vacuous green on
            # the exact plane a mid-CSS joiner is measured on. Measured on the
            # kept corpus: 4 of 6 (p4f_runs/wanwan, orphan r3/r4/r5_pkmncc) did
            # exactly that on their css2 viewer. The rule this file already
            # states run-wide ("a term that saw nothing must not read as a
            # pass") is now enforced per plane.
            #
            # Two shapes, deliberately different verdicts:
            #   * every pair REFUSED by the boot-flavour rule -> the pairing was
            #     structurally declined, like the single-match NOT-APPLICABLE
            #     hatch. Honestly SKIPPED, not a pass and not a red.
            #   * pairs were attempted and none produced a verdict (no host
            #     resting level / too short / no object) -> the term ran on this
            #     plane and saw nothing: RED, the same call the run-wide rule
            #     makes.
            if pv == 0:
                if prefused and prefused == npair:
                    lines.append(
                        f"[harness] CSS-WIN FALL {s['tag']}: {npair} paired "
                        f"window(s) ({len(swin)} spectator / {len(hwin)} host), "
                        f"ALL {prefused} refused on the boot-flavour pairing "
                        f"rule -> NO VERDICT for this plane (not computed, "
                        f"honestly skipped -- NOT a pass)")
                else:
                    _fail(f"[harness] CSS-WIN FALL {s['tag']}: {npair} paired "
                          f"window(s) ({len(swin)} spectator / {len(hwin)} "
                          f"host) and NOT ONE produced a verdict "
                          f"({prefused} refused on the boot-flavour rule, the "
                          f"rest had no host resting level / too few frames) "
                          f"-- NOT COMPUTED on this plane. A term that saw "
                          f"nothing must not read as a pass")
            else:
                lines.append(f"[harness] CSS-WIN FALL {s['tag']}: {npair} paired "
                             f"window(s) ({len(swin)} spectator / {len(hwin)} host), "
                             f"{pv} judged"
                             + (f", {prefused} refused (boot flavour)" if prefused else "")
                             + f", {bad} with a falling object"
                             + (" -> FAIL" + adv if bad else " -> PASS"))
        if boot_suppressed:
            lines.append(f"[harness] CSS-WIN FALL: {boot_suppressed} "
                         f"window(s) refused on the boot-flavour pairing rule "
                         f"(spectator boot window vs host non-boot window)")
        if verdicts == 0:
            _fail("[harness] CSS-WIN FALL: not one window/player pair produced a "
                  "verdict"
                  + (f" ({boot_suppressed} window(s) refused on the "
                     f"boot-flavour pairing rule, the rest had no host resting "
                     f"level)"
                     if boot_suppressed
                     else " (no host resting level anywhere)")
                  + " -- NOT COMPUTED. A term that saw nothing must not read as "
                    "a pass")

    # ---- CSSPOOL ------------------------------------------------------------
    _wpat = _re.compile(
        r'\[CSS-WIN\] win=(\d+) i=(\d+) seq=\d+ tv=(\d+) nobj=(\d+) '
        r'map=0x([0-9A-Fa-f]{8}) bind=0x([0-9A-Fa-f]{8})')
    _CSS_WIN_VER = 1

    def _wrows(path):
        """{window_ordinal: {in_window_idx: (nobj, map, bind)}} + version tally."""
        out, bad_ver = {}, 0
        try:
            fh = open(path, errors="ignore")
        except OSError:
            return out, bad_ver
        for ln in fh:
            m = _wpat.search(ln)
            if not m:
                continue
            if int(m.group(3)) != _CSS_WIN_VER:
                bad_ver += 1
                continue
            out.setdefault(int(m.group(1)), {})[int(m.group(2))] = (
                int(m.group(4)), int(m.group(5), 16), int(m.group(6), 16))
        return out, bad_ver

    # INSTRUMENT CHECKS -- these run on EVERY run, including a NOT-APPLICABLE
    # one (review B4a). They do not need a between-match window or a spectator:
    # they answer "did the instrument reach the game and is it computing a real
    # digest", and a single-match recipe's boot character-select window answers
    # both. Keeping them behind the pairing was the hole that let a build with
    # the instrument missing go green on any single-window run.
    hw, hbad = _wrows(out_dir / "live_FM2K_P1_Debug.log")
    if not hw:
        _fail("[harness] CSS-WIN CSSPOOL: no [CSS-WIN] lines on the host -- "
              "NOT COMPUTED (FM2K_CSS_WIN did not reach the game, or the "
              "parity recorder never opened)"
              + (f"; {hbad} line(s) at an unknown tv=" if hbad else ""))
    elif not any(r[1] for w in hw.values() for r in w.values()):
        # Host-side form of the A4a(ii) sentinel rule: every host sample carries
        # ParityPool's documented "not computed" digest, so nothing downstream
        # can compare anything. Previously only reachable through a paired
        # window, i.e. never on a single-match recipe.
        _fail("[harness] CSS-WIN CSSPOOL: every host [CSS-WIN] sample has "
              "map=0x00000000 (ParityPool's not-computed sentinel) -> GATE "
              "INACTIVE. Cause: FM2K_CK_TOPOLOGY=0 in the game's environment, "
              "or an ENGINE_FM95 build")
    elif not_applicable:
        lines.append(f"[harness] CSS-WIN CSSPOOL: instrument present on the "
                     f"host ({sum(len(w) for w in hw.values())} sample(s) over "
                     f"{len(hw)} window(s), digests non-sentinel); per-window "
                     f"pairing NOT APPLICABLE on a single-match recipe")
    else:
        hord = sorted(hw)
        for s in specs:
            sw, sbad = _wrows(s["live"])
            if not sw:
                _fail(f"[harness] CSS-WIN CSSPOOL {s['tag']}: no [CSS-WIN] "
                      f"lines -- NOT COMPUTED")
                continue
            sord = sorted(sw)
            # Pair from the END: a viewer that joined mid-run simply lacks the
            # earlier windows, and it is the trailing ones that correspond.
            npair = min(len(hord), len(sord))
            for j in range(npair):
                ho, so = hord[len(hord) - npair + j], sord[len(sord) - npair + j]
                hrow, srow = hw[ho], sw[so]
                keys = sorted(set(hrow) & set(srow))
                nc = sum(1 for k in keys
                         if hrow[k][1] == 0 or srow[k][1] == 0)
                cmpk = [k for k in keys if hrow[k][1] and srow[k][1]]
                if not cmpk:
                    if nc:
                        _fail(f"[harness] CSS-WIN CSSPOOL {s['tag']} win{so} vs "
                              f"host-win{ho}: topology NOT COMPUTED on {nc} "
                              f"paired sample(s) (map=0x00000000 sentinel) -> "
                              f"GATE INACTIVE. Cause: FM2K_CK_TOPOLOGY=0 in the "
                              f"game's environment, or an ENGINE_FM95 build")
                    else:
                        _fail(f"[harness] CSS-WIN CSSPOOL {s['tag']} win{so} vs "
                              f"host-win{ho}: no overlapping samples -- NOT "
                              f"COMPUTED")
                    continue
                nmm = sum(1 for k in cmpk if hrow[k][0] != srow[k][0])
                mmm = sum(1 for k in cmpk if hrow[k][1] != srow[k][1])
                extra = f", {nc} not-computed excluded" if nc else ""
                lines.append(
                    f"[harness] CSS-WIN CSSPOOL {s['tag']} win{so} vs "
                    f"host-win{ho}: nobj= {nmm}/{len(cmpk)}, map= "
                    f"{mmm}/{len(cmpk)} mismatched samples "
                    f"(host {len(hrow)} / spec {len(srow)} samples{extra}) "
                    f"[measurement]")
            if sbad or hbad:
                lines.append(f"[harness] CSS-WIN CSSPOOL {s['tag']}: "
                             f"{sbad + hbad} line(s) skipped at an unknown tv= "
                             f"(build/harness [CSS-WIN] format mismatch)")

    # ---- CSSANIM: the PER-SLOT fall census (source = [CSS-ANIM] ev=rec) ------
    # WHY THIS TERM EXISTS. FALL above compares the ONE object
    # FindPlayerObjectSlot resolves per player (the first type-4 slot with a
    # matching player slot id). An orphaned preview that falls while a SIBLING
    # slot resolves as "the player object" is invisible to it: on the
    # orphaned-preview lane's RED corpus (r1_vanpri_RED, 2026-08-17) FALL
    # reported 0 falling objects across 12 paired windows on a run carrying four
    # measured 480 -> 920 px descents. A fatal term that scores 0/4 on the only
    # corpus where the bug is known to exist cannot be the assurance for it.
    #
    # [CSS-ANIM] records EVERY type-4 object in the window -- keyed on
    # (slot, owner, kind, player_slot) and closed the instant the slot stops
    # being type 4, so a record is a genuine object instance -- with y_first and
    # y_last. The same window is therefore scorable per SLOT.
    #
    # THE PREDICATE, from the engine's own semantics rather than a heuristic:
    # a record whose y descended more than CSS_ANIM_FALL_PX inside the window,
    # restricted to the HAZARD class (entity_kind 0/1/5 with a valid player
    # slot = the objects that run a fighter script through a character slot's
    # command blob). Kinds 2/3/4 are the character-select UI, and the confirm
    # sprite legitimately descends on BOTH planes.
    #
    # The verdict is a HOST-RELATIVE count per paired window: the spectator may
    # not carry char-bound fallers the host does not. Measured over the kept
    # corpus (4 games, 12 runs): the host scores 0 in every window of every run,
    # and every GREEN spectator arm scores 0; only the two RED arms score.
    #
    # NOT COMPUTED: dark instrument (nobody armed FM2K_CSS_ANIM) is an honest
    # SKIP, not a pass and not a red -- the gate stages that judge character
    # select arm it (tools/run_all_tests.sh), an ad-hoc run may not. A
    # ONE-SIDED instrument (present on the host, absent on a viewer, or the
    # reverse) IS a red: that is the half-blind-run trap, and it is the only way
    # this term can silently measure nothing while looking armed.
    _anim_rx = _re.compile(
        r"\[CSS-ANIM\] win=(\d+) ev=rec tv=(\d+) n=\d+ dropped=(\d+) "
        r"part=\d+/\d+ f=\S+ d=(.*)$")
    _ANIM_FIELDS = ("slot owner kind pslot script0 script1 born life adv "
                    "firstadv lastadv park item0 item1 y0 y1").split()

    def _anim_wins(path):
        """{window_ordinal: [record dict]} from one plane's [CSS-ANIM] lines."""
        out, bad_ver = {}, 0
        try:
            fh = open(path, errors="ignore")
        except OSError:
            return out, bad_ver
        for ln in fh:
            m = _anim_rx.search(ln)
            if not m:
                continue
            if int(m.group(2)) != _CSS_WIN_VER:
                bad_ver += 1
                continue
            recs = out.setdefault(int(m.group(1)), [])
            for rec in m.group(4).split():
                f = rec.split(":")
                if len(f) != len(_ANIM_FIELDS):
                    continue
                try:
                    recs.append(dict(zip(_ANIM_FIELDS, (int(x) for x in f))))
                except ValueError:
                    continue
        return out, bad_ver

    def _anim_falls(recs):
        return [r for r in recs
                if r["y0"] != 0 and (r["y1"] - r["y0"]) > CSS_ANIM_FALL_PX
                and r["kind"] in (0, 1, 5) and 0 <= r["pslot"] < 8]

    def _closed_a_window(path):
        """Did this plane CLOSE a character-select window in this capture?

        [CSS-ANIM] dumps its records at window CLOSE (css_window.cpp
        CloseWindow), so a capture that ENDS inside a character-select window
        legitimately carries zero records. Measured on the kept corpus: the
        pkmncc css2 viewer of r3/r4 opens its boot window and is terminated
        inside it (ev=open 1, why=close 0), which is why "host has records,
        viewer has none" cannot be fatal on its own.
        """
        try:
            with open(path, errors="ignore") as fh:
                for ln in fh:
                    if "why=close" in ln and "[CSS-OBJ]" in ln:
                        return True
        except OSError:
            pass
        return False

    ha, habad = _anim_wins(out_dir / "live_FM2K_P1_Debug.log")
    sa = {s["tag"]: _anim_wins(s["live"])[0] for s in specs}
    if not ha and not any(sa.values()):
        lines.append("[harness] CSS-WIN CSSANIM: no [CSS-ANIM] lines on any "
                     "plane -- the per-slot fall census was NOT ARMED in this "
                     "run (FM2K_CSS_ANIM unset). NOT COMPUTED, honestly skipped "
                     "-- this is not a pass. The shipping gate stages that judge "
                     "character select arm it")
    elif not ha:
        _fail("[harness] CSS-WIN CSSANIM: [CSS-ANIM] lines on a spectator but "
              "NONE on the host -- a one-sided census measures the viewer "
              "against nothing. NOT COMPUTED")
    else:
        hord = sorted(ha)
        hfall_tot = sum(len(_anim_falls(ha[w])) for w in hord)
        lines.append(f"[harness] CSS-WIN CSSANIM: host census "
                     f"{sum(len(ha[w]) for w in hord)} record(s) over "
                     f"{len(hord)} window(s), {hfall_tot} char-bound faller(s)"
                     + (f"; {habad} line(s) at an unknown tv=" if habad else ""))
        for s in specs:
            sw = sa[s["tag"]]
            if not sw:
                if _closed_a_window(s["live"]):
                    _fail(f"[harness] CSS-WIN CSSANIM {s['tag']}: no [CSS-ANIM] "
                          f"lines while the HOST has them, on a viewer that DID "
                          f"close a character-select window -- the census did "
                          f"not reach this plane (one-sided instrument). NOT "
                          f"COMPUTED")
                else:
                    lines.append(
                        f"[harness] CSS-WIN CSSANIM {s['tag']}: no [CSS-ANIM] "
                        f"lines and no closed character-select window in this "
                        f"capture -- the census dumps at window CLOSE, so a "
                        f"viewer terminated inside its window has nothing to "
                        f"dump. NOT COMPUTED on this plane [LOUD, not fatal]")
                continue
            sord = sorted(sw)
            npair = min(len(hord), len(sord))
            sbad = 0
            for j in range(npair):
                ho = hord[len(hord) - npair + j]
                so = sord[len(sord) - npair + j]
                hf, sf = _anim_falls(ha[ho]), _anim_falls(sw[so])
                if len(sf) <= len(hf):
                    continue
                sbad += 1
                if CSS_WIN_FATAL:
                    fail = True
                lines.append(
                    f"[harness] CSS-WIN CSSANIM {s['tag']} win{so} vs "
                    f"host-win{ho}: {len(sf)} char-bound falling object(s) on "
                    f"the spectator vs {len(hf)} on the host -> CSS FALLING "
                    f"OBJECT (per-slot census). "
                    + " ".join(
                        f"slot{r['slot']}[own{r['owner']} k{r['kind']} "
                        f"p{r['pslot']} scr{r['script0']}->{r['script1']} "
                        f"born{r['born']} life{r['life']} "
                        f"y{r['y0']}->{r['y1']}]" for r in sf[:6])
                    + adv)
            lines.append(f"[harness] CSS-WIN CSSANIM {s['tag']}: {npair} paired "
                         f"window(s) ({len(sord)} spectator / {len(hord)} host), "
                         f"{sum(len(_anim_falls(sw[w])) for w in sord)} "
                         f"char-bound faller(s) total, {sbad} window(s) worse "
                         f"than the host"
                         + (" -> FAIL" + adv if sbad else " -> PASS"))

    if not CSS_WIN_FATAL:
        lines.append("[harness] CSS-WIN: ADVISORY -- explicitly downgraded by "
                     "FM2K_CSSWIN_FATAL=0, so these terms do not affect the run "
                     "verdict. The default is FATAL as of Wave 2 (the "
                     "character-select falling-object bug is fixed in "
                     "css_autoconfirm.cpp); unset the variable to gate on them")
    return fail, lines


def _css_pin_gate(out_dir, specs):
    """The two ALWAYS-ON hook detectors on the character-select plane, parsed.

    Returns (fail: bool, lines: list[str]).

    WHY THIS FUNCTION EXISTS AT ALL. Both instruments below were shipped
    always-on, and both printed into the void: `grep -rn "CSSPIN\\|CSSPARK-TRIP"
    tools/` returned only env forwarders and archived logs. This repo has now
    fixed that same shape three times (f8e4b67 "previously printed into the
    void"; 09917b2's frame-zero tripwire; the [ROUNDS-RELATCH]/[SPEC-RELATCH]
    blocks in _parity_gates), and the seam-fix hard rules allow a detector to
    survive a deleted mechanism only BECAUSE a detector is not a fallback -- a
    detector nobody parses is neither.

    Deliberately NOT under FM2K_CSSWIN_FATAL. That switch exists to downgrade
    the .pty-derived FALL/CSSPOOL/CSSANIM terms, which have a filed
    false-positive class on pkmncc (orphan_fix.md 5.5) and are turned off on the
    rotation leg because of it. These two terms are hook-side assertions with no
    false-positive class: each one fires only on a state the engine itself never
    produces.

    CSSPIN (css_autoconfirm.cpp, one line per pin ARM)
        `orphans>0 unloaded=0` means the CssAutoConfirm pin found live preview
        objects bound to the char slot it was about to re-select and tore down
        NONE of them -- i.e. the orphaned-preview defect, back. FATAL.
        This is the ONLY automated detector of the class the 2026-08-17 fix
        closed: the .pty-derived FALL term structurally cannot see it (it
        resolves one object per player, and the orphan is usually a sibling), so
        without this the gate would carry a fatal term on the bug's behalf that
        scored 0 of 4 on the corpus where the bug is known present.

        A pin ARM with NO [CSSPIN] line is LOUD but not fatal: measured on the
        kept corpus (r11_wanwan_GREEN2 S1), a viewer can arm at a match-end seam
        and leave character select without the pin's mode-2000 block ever
        running, which produces an arm and no line legitimately.

        A [CSSPIN] line on a PLAYER plane is FATAL: the pin arms from three
        spectator/offline-replay sites plus the test-only FM2K_TEST_CSS_CHAR,
        which no spec_selftest env list carries and which cannot leak in (the
        harness passes env through a generated .bat, not the WSL environment).
        A line there means plane containment broke.

    CSSPARK-TRIP (spec_css_tripwire.cpp, battle frame 0)
        Type-4 objects still PARKED (script_init_state == 2) at the spectator's
        first battle frame: fighters that cannot execute their scripts. The
        character-select park that used to cause it was deleted 2026-08-17, so a
        line here now names a NEW +0x152 writer or the match-end seam park. This
        is the detector the no-fallback rule kept when the mechanism went; FATAL,
        matching the [SPEC-RELATCH] TRIP block's "read this first" prominence.
    """
    import re as _re
    fail, lines = False, []

    def _fail(msg):
        nonlocal fail
        fail = True
        lines.append(msg)

    _pin_rx = _re.compile(r"p(\d) sel=(-?\d+)->(-?\d+) slot=(-?\d+) "
                          r"orphans=(\d+) unloaded=(\d+)")

    def _scan(path):
        """(csspin_lines, orphan_no_unload, cssparktrip_lines, armed)."""
        pin, bad, trip, armed = 0, [], [], 0
        try:
            fh = open(path, errors="ignore")
        except OSError:
            return pin, bad, trip, armed
        for ln in fh:
            if "[CSSPARK-TRIP]" in ln:
                trip.append(ln.rstrip())
            elif "[CSSPIN] arm " in ln:
                pin += 1
                for m in _pin_rx.finditer(ln):
                    if int(m.group(5)) > 0 and int(m.group(6)) == 0:
                        bad.append(ln.rstrip())
                        break
            elif "CssAutoConfirm: armed" in ln:
                armed += 1
        return pin, bad, trip, armed

    planes = [("P1", out_dir / "live_FM2K_P1_Debug.log"),
              ("P2", out_dir / "live_FM2K_P2_Debug.log")]
    planes += [(s["tag"], s["live"]) for s in specs]

    n_pin, n_trip, n_armed = 0, 0, 0
    for tag, path in planes:
        pin, bad, trip, armed = _scan(path)
        n_pin += pin
        n_trip += len(trip)
        n_armed += armed
        is_player = tag in ("P1", "P2")
        if pin and is_player:
            _fail(f"[harness] CSSPIN {tag}: {pin} [CSSPIN] line(s) on a PLAYER "
                  f"plane -- the CssAutoConfirm pin ARMED on a production "
                  f"player process. It arms only from the spectator / "
                  f"offline-replay sites and the test-only FM2K_TEST_CSS_CHAR, "
                  f"which this harness never sets: plane containment is broken "
                  f"and a sim-visible write ran where it must not")
        if bad:
            _fail(f"[harness] CSSPIN {tag}: ORPHANED PREVIEW x{len(bad)} -- the "
                  f"pin re-selected a character while live preview objects were "
                  f"still bound to that char slot and unloaded NONE of them "
                  f"(orphans>0 unloaded=0). This is the falling-character-select-"
                  f"object defect fixed 2026-08-17 in css_autoconfirm.cpp; "
                  f"expected ONLY in an FM2K_SPEC_CSS_UNLOAD=0 red arm")
            lines.append(f"    {bad[0]}")
        if trip:
            _fail(f"[harness] CSSPARK-TRIP {tag}: x{len(trip)} -- type-4 script "
                  f"VMs were still PARKED at battle frame 0, so those fighters "
                  f"cannot execute their scripts. The character-select park was "
                  f"DELETED 2026-08-17, so this names a NEW +0x152 writer or the "
                  f"match-end seam park (round_events.cpp sim-902)")
            lines.append(f"    {trip[0]}")
        if not is_player and armed and pin == 0:
            lines.append(
                f"[harness] CSSPIN {tag}: the pin ARMED {armed} time(s) and "
                f"printed NO [CSSPIN] line -- the arm never reached the "
                f"mode-2000 character-select block (a match-end-seam arm that "
                f"left character select can do this legitimately), so the "
                f"orphan census did not run on this viewer [LOUD, not fatal]")
    if not fail:
        lines.append(f"[harness] CSSPIN/CSSPARK-TRIP: {n_pin} pin census "
                     f"line(s) over {n_armed} arm(s), 0 orphaned previews "
                     f"(orphans>0 unloaded=0), 0 parked-VM tripwires -> PASS")
    return fail, lines


def _parity_gates(out_dir, specs):
    """The three correctness gates, over the PRESERVED live_ logs only.

    CINPUT (primary, frame-keyed input identity), CHECKSUM (the full-state
    fencepost -- host-vs-spectator AND, since 2026-08-16, PLAYER-vs-PLAYER)
    and the rng/hp trace GATE. Returns (cin_fail, ck_fail, pvp_fail) and sets
    `gate`/`ok` on each spec dict; prints its verdict lines as it goes.
    `pvp_fail` is kept SEPARATE from `ck_fail` on purpose: the two flags name
    different planes (players vs spectators) and this file has twice shipped a
    verdict line that named the wrong gate (Wave-2 review B4c).

    Module-level (like _css_parity_gate, which tools/test_css_gate.py re-runs
    offline) for two reasons: main() must be able to call it BEFORE any early
    return -- a run that bails on liveness or a missing stream used to produce
    no correctness verdict in either direction -- and a --keep log set can be
    re-gated offline without launching a game.
    """
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
    # Since 2026-08 main() calls this BEFORE the replay phase too, so the trap
    # is structurally out of reach rather than merely avoided by convention.
    host_dbg = out_dir / "live_FM2K_P1_Debug.log"
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

    def _pair_key(mismatches, overlap, prefix):
        """Ranking key that picks WHICH host match a spectator segment belongs to.

        A spectator segment is compared against every host match and the best
        pairing is the one reported. Until 2026-08 the key led with the longest
        consecutive-MISMATCH run, which is a divergence-shape statistic, not a
        pairing statistic: a wrong host match whose CRCs disagree everywhere
        scores a long run, but so does the RIGHT host match once a real desync
        starts -- and past the first few hundred divergent frames the wrong
        pairing can score BETTER. The stage that hurt: the match-end-seam
        forensics (docs/dev/matchend_seam_campaign.md) printed
        "vs host-match0" for a segment that plainly belonged to a later match,
        and the desync frames only lined up after agents re-paired the segments
        by raw CRC by hand.

        The key is now, in order:
          1. overlap > 0            -- a candidate with no aligned frames at all
                                       explains nothing; it may only win if
                                       every candidate is empty (that case is
                                       reported as NO OVERLAP downstream).
          2. fewest mismatches      -- the actual "does this segment belong to
                                       this match" evidence.
          3. longest identical prefix -- tie-break: the segment that agreed with
                                       this host match for longest BEFORE
                                       diverging is the one it was really
                                       following.
          4. largest overlap        -- final tie-break, unchanged.
        Known residual (unchanged by this fix): a badly truncated candidate can
        still win on raw count against a long, nearly-identical one. Both keys
        had that; it has never been observed, because CRC/press collisions
        across matches are what would be needed to produce it.
        """
        return (0 if overlap > 0 else 1, mismatches, -prefix, -overlap)

    def _pick_tied(cands, prev_hi):
        """Resolve an EXACT tie on _pair_key by ordinal continuity.

        Ties are not a corner case here, they are the norm on the CINPUT side:
        the autoplay drives the SAME scripted input sequence every match, so a
        spectator's match-N input stream aligns against EVERY host match with
        zero mismatches, all candidate keys are identical, and `min()` handed
        back host-match0 forever. A 20-match stressor run printed "vs
        host-match0" for all 20 segments, which is what sent the match-end-seam
        forensics looking for a divergence at frame ~2530 of match 1.

        Matches are strictly time-ordered on both sides and a spectator sees
        them in order, so when the evidence cannot separate candidates the next
        segment continues the sequence: prefer the tied candidate nearest to
        (previous choice + 1). This is DELIBERATELY only reachable on an exact
        tie -- every tied candidate produced identical mismatch/overlap/prefix
        numbers, so the choice cannot change any verdict, only the label.

        cands: [(key, hi, ...)] ; returns the winning tuple.
        """
        bkey = min(c[0] for c in cands)
        tied = [c for c in cands if c[0] == bkey]
        want = prev_hi + 1
        return min(tied, key=lambda c: (abs(c[1] - want), c[1])), len(tied)

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
        # (full_mismatches, offset, overlap, longest_run, identical_prefix).
        # Offset = MODE of all press-deltas -- robust to stray boundary inputs
        # and the autoplay's identical match openings (a single-press anchor
        # mis-aligns). longest_run = longest consecutive-mismatch streak;
        # isolated 1-frame artifacts (off-by-one boundary, a stray transition
        # press) don't count as a desync, a SUSTAINED run does. identical_prefix
        # = aligned frames that matched before the FIRST mismatch; it is the
        # pairing tie-break (see _pair_key), never a verdict.
        spress = [(bf, sseg[bf]) for bf in sorted(sseg) if sseg[bf] != (0, 0)]
        if not spress: return (0, 0, 0, 0, 0)
        hbi = {}
        for hb in hseg:
            if hseg[hb] != (0, 0): hbi.setdefault(hseg[hb], []).append(hb)
        deltas = {}
        for sb, si in spress:
            for hb in hbi.get(si, ()):
                deltas[hb - sb] = deltas.get(hb - sb, 0) + 1
        if not deltas: return (len(spress), 0, 0, len(spress), 0)
        O = max(deltas, key=deltas.get)
        bfs = sorted(bf for bf in sseg if (bf + O) in hseg)
        run = mx = fmm = pfx = 0
        for bf in bfs:
            if hseg[bf + O] != sseg[bf]:
                fmm += 1; run += 1; mx = max(mx, run)
            else:
                run = 0
                if fmm == 0: pfx += 1
        return (fmm, O, len(bfs), mx, pfx)
    cin_H = _cin_parse(out_dir / "live_FM2K_P1_Debug.log")
    cin_fail = False
    if not cin_H:
        print("[harness] CINPUT: no host [CINPUT] -- detector inactive (need FM2K_CINPUT=1 + a battle)")
    else:
        cin_G = _cin_parse(out_dir / "live_FM2K_P2_Debug.log")
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
            prev_hi = -1
            for si, sseg in enumerate(_cin_parse(s["live"])):
                cands = []
                for hi, hseg in enumerate(cin_H):
                    fmm, O, n, mx, pfx = _cin_align(sseg, hseg)
                    cands.append((_pair_key(fmm, n, pfx), hi, fmm, O, n, mx))
                (_, hi, fmm, O, n, mx), nties = _pick_tied(cands, prev_hi)
                prev_hi = hi
                tie = f" [tie:{nties}]" if nties > 1 else ""
                if mx > 3:   # sustained mismatch run = real input-frame desync
                    cin_fail = True
                    hseg = cin_H[hi]
                    fb = next((bf for bf in sorted(sseg) if (bf + O) in hseg and hseg[bf + O] != sseg[bf]), None)
                    print(f"[harness] CINPUT {s['tag']} seg{si}: vs host-match{hi} off{O}: {fmm} mismatches "
                          f"(longest run {mx}) -> DESYNC (first spec-bf={fb} host-bf={fb + O if fb is not None else '?'}: "
                          f"spec={sseg.get(fb)} host={hseg.get(fb + O) if fb is not None else '?'}){tie}")
                else:
                    extra = f" ({fmm} isolated boundary artifact{'s' if fmm != 1 else ''})" if fmm else ""
                    print(f"[harness] CINPUT {s['tag']} seg{si}: vs host-match{hi} off{O}: "
                          f"{n} frames input-frame IDENTICAL{extra}{tie}")

    # ---- FULL-STATE FENCEPOST: [CHECKSUM] gameplay_fingerprint (GDC GAP #1) ----
    # The host logs the gameplay_fingerprint (HP/pos/rng/timer -- gekko's own
    # P1-vs-P2 desync hash) at every SAVE event; the spectator RECOMPUTES the same
    # fingerprint from its live memory each applied frame (never rolls back ->
    # always confirmed). Aligning the spec's CRC sequence to the host's catches
    # POSITION/full-state desyncs the subset rng/hp gate is structurally blind to.
    # Host f resets per match + re-emits per re-sim -> segment on f=-1, dedupe
    # frame-LAST. Spec bf resets per battle -> segment on bf reset.
    # Phase 4c: the line now carries the POOL-TOPOLOGY terms alongside crc --
    #   top=  slot->type map + population  (which objects exist, of what kind,
    #         at which slot indices; the term a re-INDEXING moves)
    #   bind= owner / player slot / entity kind / creator link, in slot order
    #   nobj= active-slot population
    # All four are optional in the regex so logs from builds that predate them
    # still parse (they simply contribute no topology verdict).
    #
    # tv= TOPOLOGY TERM VERSION (Phase 4e, review A4a(i)). PRE-4c builds emit a
    # top= too, but it is the RETIRED COMBINED digest (slot map AND bindings
    # folded together, tag PTO1) -- a different quantity under the same name.
    # Re-gating an old corpus with the new slot-map semantics therefore reds
    # every segment (measured on p4b_runs/R5: top 3953/3953). The version tag
    # makes PTO1 and PTO2 distinguishable on the wire: `tv=2` means "top=/bind=
    # are the 4c split", its ABSENCE means PTO1 and the pool terms are skipped
    # with a loud line instead of compared.
    _ckpat = _cre.compile(
        r'\[CHECKSUM\] f=(-?\d+) crc=0x([0-9A-Fa-f]+)'
        r'(?: tv=(\d+))?'
        r'(?: top=0x([0-9A-Fa-f]+))?(?: bind=0x([0-9A-Fa-f]+))?(?: nobj=(\d+))?')
    def _ck_row(m):
        # (crc, top, bind, nobj, topology_version); None for a term this build
        # did not emit. VERSION: the explicit tv= when present, otherwise
        # INFERRED from bind= -- the 4c split introduced bind= and PTO1 builds
        # never emitted it, so "has bind=" is an exact test for the new
        # semantics on the corpus recorded between 4c and 4e. That keeps the
        # p4c_runs corpus re-gatable instead of blanket-skipping everything
        # older than the tag.
        bind = int(m.group(5), 16) if m.group(5) else None
        if m.group(3):
            ver = int(m.group(3))
        else:
            ver = 2 if bind is not None else 1
        return (int(m.group(2), 16),
                int(m.group(4), 16) if m.group(4) else None,
                bind,
                int(m.group(6))     if m.group(6) else None,
                ver)
    def _ck_host(path):
        # per-MATCH segments [{frame: row}]; split on f=-1 (battle-entry marker),
        # dedupe frame-LAST within a segment (re-sim re-emits; last = confirmed).
        segs, cur = [], {}
        try: fh = open(path, errors="ignore")
        except OSError: return segs
        for ln in fh:
            m = _ckpat.search(ln)
            if not m: continue
            f = int(m.group(1))
            if f < 0:
                if cur: segs.append(cur); cur = {}
                continue
            cur[f] = _ck_row(m)
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
            if bf <= last and cur: segs.append(cur); cur = {}
            cur[bf] = _ck_row(m); last = bf
        if cur: segs.append(cur)
        return segs
    def _ck_crc(seg):
        return {f: r[0] for f, r in seg.items()}
    _CK_TOPO_VER = 2       # the 4c top=/bind= split; see the tv= note above
    def _ck_term(sseg, hseg, O, idx):
        '''Compare one non-crc [CHECKSUM] term at the ALREADY-CHOSEN pairing
        offset O. Returns (mismatches, compared, longest_run, first_bf,
        legacy_rows, notcomputed_rows).

        Frames where either side did not emit the term are skipped, so an old
        log pairs cleanly against a new one instead of reporting a false red.
        Two further classes are NOT compared, and are counted so the caller can
        say so out loud instead of scoring them as agreement:

        LEGACY (review A4a(i)) -- either side lacks `tv=2`, i.e. its top=/bind=
        are the retired PTO1 combined digest. A different quantity under the
        same name; comparing it is a guaranteed false RED.

        NOT-COMPUTED (review A4a(ii), the ship-blocker) -- either side's
        topology digest is the documented `0` sentinel (parity_pool.h:104-106),
        emitted whenever TopologyEnabled() is false: ENGINE_FM95, or
        FM2K_CK_TOPOLOGY=0. The line is still printed as
        `top=0x00000000 bind=0x00000000 nobj=0`, so a naive comparison finds
        both planes equal and prints a POSITIVE verdict for a FATAL term that
        never ran. A real digest can never be 0 (the seed is non-zero and
        ScanPool reserves 0 with `folded ? folded : 1u`), so the test is exact.'''
        run = mx = mm = 0; first = None; n = 0; n_legacy = 0; n_nc = 0
        for bf in sorted(sseg):
            srow = sseg[bf]
            hrow = hseg.get(bf + O)
            if hrow is None: continue
            sv, hv = srow[idx], hrow[idx]
            if sv is None or hv is None: continue
            # Version gate FIRST: an unversioned row's top=/bind= mean something
            # else, and its nobj= rides the same topology pass.
            if srow[4] != _CK_TOPO_VER or hrow[4] != _CK_TOPO_VER:
                n_legacy += 1; continue
            # Sentinel gate: 0 == not computed, on EITHER plane.
            if srow[1] == 0 or hrow[1] == 0:
                n_nc += 1; continue
            n += 1
            if hv != sv:
                mm += 1; run += 1; mx = max(mx, run)
                if first is None: first = bf
            else:
                run = 0
        return (mm, n, mx, first, n_legacy, n_nc)
    def _ck_align(sseg, hseg):
        # offset O (host_f = spec_bf + O) maximizing CRC matches via distinctive
        # non-zero CRC anchors. Returns (mismatches, O, overlap, longest_run,
        # first, identical_prefix). identical_prefix feeds _pair_key only.
        sanchor = [(bf, sseg[bf]) for bf in sorted(sseg) if sseg[bf] != 0]
        if not sanchor: return (0, 0, 0, 0, None, 0)
        hbi = {}
        for hf, hc in hseg.items():
            if hc != 0: hbi.setdefault(hc, []).append(hf)
        deltas = {}
        for sb, sc in sanchor:
            for hf in hbi.get(sc, ()):
                deltas[hf - sb] = deltas.get(hf - sb, 0) + 1
        if not deltas: return (len(sanchor), 0, 0, len(sanchor), sanchor[0][0], 0)
        O = max(deltas, key=deltas.get)
        bfs = sorted(bf for bf in sseg if sseg[bf] != 0 and (bf + O) in hseg)
        run = mx = mm = pfx = 0; first = None
        for bf in bfs:
            if hseg[bf + O] != sseg[bf]:
                mm += 1; run += 1; mx = max(mx, run)
                if first is None: first = bf
            else:
                run = 0
                if mm == 0: pfx += 1
        return (mm, O, len(bfs), mx, first, pfx)
    ck_H = _ck_host(out_dir / "live_FM2K_P1_Debug.log")
    ck_fail = False
    if not ck_H:
        print("[harness] CHECKSUM: no host [CHECKSUM] -- full-state fencepost "
              "inactive (need FM2K_CINPUT=1 + a battle)")
    else:
        for s in specs:
            prev_hi = -1
            # G1's rule, arriving in a SECOND term (2026-08-18). A segment with
            # ZERO overlapping frames compared NOTHING, and "nothing was
            # compared" is not a desync -- printing one is the same
            # not-a-measurement error the CSS-SPEC `spec=None` branch used to
            # make. Materialised so the LAST segment can be told from a
            # mid-stream one, which is the whole discriminator (see below).
            _ck_segs = list(_ck_spec(s["live"]))
            ck_scored = 0
            for si, sseg in enumerate(_ck_segs):
                scrc = _ck_crc(sseg)
                nz = sum(1 for c in scrc.values() if c)
                if nz == 0:
                    ck_fail = True
                    print(f"[harness] CHECKSUM {s['tag']} seg{si}: ALL-ZERO CRCs "
                          f"(stale spec fingerprint -- recompute regressed) -> FAIL")
                    continue
                cands = []
                for hi, hseg in enumerate(ck_H):
                    mm, O, n, mx, fb, pfx = _ck_align(scrc, _ck_crc(hseg))
                    cands.append((_pair_key(mm, n, pfx), hi, mm, O, n, mx, fb))
                (_, hi, mm, O, n, mx, fb), nties = _pick_tied(cands, prev_hi)
                prev_hi = hi
                tie = f" [tie:{nties}]" if nties > 1 else ""
                if n == 0 and si == len(_ck_segs) - 1:
                    # TRUNCATED TAIL -- NO VERDICT, and it is not a pass either
                    # (the coverage floor below is what protects this).
                    #
                    # Measured 2026-08-18, and it was MY change that surfaced
                    # it: raising FM2K_SPEC_HOST_GONE_MS to the shipped 12000
                    # (H1) lets a legitimately-behind from-frame-0 viewer live
                    # 7s longer, long enough to OPEN a second battle segment out
                    # of its own buffer that it never gets host-comparable
                    # content for. Same-tree A/B on the kill-switch recipe:
                    #   HOST_GONE=5000  -> one segment, 2136 frames IDENTICAL
                    #   HOST_GONE=12000 -> the same 2136 IDENTICAL, PLUS an
                    #                      empty seg1 printed as FULL-STATE
                    #                      DESYNC
                    # Everything real was identical across the arms (same lag,
                    # gap -1077 vs -1076; same bit-exact seg0). The viewer did
                    # not desync; the term scored a segment that held nothing.
                    print(f"[harness] CHECKSUM {s['tag']} seg{si}: TRUNCATED "
                          f"(0 frames overlap any host match -- the viewer opened "
                          f"this segment and the stream ended) -- NO VERDICT")
                elif n == 0:
                    # MID-STREAM zero overlap is still FATAL. A viewer that
                    # entered a segment and left it with nothing comparable,
                    # while continuing to produce later segments, is a real
                    # finding (G1's DEGENERATE class, same reasoning).
                    ck_fail = True
                    print(f"[harness] CHECKSUM {s['tag']} seg{si}: NO OVERLAP with "
                          f"any host match, MID-STREAM (segment {si} of "
                          f"{len(_ck_segs)}) -> FULL-STATE DESYNC")
                elif mx > 3:
                    ck_fail = True
                    ck_scored += 1      # measured, and it FAILED -- not "unmeasured"
                    hcrc = _ck_crc(ck_H[hi])
                    print(f"[harness] CHECKSUM {s['tag']} seg{si}: vs host-match{hi} "
                          f"off{O} {mm}/{n} CRC mismatches (longest run {mx}) -> "
                          f"FULL-STATE DESYNC (first spec-bf={fb} host-f={fb + O}: "
                          f"spec=0x{scrc[fb]:08X} host=0x{hcrc[fb + O]:08X}){tie}")
                else:
                    ck_scored += 1
                    extra = f" ({mm} tail/predicted artifact)" if mm else ""
                    print(f"[harness] CHECKSUM {s['tag']} seg{si}: vs host-match{hi} "
                          f"off{O} {n} frames FULL-STATE IDENTICAL{extra}{tie}")

                # ---- POOL TOPOLOGY (Phase 4c) --------------------------------
                # The crc term above hashes rng / HP / timers / ring inputs. It
                # is structurally blind to the object pool, and 4b measured the
                # consequence: a spectator's pool population drifted from the
                # host's for 1163 frames BEFORE either fighter diverged, in a
                # match every existing gate called clean until the fighters
                # finally forked. nobj= and top= are that early warning.
                #
                # FAILING terms, same longest-run>3 rule the crc/CINPUT gates
                # use (so the documented single-frame emit/dedupe artifact
                # cannot flip a verdict), raw counts always printed:
                #   nobj -- active-object POPULATION. 0 mismatches across 22094
                #           clean paired frames in 4b, 530 in the pre-divergence
                #           window of the one match that broke.
                #   top  -- slot->type map + population, i.e. the re-INDEXING
                #           itself. This is the term the match-start pool
                #           resync exists to keep at zero.
                # ADVISORY term:
                #   bind -- owner / player / kind / creator link. Strictly more
                #           sensitive; reported so a regression here is visible,
                #           never fatal on its own.
                #
                # PHASE 4e (review A4a). A FATAL term that could not run must be
                # RED, never a vacuous green: `tn == 0` used to `continue`
                # SILENTLY, so a FM95 build or a single FM2K_CK_TOPOLOGY=0 turned
                # the campaign's headline gate into a positive verdict on a term
                # that was never computed. The three no-compare classes are now
                # distinguished and each says what it is:
                #   not-computed  -> FATAL FAIL (or an advisory skip on FM95,
                #                    where the engine genuinely has no pool to
                #                    scan and the whole stage is advisory)
                #   legacy (no tv=2) -> loud SKIP, never a verdict (A4a(i))
                #   term absent      -> loud SKIP (pre-4b log)
                hrow = ck_H[hi]
                for idx, name, fatal in ((3, "nobj", True), (1, "top", True),
                                         (2, "bind", False)):
                    tmm, tn, tmx, tfb, tleg, tnc = _ck_term(sseg, hrow, O, idx)
                    if tn == 0:
                        if tnc:
                            if IS_FM95_RUN:
                                print(f"[harness] POOL {s['tag']} seg{si}: {name}= "
                                      f"NOT COMPUTED on {tnc} paired frames "
                                      f"(top=0x00000000 sentinel) -- ENGINE_FM95 "
                                      f"build has no FM2K object pool to scan. "
                                      f"ADVISORY SKIP (the FM95 stage is "
                                      f"advisory; on FM2K this is a FAIL)")
                            elif fatal:
                                ck_fail = True
                                print(f"[harness] POOL {s['tag']} seg{si}: {name}= "
                                      f"topology NOT COMPUTED on {tnc} paired "
                                      f"frames (top=0x00000000 sentinel) -> "
                                      f"POOL-TOPOLOGY GATE INACTIVE (FAIL). A "
                                      f"fatal term that cannot run must not "
                                      f"pass. Cause: FM2K_CK_TOPOLOGY=0 in the "
                                      f"game's environment, or an ENGINE_FM95 "
                                      f"build. UNSET FM2K_CK_TOPOLOGY and re-run")
                            else:
                                print(f"[harness] POOL {s['tag']} seg{si}: {name}= "
                                      f"NOT COMPUTED on {tnc} paired frames "
                                      f"[advisory term, skipped]")
                        elif tleg:
                            print(f"[harness] POOL {s['tag']} seg{si}: {name}= "
                                  f"SKIPPED on {tleg} paired frames -- one or "
                                  f"both logs predate the Phase-4c split (no "
                                  f"tv=2). Their top= is the retired PTO1 "
                                  f"COMBINED digest, a different quantity under "
                                  f"the same name; comparing it would be a "
                                  f"false RED, not a verdict")
                        else:
                            print(f"[harness] POOL {s['tag']} seg{si}: {name}= "
                                  f"absent from these logs -- no topology "
                                  f"verdict (build predates the term)")
                        continue
                    if tnc or tleg:
                        print(f"[harness] POOL {s['tag']} seg{si}: {name}= "
                              f"{tnc} not-computed + {tleg} legacy frame(s) "
                              f"excluded from the {tn} compared")
                    if fatal and tmx > 3:
                        ck_fail = True
                        sv = sseg[tfb][idx]; hv = hrow[tfb + O][idx]
                        fmt = (lambda v: str(v)) if name == "nobj" else (lambda v: f"0x{v:08X}")
                        print(f"[harness] POOL {s['tag']} seg{si}: vs host-match{hi} "
                              f"off{O} {name}= {tmm}/{tn} mismatches (longest run "
                              f"{tmx}) -> POOL-TOPOLOGY DESYNC (first spec-bf={tfb} "
                              f"host-f={tfb + O}: spec={fmt(sv)} host={fmt(hv)})")
                    elif tmm:
                        lbl = "advisory" if not fatal else "isolated artifact"
                        print(f"[harness] POOL {s['tag']} seg{si}: vs host-match{hi} "
                              f"off{O} {name}= {tmm}/{tn} mismatches "
                              f"(longest run {tmx}, first spec-bf={tfb}) [{lbl}]")
                    else:
                        print(f"[harness] POOL {s['tag']} seg{si}: vs host-match{hi} "
                              f"off{O} {name}= {tn} frames IDENTICAL")
            # COVERAGE FLOOR, fail-closed -- the other half of the TRUNCATED
            # hatch above. Excusing an unmeasurable tail segment is only honest
            # while SOMETHING was measured; a viewer whose every segment was
            # unmeasurable has produced no full-state verdict at all, and that
            # must never read as a pass.
            if _ck_segs and ck_scored == 0:
                ck_fail = True
                print(f"[harness] CHECKSUM {s['tag']}: 0 of {len(_ck_segs)} "
                      f"viewer segment(s) scorable -- NO VERDICT COMPUTED. A "
                      f"term that measured nothing must never report a pass.")
            elif _ck_segs:
                print(f"[harness] CHECKSUM {s['tag']}: scored {ck_scored}/"
                      f"{len(_ck_segs)} viewer segment(s) against {len(ck_H)} "
                      f"host match(es)")

    # ---- PLAYER-vs-PLAYER FULL-STATE FENCEPOST (the gate hole) ---------------
    # Until 2026-08-16 the harness compared P1 against P2 on CINPUT ONLY --
    # inputs, and nothing else -- while it ran the full [CHECKSUM] term set
    # against every SPECTATOR. That hole hid a real cross-peer simulation
    # divergence for the whole life of the campaign: on vanpri the GUEST enters
    # battle carrying exactly TWO EXTRA live objects, from battle frame 1
    # onward, in 6/6 desyncing sessions across both builds, and 0/11 clean
    # sessions ever show it (ab_rate_verdict.md 5.1b). Every one of those runs
    # printed "CINPUT P1-vs-P2 match0: N frames IDENTICAL (players lockstep)"
    # while the two pools differed by two objects on all N of them.
    #
    # GekkoNet cannot see it either: its desync hash is the
    # gameplay_fingerprint (rng / both HP / both timers / both current inputs --
    # savestate_fm2k_diag.cpp), which has NO object-pool term at all (the
    # documented "GDC GAP #1"). The divergence only becomes a DESYNC # at match
    # end, thousands of frames later, when the extra objects finally push the
    # peers onto different branches of the match-end state machine.
    #
    # Both players are host-shaped logs (segment on the f=-1 battle-entry
    # marker, dedupe frame-LAST so re-sim emits collapse to the confirmed
    # state) and they run the SAME gekko frame numbering, so the pairing offset
    # is 0 by construction -- no _ck_align/_pair_key search, and no chance of
    # the wrong-match mispairing the spectator side needs that machinery for.
    #
    # Fatality, and why each term is where it is:
    #   nobj FATAL -- active-object population. THE term this hole hid; the
    #                 complete predictor of the desync in 17/17 sessions, and
    #                 the only one measured clean on every clean corpus.
    #   top  ADVISORY on THIS plane (it stays FATAL host-vs-spectator, where 4c
    #                 proved 0/34000+). It was fatal here for exactly one gate
    #                 run: base-gate netplay run 2/4 (wanwan, --frames 1200)
    #                 came back top= 1199/1199 red from f=0 with nobj=, crc= and
    #                 CINPUT all IDENTICAL, while runs 1, 3 and 4 of the SAME
    #                 stage were clean on all four terms. top= mixes only
    #                 (slot index, type) + active_count and nothing process-
    #                 dependent (ScanPool, parity_recorder.cpp), so that is a
    #                 REAL intermittent player-plane slot-map divergence -- but
    #                 it is sim-silent, it is NOT the phantom class (which moves
    #                 nobj=), and shipping it fatal reds the base gate ~1 run in
    #                 4 for a defect nothing else can see. Filed as its own
    #                 ticket instead. Precedent: 4c demoted bind= for the same
    #                 reason in the same words -- "a real difference, but not
    #                 the one the term claimed to report, and useless as a gate
    #                 at 100% red".
    #   bind ADVISORY -- owner / player slot / kind / creator link. Strictly
    #                 more sensitive than top=; red on the same run 2/4 above.
    #   crc  ADVISORY -- this is gekko's OWN P1-vs-P2 hash and gekko already
    #                 fires DESYNC # on it; the campaign rule is to judge by
    #                 DESYNC # only. It also diverges legitimately in the
    #                 match-end tail, where the two peers cross battle -> CSS on
    #                 different frames and compute the frames in between from
    #                 different planes. Counted and printed, never fatal.
    #                 KNOWN LIMITATION, deliberately not fixed: crc rides
    #                 _ck_term, so it inherits that helper's tv= version gate
    #                 and topology-sentinel skip even though the fingerprint
    #                 itself never changed across PTO1/PTO2. On a pre-4c corpus
    #                 the crc line therefore reads SKIPPED. Left alone rather
    #                 than special-cased, because _ck_term is shared with the
    #                 FATAL spectator terms and crc is advisory here anyway.
    # Fatal terms use the same longest-run > 3 rule as every other gate here, so
    # a single-frame emit/dedupe artifact cannot flip a verdict.
    pvp_fail = False
    ck_G = _ck_host(out_dir / "live_FM2K_P2_Debug.log")
    if not ck_H:
        # NOT-COMPUTED MUST FAIL IN BOTH DIRECTIONS (review C4b). This branch
        # used to be a bare `pass`, deferring to the "no host [CHECKSUM]"
        # message printed above -- which only PRINTS and sets nothing. So a host
        # log without [CHECKSUM] (FM2K_CINPUT stripped from P1, a host that
        # never reached battle, an out-dir mixup) silently retired the ENTIRE
        # full-state fencepost -- the spectator terms AND these PVP terms -- and
        # the run could still be declared PASS by the remaining gates. That is
        # the ShadowArts shape this campaign was burned by in d0455bc: a stage
        # reporting PASS while its most important check never ran, and the rule
        # (Phase 4e A4a(ii): a fatal term that cannot run must be RED, never a
        # vacuous green) was being applied in one direction only. Same rule,
        # same wording and the same FM95 advisory hatch as the guest branch.
        if IS_FM95_RUN:
            print("[harness] PVP: no host [CHECKSUM] -- ADVISORY SKIP "
                  "(ENGINE_FM95 stage is advisory)")
        else:
            pvp_fail = True
            print("[harness] PVP: the HOST emitted no [CHECKSUM] at all -- "
                  "PLAYER-vs-PLAYER FULL-STATE GATE INACTIVE (FAIL). A fatal "
                  "term that cannot run must not pass. Cause: FM2K_CINPUT "
                  "missing from the host's environment, the host never reached "
                  "a battle, or the harness read the wrong out-dir")
    elif not ck_G:
        # A fatal term that cannot run must be RED, never a vacuous green
        # (Phase 4e review A4a(ii), same rule as the topology sentinel).
        if IS_FM95_RUN:
            print("[harness] PVP: no guest [CHECKSUM] -- ADVISORY SKIP "
                  "(ENGINE_FM95 stage is advisory)")
        else:
            pvp_fail = True
            print("[harness] PVP: the host emitted [CHECKSUM] but the GUEST did "
                  "not -- PLAYER-vs-PLAYER FULL-STATE GATE INACTIVE (FAIL). "
                  "Cause: FM2K_CINPUT missing from the guest's environment, or "
                  "the guest never reached a battle")
    else:
        if len(ck_G) != len(ck_H):
            print(f"[harness] PVP: host has {len(ck_H)} [CHECKSUM] match "
                  f"segment(s), guest {len(ck_G)} -- comparing the first "
                  f"{min(len(ck_H), len(ck_G))} (a truncated guest is the "
                  f"spectator-stall/desync-terminate class, not a pairing bug)")
        # TAIL GUARD for the FATAL terms only (review C4c). = MAX_ROLLBACK_FRAMES
        # (savestate_internal.h:40), the deepest a peer's last emit can have been
        # re-simulated from.
        _PVP_TAIL_GUARD = 64
        for i, hseg in enumerate(ck_H):
            if i >= len(ck_G): break
            gseg = ck_G[i]
            _paired = [bf for bf in gseg if bf in hseg]
            tail_cut = (max(_paired) - _PVP_TAIL_GUARD) if _paired else None
            for idx, name, fatal in ((3, "nobj", True), (1, "top", False),
                                     (2, "bind", False), (0, "crc", False)):
                tmm, tn, tmx, tfb, tleg, tnc = _ck_term(gseg, hseg, 0, idx)
                if tn == 0:
                    if tnc:
                        if IS_FM95_RUN:
                            print(f"[harness] PVP match{i}: {name}= NOT COMPUTED "
                                  f"on {tnc} paired frames (top=0x00000000 "
                                  f"sentinel) -- ENGINE_FM95 build has no FM2K "
                                  f"object pool to scan. ADVISORY SKIP")
                        elif fatal:
                            pvp_fail = True
                            print(f"[harness] PVP match{i}: {name}= topology NOT "
                                  f"COMPUTED on {tnc} paired frames "
                                  f"(top=0x00000000 sentinel) -> PLAYER-vs-"
                                  f"PLAYER POOL GATE INACTIVE (FAIL). Cause: "
                                  f"FM2K_CK_TOPOLOGY=0 in a player's "
                                  f"environment, or an ENGINE_FM95 build. "
                                  f"UNSET FM2K_CK_TOPOLOGY and re-run")
                        else:
                            print(f"[harness] PVP match{i}: {name}= NOT COMPUTED "
                                  f"on {tnc} paired frames [advisory, skipped]")
                    elif tleg:
                        print(f"[harness] PVP match{i}: {name}= SKIPPED on {tleg} "
                              f"paired frames -- one or both player logs predate "
                              f"the Phase-4c split (no tv=2). Their top= is the "
                              f"retired PTO1 COMBINED digest, a different "
                              f"quantity under the same name; comparing it would "
                              f"be a false RED, not a verdict")
                    else:
                        print(f"[harness] PVP match{i}: {name}= absent from these "
                              f"logs -- no verdict (build predates the term)")
                    continue
                if tnc or tleg:
                    print(f"[harness] PVP match{i}: {name}= {tnc} not-computed + "
                          f"{tleg} legacy frame(s) excluded from the {tn} compared")
                if fatal and tmx > 3:
                    # TAIL GUARD (review C4c). Both player segments dedupe
                    # frame-LAST, which is the CONFIRMED state for confirmed
                    # frames -- but the last frames before a hard terminate can
                    # be a PREDICTED re-sim that was never re-confirmed, taken
                    # independently on each peer. One mispredicted input near
                    # the tail can move object creation for more than the 3
                    # consecutive frames this rule needs, and this term is now
                    # FATAL in ~5 gate stages. So the FATAL verdict is judged on
                    # the segment MINUS its last MAX_ROLLBACK_FRAMES: a tolerated
                    # truncation condition may only SHRINK the compared set, it
                    # must never flip the verdict. Raw counts over the FULL
                    # segment are still printed either way, and a red confined to
                    # the tail prints loudly as an advisory instead of vanishing
                    # (phantom_hunt 7.1 named that exact observable). The guard
                    # is conservative, not blinding: the phantom class is red
                    # from f=1 for the whole match, thousands of frames outside
                    # the guarded window, and re-gating the corpora after this
                    # change keeps 3/3 broken runs RED.
                    gcore = ({bf: r for bf, r in gseg.items() if bf <= tail_cut}
                             if tail_cut is not None else {})
                    if gcore:
                        cmm, cn, cmx, cfb, _cl, _cn = _ck_term(gcore, hseg, 0, idx)
                    else:
                        # Segment shorter than the guard: judging it on nothing
                        # would be exactly the vacuous green this block refuses
                        # everywhere else, so fall back to the full segment and
                        # say so.
                        cmm, cn, cmx, cfb = tmm, tn, tmx, tfb
                        print(f"[harness] PVP match{i}: {name}= segment is shorter "
                              f"than the {_PVP_TAIL_GUARD}-frame tail guard -- "
                              f"judging it on all {tn} paired frames")
                    fmt = (lambda v: str(v)) if name == "nobj" else (lambda v: f"0x{v:08X}")
                    if cmx > 3:
                        pvp_fail = True
                        gv = gseg[cfb][idx]; hv = hseg[cfb][idx]
                        print(f"[harness] PVP match{i}: {name}= {cmm}/{cn} mismatches "
                              f"(longest run {cmx}) -> PLAYERS DESYNCED IN THE OBJECT "
                              f"POOL (first f={cfb}: P1={fmt(hv)} P2={fmt(gv)}). This "
                              f"is a REAL cross-peer sim divergence that the gekko "
                              f"fingerprint cannot see (no pool term). [full segment "
                              f"incl. the last {_PVP_TAIL_GUARD} frames: {tmm}/{tn}, "
                              f"longest run {tmx}]")
                    else:
                        gv = gseg[tfb][idx]; hv = hseg[tfb][idx]
                        print(f"[harness] PVP match{i}: {name}= {tmm}/{tn} mismatches "
                              f"(longest run {tmx}, first f={tfb}: P1={fmt(hv)} "
                              f"P2={fmt(gv)}) CONFINED TO THE LAST "
                              f"{_PVP_TAIL_GUARD} FRAMES of the segment (longest run "
                              f"{cmx} over the {cn} frames outside it) [advisory -- "
                              f"tail guard: frame-LAST dedupe can leave predicted, "
                              f"never-re-confirmed frames at a hard terminate. If "
                              f"this line shows up on a run that ALSO has a DESYNC "
                              f"#, treat it as real and re-check the guard]")
                elif tmm:
                    lbl = ("advisory" if not fatal else "isolated artifact")
                    note = ""
                    if name == "crc":
                        note = " -- gekko owns this term; judge by DESYNC #"
                    elif name in ("top", "bind") and tmx > 3:
                        # Loud on purpose: this is a REAL player-plane slot-map
                        # divergence, just not a fatal one yet (see the term
                        # table above). Silent-advisory is how a finding gets
                        # scrolled past.
                        note = (" -- REAL player-plane pool divergence, sim-"
                                "silent; advisory pending its own ticket")
                    # The VALUES, not just the counts. The fatal branch above has
                    # always printed `P1=.. P2=..` and the demoted advisory did
                    # not, which cost real evidence: the FIRST recorded
                    # occurrence of this class printed its digests (and a 2026-08
                    # soak run reproduced them byte-for-byte, proving one stable
                    # shape rather than a random race), while the SECOND, logged
                    # after the demotion, is undiffable. Two advisories are worth
                    # comparing on sight or they are worth nothing.
                    _afmt = ((lambda v: str(v)) if name == "nobj"
                             else (lambda v: f"0x{v:08X}"))
                    try:
                        _av = (f": P1={_afmt(hseg[tfb][idx])} "
                               f"P2={_afmt(gseg[tfb][idx])}")
                    except (KeyError, IndexError, TypeError):
                        _av = ""
                    print(f"[harness] PVP match{i}: {name}= {tmm}/{tn} mismatches "
                          f"(longest run {tmx}, first f={tfb}{_av}) [{lbl}{note}]")
                else:
                    print(f"[harness] PVP match{i}: {name}= {tn} frames IDENTICAL")

    # ---- CSS RENDEZVOUS PARK census, surfaced (loud ADVISORY) ---------------
    # netplay_css.cpp emits one [CSS-RDV] rendezvous: line per player per CSS
    # phase carrying park_ticks= (sim ticks held waiting for the peer) and
    # freerun_sim_ticks= (pre-rendezvous ticks that DID sim). The whole claim of
    # the pre-rendezvous park is that freerun_sim_ticks is 0 on BOTH peers, so
    # both enter character-select lockstep on the same scene frame and the
    # object pool is identical by construction -- that is the acceptance term,
    # and printing it here is what stops it being re-derived by hand from
    # [POOLSET] every time. ADVISORY: it touches no verdict. It is a DETECTOR,
    # not scaffolding -- it stays when the FM2K_CSS_PARK switch is deleted.
    #
    # PARKED-WHILE-WAITING (review amendment 1, css_rendezvous_review.md G1a).
    # The rendezvous line only exists when the rendezvous COMPLETES, so a peer
    # that parks and never un-parks -- the one new failure mode the park creates
    # -- emitted nothing and was mislabelled here as "no character-select phase
    # ran", i.e. a freeze reported as an absence. netplay_css_park.cpp now warns
    # every 2 s while parked ([CSS-RDV] parked ... waiting for peer
    # BATTLE_READY); those lines are counted below and, when a peer has them but
    # no rendezvous, the label says NEVER UN-PARKED instead.
    import re as _rdv_re
    _rdv = {}
    _rdv_parked = {}
    _rdv_realign = {}
    for _tag, _lp in (("P1", out_dir / "live_FM2K_P1_Debug.log"),
                      ("P2", out_dir / "live_FM2K_P2_Debug.log")):
        rows = []
        parked = []
        realign = []
        try:
            with open(_lp, errors="ignore") as fh:
                for ln in fh:
                    if "[CSS-RDV] realign:" in ln:
                        realign.append(ln.strip()); continue
                    if "[CSS-RDV] parked " in ln:
                        mp = _rdv_re.search(
                            r"parked (\d+) ms / (\d+) ticks .*?\(leg=(\d)\)", ln)
                        if mp:
                            parked.append((int(mp.group(1)), int(mp.group(2)),
                                           int(mp.group(3))))
                        continue
                    if "[CSS-RDV] rendezvous:" not in ln: continue
                    m = _rdv_re.search(
                        r"park_ticks=(\d+)\s+freerun_sim_ticks=(\d+)"
                        r"\s+park=(\d)", ln)
                    if m:
                        rows.append((int(m.group(1)), int(m.group(2)),
                                     int(m.group(3))))
        except OSError:
            continue
        _rdv[_tag] = rows
        _rdv_parked[_tag] = parked
        _rdv_realign[_tag] = realign
    if _rdv.get("P1") or _rdv.get("P2") or any(_rdv_parked.values()):
        for _tag in ("P1", "P2"):
            rows = _rdv.get(_tag) or []
            parked = _rdv_parked.get(_tag) or []
            if not rows:
                if parked:
                    print(f"[harness] CSS-RDV {_tag}: NEVER UN-PARKED -- "
                          f"{len(parked)} parked-while-waiting warn(s), longest "
                          f"{max(p[0] for p in parked)} ms / "
                          f"{max(p[1] for p in parked)} ticks (leg="
                          f"{','.join(sorted({str(p[2]) for p in parked}))}), "
                          f"and NO rendezvous line: this peer held the "
                          f"character-select sim waiting for its peer's "
                          f"BATTLE_READY and never got it [advisory]")
                else:
                    print(f"[harness] CSS-RDV {_tag}: NO rendezvous line and no "
                          f"parked-while-waiting warn -- either no netplay "
                          f"character-select phase ran, or the binary predates "
                          f"the pre-rendezvous park [advisory]")
                continue
            if parked:
                print(f"[harness] CSS-RDV {_tag}: {len(parked)} long-park "
                      f"warn(s) before the rendezvous, longest "
                      f"{max(p[0] for p in parked)} ms -- the peer arrived, but "
                      f"the character-select screen was held that long "
                      f"[advisory]")
            print(f"[harness] CSS-RDV {_tag}: {len(rows)} phase(s) park="
                  f"{rows[0][2]} park_ticks="
                  f"{','.join(str(r[0]) for r in rows)} freerun_sim_ticks="
                  f"{','.join(str(r[1]) for r in rows)}")
        fr1 = [r[1] for r in (_rdv.get("P1") or [])]
        fr2 = [r[1] for r in (_rdv.get("P2") or [])]
        if any(fr1) or any(fr2):
            print("[harness] CSS-RDV: NONZERO pre-rendezvous sim ticks "
                  f"(P1={fr1} P2={fr2}) -- the character-select scene was "
                  "free-run before lockstep, which is the pool-transposition "
                  "shape. Expected ONLY in a FM2K_CSS_PARK=0 red arm "
                  "[advisory]")
        elif fr1 and fr2:
            print(f"[harness] CSS-RDV: pre-rendezvous sim ticks 0 on BOTH "
                  f"peers across {len(fr1)}/{len(fr2)} phases -- "
                  f"character-select entered lockstep on the same scene frame")
        # REALIGN TRIPWIRE. netplay_css.cpp used to ZERO P1/P2 action state and
        # the round-timer counter at the rendezvous; that write was deleted on
        # 0/228 measured nonzero readings and replaced by a read-only tripwire.
        # A line here means the deletion's premise broke on this run -- nothing
        # repairs it any more, so it has to be read. Surfaced loudly; advisory,
        # because a nonzero value is evidence to diagnose, not proof of a
        # divergence on its own.
        _rl = [r for _t in ("P1", "P2") for r in (_rdv_realign.get(_t) or [])]
        if _rl:
            print(f"[harness] CSS-RDV REALIGN TRIPWIRE: {len(_rl)} line(s) -- "
                  f"action-state / round-timer were NOT zero at a "
                  f"character-select rendezvous, and the zeroing write is gone. "
                  f"First: {_rl[0][-160:]} [advisory, DIAGNOSE]")

    # ---- BATTLE-ENTRY LATCH RE-DERIVE, surfaced (loud ADVISORY) --------------
    # netplay_barriers.cpp re-derives g_round_limit at the entry barrier and
    # DETECTS -- deliberately never writes -- a stranded g_active_stage_id. Both
    # of those used to print into the void: `grep -rn ROUNDS-RELATCH tools/`
    # returned only log files, so the fix had no oracle of its own and a stage
    # strand could only ever fail a run as an eventual nobj=/DESYNC with no
    # attribution. Surfaced here, and ADVISORY ONLY -- nothing below touches
    # pvp_fail:
    #   contract:  the instrument is present in this binary and says whether the
    #              FM2K_ROUNDS_RELATCH kill switch is on. Printed so "no
    #              [ROUNDS-RELATCH] events" is distinguishable from "no
    #              [ROUNDS-RELATCH] instrument".
    #   CORRECTED: the fix FIRED -- this run raced and was repaired. A PASS with
    #              this line is not the same result as a PASS without it.
    #   REFUSED / SKIPPED: the re-derive declined (no true settings agreement,
    #              non-VS mode flag, implausible wire-supplied round count).
    #              Fail-closed = pre-fix behaviour, so not a failure by itself,
    #              but it is the FIRST line to read when a fatal term above reds.
    #   STAGE-LATCH MISMATCH: the same race stranded g_active_stage_id. NOT
    #              repaired by the fix (the stale stage FILE is already loaded by
    #              then), so expect a stage-driven divergence this match. Loudest
    #              of the four, and still advisory: the fatal terms own verdicts.
    _relatch_order = ("contract:", "CORRECTED", "REFUSED", "SKIPPED",
                      "STAGE-LATCH MISMATCH")
    for _tag, _lp in (("P1", out_dir / "live_FM2K_P1_Debug.log"),
                      ("P2", out_dir / "live_FM2K_P2_Debug.log")):
        hits = {}
        try:
            with open(_lp, errors="ignore") as fh:
                for ln in fh:
                    if "[ROUNDS-RELATCH]" not in ln: continue
                    for p in _relatch_order:
                        if p in ln:
                            hits.setdefault(p, []).append(ln.rstrip()); break
        except OSError:
            continue
        for p in _relatch_order:
            lines = hits.get(p)
            if not lines: continue
            if p == "STAGE-LATCH MISMATCH":
                print(f"[harness] RELATCH {_tag}: STAGE-LATCH MISMATCH x{len(lines)} "
                      f"-- g_active_stage_id was stranded by the battle-entry "
                      f"race and is NOT repaired (assets for the stale stage are "
                      f"already loaded); expect a stage-driven divergence "
                      f"[ADVISORY -- the fatal terms above own the verdict]")
                print(f"    {lines[0]}")
            elif p == "contract:":
                print(f"[harness] RELATCH {_tag}: re-derive instrument present -- "
                      f"{lines[0].split('contract:')[-1].split('(')[0].strip()}")
            else:
                print(f"[harness] RELATCH {_tag}: {len(lines)} {p} line(s) [advisory]")
                print(f"    {lines[0]}")

    # ---- SPECTATOR-PLANE latch re-derive, surfaced (loud ADVISORY) -----------
    # Sibling of the block above, for the viewer plane's own re-derive
    # (spec_relatch.cpp, at the MATCH_START apply). Same reason it exists: the
    # player block was added because a fix with no oracle only ever shows up as
    # an unattributed nobj=/DESYNC later, and the viewer half had exactly that
    # problem one plane over. Advisory only -- nothing here touches pvp_fail.
    #   CORRECTED: the viewer HAD latched a stale round limit and it was
    #              repaired. A PASS carrying this line is not the same result as
    #              a PASS without it.
    #   TRIP:      the frame-zero assertion failed -- something (prime suspect:
    #              a snapshot apply, whose GAME_STATE memcpy covers 0x470048)
    #              put a different value back between the MATCH_START apply and
    #              the viewer's first battle frame. This is the loudest line the
    #              instrument can emit and it means the ordering proof is wrong.
    #   REFUSED / SKIPPED: the re-derive declined. The common one is "h+89 = 0",
    #              i.e. the MATCH_START header does not carry the HOST'S LATCH
    #              -- a legacy .fm2krep or a producer older than that stamp.
    #              The re-derive deliberately does NOT fall back on h+85 (the
    #              host's config SOURCE): a source is not a latch. Also non-VS
    #              mode flag and out-of-range value. Fail-closed = pre-fix
    #              behaviour, so a run full of SKIPPED is un-fixed, not broken.
    #   KILL-SWITCH ARM: the OFF arm's measurement of the same thing -- the
    #              viewer entered the match with the stale latch and nothing
    #              repaired it. Expected (and required) in a red arm; in a run
    #              that did NOT set FM2K_ROUNDS_RELATCH=0 it cannot appear.
    _specrelatch_order = ("TRIP", "KILL-SWITCH ARM", "CORRECTED", "REFUSED",
                          "SKIPPED", "STAGE-LATCH MISMATCH")
    for s in specs:
        hits = {}
        try:
            with open(s["live"], errors="ignore") as fh:
                for ln in fh:
                    if "[SPEC-RELATCH]" not in ln: continue
                    for p in _specrelatch_order:
                        if p in ln:
                            hits.setdefault(p, []).append(ln.rstrip()); break
        except OSError:
            continue
        for p in _specrelatch_order:
            lines = hits.get(p)
            if not lines: continue
            if p == "TRIP":
                print(f"[harness] SPEC-RELATCH {s['tag']}: TRIP x{len(lines)} -- "
                      f"g_round_limit was NOT this match's MATCH_START value at "
                      f"the viewer's first battle frame; something reverted the "
                      f"re-derive [ADVISORY -- read this first]")
            elif p == "KILL-SWITCH ARM":
                print(f"[harness] SPEC-RELATCH {s['tag']}: KILL-SWITCH ARM "
                      f"x{len(lines)} -- the viewer ran with the STALE round "
                      f"limit (re-derive disabled). Expected only in a "
                      f"FM2K_ROUNDS_RELATCH=0 red arm [advisory]")
            else:
                print(f"[harness] SPEC-RELATCH {s['tag']}: {len(lines)} {p} "
                      f"line(s) [advisory]")
            print(f"    {lines[0]}")

    # ---- [CFG] per-battle settings stamp: VS-1v1 MODE PIN (FATAL) ------------
    # The hook stamps one [CFG] line per battle per plane (the
    # vs_round_function detour in round_events.cpp),
    # carrying the five digest fields plus mode_flag = g_game_mode_flag. Two
    # things are surfaced here:
    #   * mode_flag != 1 anywhere = the VS-1v1 pin moved. FATAL as of the
    #     story-only refusal work. Netplay and spectating are pinned to 1v1 by
    #     construction (MATCH_START and SPEC_JOIN_ACK carry exactly two
    #     characters), team mode is a documented NON-GOAL, and 1P/STORY is
    #     STRUCTURALLY unplayable online: process_game_inputs @0x4146D0 samples
    #     ONE pad per battle frame when mode_flag==0, and vs_round_function
    #     picks the fighters out of the story progression table instead of
    #     g_p1/p2_selected_char_idx. A mode!=1 netplay session is NEVER a pass.
    #     It earned the promotion on its first outing: DragonPuppy came back
    #     mode_flag=0 on host, guest AND spectator alongside a full-state
    #     spectator desync from battle frame 0 with inputs identical, and the
    #     advisory wording let the run's verdict be attributed to the desync.
    #     Story-only CONTENT is now refused earlier and louder (rc=3
    #     NOT-APPLICABLE above); this term catches the other way in -- a
    #     VS-capable game that nonetheless ran a non-VS session.
    #   * no [CFG] lines at all = the instrument is absent from this binary (or
    #     FM2K_CFG_TRACE=0), which must stay distinguishable from "the pin is
    #     fine" -- the sentinel lesson from Phase 4d/4e. Now that the term is
    #     fatal, "could not run" is fatal too (review A4a: a FATAL term that
    #     could not run must not print a positive verdict), with the same
    #     ENGINE_FM95 advisory hatch the POOL terms use -- [CFG] is inside
    #     `if constexpr (FM2K::kIsFM2K)`, so an FM95 stage legitimately has none.
    import re as _re_cfg
    _cfg_rx = _re_cfg.compile(r"\[CFG\] plane=(\w+) match=(\d+) .*?mode_flag=(-?\d+)")
    _cfg_planes, _cfg_bad = {}, []
    for _tag, _lp in ([("P1", out_dir / "live_FM2K_P1_Debug.log"),
                       ("P2", out_dir / "live_FM2K_P2_Debug.log")]
                      + [(s["tag"], s["live"]) for s in specs]):
        try:
            with open(_lp, errors="ignore") as fh:
                for ln in fh:
                    m = _cfg_rx.search(ln)
                    if not m:
                        continue
                    _cfg_planes.setdefault(_tag, 0)
                    _cfg_planes[_tag] += 1
                    if int(m.group(3)) != 1:
                        _cfg_bad.append(f"{_tag} match{m.group(2)} "
                                        f"mode_flag={m.group(3)}")
        except OSError:
            continue
    teampin_fail = False
    if not _cfg_planes:
        if IS_FM95_RUN:
            print("[harness] TEAM-PIN: no [CFG] lines -- the stamp is inside "
                  "`if constexpr (FM2K::kIsFM2K)`, so an ENGINE_FM95 build has "
                  "none. ADVISORY SKIP (the FM95 stage is advisory).")
        else:
            teampin_fail = True
            print("[harness] TEAM-PIN FAIL: no [CFG] lines on any plane -- the "
                  "settings stamp is NOT PRESENT in this build (or "
                  "FM2K_CFG_TRACE=0), so the VS-1v1 pin was NOT CHECKED this "
                  "run. A fatal term that could not run is a FAIL, not a pass "
                  "(review A4a). Set FM2K_CFG_TRACE=1 or use a build that "
                  "stamps [CFG].")
    elif _cfg_bad:
        teampin_fail = True
        print(f"[harness] TEAM-PIN FAIL: g_game_mode_flag != 1 (VS 1v1) on "
              f"{len(_cfg_bad)} battle(s): {', '.join(_cfg_bad[:4])} -- netplay "
              f"and spectating are PINNED to 1v1 (MATCH_START / SPEC_JOIN_ACK "
              f"carry two characters). mode_flag=0 is 1P/STORY, where the engine "
              f"samples ONE pad per battle frame and takes the fighters from the "
              f"story table; mode_flag=2 is TEAM, a documented non-goal. Either "
              f"way the session is not the one the protocol describes.")
    else:
        print("[harness] TEAM-PIN: g_game_mode_flag == 1 (VS 1v1) on every "
              "stamped battle -- "
              + ", ".join(f"{k}:{v}" for k, v in sorted(_cfg_planes.items())))

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
    return cin_fail, ck_fail, pvp_fail, teampin_fail


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
    ap.add_argument("--game-exe", default="",
                    help="override the registry path for --game. Exists so a "
                         "caller's 'which install' variable can be LOAD-BEARING "
                         "rather than decorative: the harness runs exactly this "
                         "exe and FAILS (rc=2) if it is absent, instead of "
                         "silently running the hardcoded copy.")
    ap.add_argument("--spectators", default="css",
                    help="comma-list of spectator join-phases. 'css' = dial in "
                         "during the host's CSS (CSS-walk, natural boot); "
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
    ap.add_argument("--assert-spectator-live", action="store_true",
                    help="exit nonzero if any spectator's applied battle-frame "
                         "progression did NOT reach/hold near the host's final "
                         "battle frame (the 'held the live edge' pass condition). "
                         "Always computed + printed; this flag makes it a gate. "
                         "Bounded-deep-join aware: a between-matches joiner is "
                         "judged on the host matches it could observe. The "
                         "verdict is DEFERRED to the OVERALL line so it can no "
                         "longer short-circuit the parity gates.")
    ap.add_argument("--assert-spectator-catchup", action="store_true",
                    help="exit nonzero if a spectator's boot -> first-played-"
                         "frame wall clock exceeds --catchup-secs (or if it "
                         "never played a frame). Always computed + printed; "
                         "this flag makes it a gate.")
    ap.add_argument("--catchup-secs", type=float, default=DEEP_JOIN_CATCHUP_SECS,
                    help="threshold in seconds for --assert-spectator-catchup "
                         "(default 10, deliberately lenient: measured deep joins "
                         "are 2.0-3.1s, with a 5.9s outlier when the first "
                         "SPEC_JOIN_REQ is lost and the 3500ms re-JOIN cadence "
                         "recovers it)")
    args = ap.parse_args()
    min_coverage = args.min_coverage if args.min_coverage >= 0 else args.frames - 100
    measure_host = bool(os.environ.get("FM2K_PERF_PROFILE"))

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    p1_pty   = OUT_DIR / "p1_parity.pty"
    if p1_pty.exists():
        p1_pty.unlink()
    # Per-spectator parity files are created below in the `specs` list.
    spec_phases = [p.strip() for p in args.spectators.split(",") if p.strip()]

    # --game-exe overrides the registry path for --game (Phase 4e, review A4d).
    # Without it a caller's "which install?" variable is DECORATIVE: run_all_tests
    # stage 2g guarded on $VANPRI_EXE while the harness resolved vanpri from the
    # hardcoded GAMES entry, so pointing the variable at a different install
    # would have passed the guard and then silently run the other copy -- the
    # exact shape of the ShadowArts defect (d0455bc), where a stage reported PASS
    # while the game it named had never run.
    game_exe = Path(args.game_exe) if args.game_exe else GAMES[args.game]
    if not game_exe.exists():
        print(f"[harness] FATAL: --game {args.game} exe not found: {game_exe}"
              + (" (from --game-exe)" if args.game_exe else ""))
        return 2
    global IS_FM95_RUN
    IS_FM95_RUN = (args.game in FM95_GAMES)
    print(f"[harness] game={args.game} ({game_exe.name}) engine="
          f"{'FM95' if IS_FM95_RUN else 'FM2K'} path={game_exe}")
    game_arg = to_win(game_exe)
    game_dir = game_exe.parent
    kill_strays()
    time.sleep(1.0)
    wait_ports_free([P1_PORT, P2_PORT] + [SPEC_PORT + k for k in range(len(spec_phases))])

    # ---- log lifecycle -------------------------------------------------------
    # The game's per-role debug logs live in the GAME dir and SURVIVE across
    # runs (each new process truncates its own on open -- eventually). Two
    # harness mechanisms read them, and a stale file corrupts BOTH:
    #   * count_marker() below decides WHEN a css[N]/battle[N] spectator dials
    #     in. Against a previous run's markers it fires instantly, so the run
    #     silently tests a DIFFERENT SCENARIO and says nothing: wave-4.1
    #     rematch seed 83 dialled both spectators in at session start, turning a
    #     "join between matches 2 and 3" run into a session-start join.
    #   * the live_ preservation step feeds every parity gate; if a spectator
    #     never launches, the copy would be the PREVIOUS run's spectator log.
    # Clear them here (kill_strays already ran, so nothing holds a handle) and,
    # for the case where Windows still refuses the unlink, remember the current
    # size so counting can start past it.
    log_dir = game_dir / "logs"
    stale_bases = {}
    for _lf in (["FM2K_P1_Debug.log", "FM2K_P2_Debug.log"]
                + [f"FM2K_S{k + 1}_Debug.log" for k in range(len(spec_phases))]):
        _p = log_dir / _lf
        try:
            _p.unlink()
            stale_bases[_p] = 0
        except FileNotFoundError:
            stale_bases[_p] = 0
        except OSError as e:
            try:
                stale_bases[_p] = _p.stat().st_size
            except OSError:
                stale_bases[_p] = 0
            print(f"[harness] (warn) could not clear stale {_lf} ({e}) -- "
                  f"markers will be counted past byte {stale_bases[_p]}")
    print(f"[harness] cleared {len(stale_bases)} stale game-dir debug log(s) "
          f"under {log_dir}")

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
    for k in ("FM2K_LOCAL_DELAY", "FM2K_PRED_WINDOW", "FM2K_PREDICTION_WINDOW", "FM2K_RUNAHEAD", "FM2K_AUTOPLAY_CSS_DWELL", "FM2K_SPECTATOR_DEBUG", "FM2K_HOST_TRACE", "FM2K_FA_TRACE", "FM2K_TEST_BATTLE_SEED",
              # host-clock sync + rift frame pacing A/B
              "FM2K_HOST_CLOCK",
              # ReliableChannel spectator A/B transport (reliable-ordered+FEC over UDP)
              "FM2K_RC_FEC", "FM2K_RC_FEC_K",
              # task #55: dead-TCP simulation (spectator dials a dead port);
              # with RC default-on the spectate must fully work regardless.
              "FM2K_RC_RESEND_MS", "FM2K_RC_RATE_PPS", "FM2K_RC_CWND",
              "FM2K_RC_STATS",
              # in-process link impairment (players' gekko+control path)
              "FM2K_NET_DELAY_MS", "FM2K_NET_JITTER_MS", "FM2K_NET_LOSS", "FM2K_NET_SEED",
              "FM2K_NET_REORDER", "FM2K_NET_DUP",
              # CSS lockstep input-delay override (#65)
              "FM2K_CSS_DELAY",
              # #66 CSS rollback opt-in + prediction window
              "FM2K_CSS_ROLLBACK", "FM2K_CSS_PREDICTION",
              # mid-join spectate desync hunt: per-region full-state fingerprint
              "FM2K_FULLFP", "FM2K_POOLSET",
              # Phase 4c match-start pool resync kill-switch. Default ON in the
              # hook, so this only ever needs forwarding to turn it OFF -- but
              # it MUST reach the host as well as the viewers: the host half is
              # what serves the per-battle snapshot at all.
              "FM2K_SPEC_POOL_SYNC",
              # RC ordered-channel liveness kill-switch (S0-S3 starve fix).
              # Default ON in the hook; forwarded so the causality control run
              # (=0 restores the flat-TTL behaviour and the permanent loss with
              # it) is reachable from a WSL-side invocation. BOTH sides: the
              # SENDER half owns retention + the holding advertisement and the
              # RECEIVER half owns the repair, so a one-sided switch would
              # measure half a fix.
              "FM2K_RC_LIVENESS",
              # ORPHANED-PREVIEW FIX causality control. FM2K_SPEC_CSS_UNLOAD=0
              # makes CssAutoConfirm's pin write selected=-1 WITHOUT the
              # engine's paired Css_UnloadPlayerPreview, i.e. reproduces the
              # orphaned character-select preview byte-for-byte on the same
              # binary. Default ON in the hook; forwarded to BOTH sides for the
              # same reason as every other kill switch here -- a one-sided env
              # list is how a control arm silently measures half a fix.
              "FM2K_SPEC_CSS_UNLOAD",
              # Phase 4e (review A4a(ii)): the topology gate's OWN escape hatch.
              # FM2K_CK_TOPOLOGY=0 makes ComputeTopology() return the "not
              # computed" sentinel, which the POOL terms now treat as a FAILED
              # fatal term rather than a match. Forwarded to BOTH sides so the
              # hatch is reachable (and visible) from a WSL-side invocation
              # instead of only through a Windows-level environment variable.
              "FM2K_CK_TOPOLOGY",
              # match-end seam diagnosis (Phase 1bc): per-save fingerprint ring
              # + [SEAM] detail lines. Dark by default in the hook.
              "FM2K_SEAM_TRACE",
              # Per-save region hashes inside that ring (docs/dev/
              # seam_ring_intermittent.md). SEPARATE from FM2K_SEAM_TRACE and
              # default off because the hashes cost ~6 us per save and the
              # violation they diagnose may be timing-sensitive. Forwarding is
              # NOT optional: without it the columns silently emit 0x00000000
              # for every row and a whole 24-run hunt reads as "all regions
              # match" when in fact nothing was ever hashed.
              "FM2K_SEAM_HASH",
              # Envelope inversion phase 1 (SHADOW MODE): full-image per-256B
              # block hashes diffed forward-vs-replay, so every remaining hole in
              # the save envelope names itself. Observe only -- it restores
              # nothing -- but it costs ~360us per forward save, so it is dark by
              # default in the hook and must be requested explicitly. Forwarded to
              # BOTH players because the instrument is per-process and a one-sided
              # forward is the recorded half-blind-run trap.
              # FM2K_ENVELOPE_SHADOW_WITNESS=0 drops the byte-level witness ring
              # (block resolution only, ~20MB and ~280us/save cheaper).
              "FM2K_ENVELOPE_SHADOW", "FM2K_ENVELOPE_SHADOW_WITNESS",
              # Phase 4b spectator across-match-boundary diagnosis. ALL of
              # these must ALSO reach the spectators (see the spectator list
              # below) -- forwarding them here only is exactly the recorded
              # trap that produces a half-blind run: the probe fires on P1/P2
              # and silently never arms on the plane under investigation.
              #   FM2K_FACING_TRACE  -- [FACING] ring, both planes (dark)
              #   FM2K_FULL_CRCS     -- per-region CRC on EVERY save, not 1/sec
              #   FM2K_EB_DIAG       -- shake/palette/screen timers (rank 4)
              #   FM2K_SPEC_FINGERPRINT -- [HOST-FP]/[SPEC-FP] pairing
              "FM2K_FACING_TRACE", "FM2K_FULL_CRCS", "FM2K_EB_DIAG",
              "FM2K_SPEC_FINGERPRINT",
              # [BATTLE-OBJ] battle-frame object census (css_window.h). MUST
              # reach BOTH PLAYERS -- diffing P1's f=1 listing against P2's is
              # the entire instrument, and a one-sided forward measures
              # nothing. Deliberately NOT folded into FM2K_CSS_WIN, which is
              # host-only by review B2; this one is symmetric by design and
              # costs two dump events per battle rather than per-frame IO.
              "FM2K_BATTLE_F1",
              # FM2K_HOSTCONFIG_LATE: the forcing lever for the stale-latch
              # race (host suppresses every HOST_CONFIG broadcast before the
              # battle-entry signal, so the guest is GUARANTEED to latch its
              # own game.ini round count instead of waiting on a ~1/4 packet
              # loss coin flip). Dark by default and only "1" arms it, so
              # plain truthiness forwarding is correct here.
              "FM2K_HOSTCONFIG_LATE",
              # Phase 6 (a) STAGE SWITCHING. The three random-stage vars go to
              # every plane on purpose, spectators INCLUDED. The spectator must
              # never roll (Hook_LoadStageFile excludes g_spectator_mode and
              # takes the stage from HOST_CONFIG instead), and the leg asserts
              # exactly that -- an assertion which is VACUOUS if the viewer
              # simply never received the env. Handing it the seed and then
              # proving it still emits no override line tests the code gate.
              "FM2K_STAGE_RANDOM_SEED", "FM2K_STAGE_RANDOM_MIN",
              "FM2K_STAGE_RANDOM_MAX",
              # Phase 6 (b) SETTINGS VARIANCE. FM2K_TEST_GAME_SPEED is exported
              # to both players and read HOST-ONLY inside HostApplyMatchSetting
              # Overrides, exactly like FM2K_TEST_ROUND_TIME: the guest and the
              # spectators must reach the same speed through HOST_CONFIG or the
              # harness masks the delivery path it is supposed to gate.
              "FM2K_TEST_GAME_SPEED",
              # Story-only refusal (title_mode_select.cpp). All three are
              # DEFAULT-CORRECT in the hook and forwarded only so a run can
              # exercise the red arms: FM2K_NO_VS_REFUSE=0 is the kill switch
              # (restores the pre-fix silent 1P/STORY netplay),
              # FM2K_TITLE_FORCE_NO_VS=1 makes a VS-capable game take the
              # refusal path, and FM2K_TITLE_FORCE_MODE_INDEX=<n> pins the
              # title cursor at a non-VS entry so the TEAM-PIN term can be
              # red-proofed on wanwan.
              "FM2K_NO_VS_REFUSE",
              "FM2K_TITLE_FORCE_NO_VS",
              "FM2K_TITLE_FORCE_MODE_INDEX",
              # [CFG] per-battle three-plane settings stamp (round_events.cpp).
              # DEFAULT ON in the hook; forwarded so a run can turn it off.
              "FM2K_CFG_TRACE",
              # Battle-entry settings barrier budget. The settings leg's
              # PLAYER-half red proof shortens it so a starved guest reaches
              # the "MATCH SETTINGS NEVER AGREED" force-complete inside the
              # run's wall clock instead of after the default 10s.
              "FM2K_CFG_BARRIER_FORCE_MS"):
        if os.environ.get(k):
            common_env[k] = os.environ[k]
    # FM2K_TEST_ROUNDS_HOST_ONLY: suppresses the per-frame g_default_round force
    # in hooks_update.cpp so round count travels the real HOST_CONFIG delivery
    # path. Forwarded BY PRESENCE for the usual reason, and it must reach EVERY
    # plane -- the force it disables runs on the guest and the spectator too, so
    # a one-sided forward would leave the very peers under test writing the right
    # answer on top of whatever was delivered. The spectator half is in the
    # per-spectator list below.
    if os.environ.get("FM2K_TEST_ROUNDS_HOST_ONLY") is not None:
        common_env["FM2K_TEST_ROUNDS_HOST_ONLY"] = os.environ["FM2K_TEST_ROUNDS_HOST_ONLY"]
    # FM2K_SOCD_MODE is deliberately NOT in the list above: it is read
    # per-process (hooks_input.cpp) and setting it on both peers would make the
    # SOCD variant a local-config test instead of a delivery test. It goes to
    # the HOST ONLY, below, and every other plane must obtain it from
    # HOST_CONFIG -- which is also what the entry barrier's digest gates.
    # FM2K_SEAM_LEGACY_PARK: same presence-not-truthiness rule. It is the
    # DIAGNOSTIC A/B lever that restores the deleted blanket load-site park
    # (i.e. reinstates the match-end-seam desync on purpose), default OFF in
    # the hook. Forwarding BY PRESENCE keeps "0"/"" round-tripping faithfully
    # instead of relying on Python's "0" being truthy.
    if os.environ.get("FM2K_SEAM_LEGACY_PARK") is not None:
        common_env["FM2K_SEAM_LEGACY_PARK"] = os.environ["FM2K_SEAM_LEGACY_PARK"]
    # FM2K_ROUNDS_RELATCH: battle-entry re-derive of g_round_limit (the stale
    # latch that gives a late-HOST_CONFIG guest +2 HUD pips and a split
    # match-end predicate). DEFAULT ON in the hook -- this is the KILL SWITCH,
    # so the value that matters is "0", and it is forwarded BY PRESENCE for the
    # same reason FM2K_SEAM_LEGACY_PARK is: an `if os.environ.get(k)` filter would
    # forward "0" only by accident of Python truthiness, and one refactor to
    # int()/== "1" would silently stop forwarding the OFF direction, turning
    # every red-arm A/B run into a second measurement of the default. Both
    # peers get it: the latch is per-process and the guest is the one that
    # strands it, but a one-sided lever makes an A/B unreadable.
    # ONE SWITCH, BOTH PLANES: the same variable also arms/disarms the VIEWER
    # re-derive at the MATCH_START apply (spec_relatch.cpp). See the viewer env
    # list below -- it is no longer inert there.
    if os.environ.get("FM2K_ROUNDS_RELATCH") is not None:
        common_env["FM2K_ROUNDS_RELATCH"] = os.environ["FM2K_ROUNDS_RELATCH"]
    # FM2K_CSS_PARK: pre-rendezvous character-select park (netplay_css.cpp).
    # DEFAULT ON in the hook -- it is the fix for the sim-silent player-plane
    # pool transposition (top=/bind= red from battle f=0 at ~1-in-3), so the
    # value that matters is "0" and it is forwarded BY PRESENCE for exactly the
    # reason the two switches above are: `if os.environ.get(k)` forwards "0"
    # only by accident of Python truthiness, and one refactor to int()/== "1"
    # silently turns every red arm into a second measurement of the default.
    # BOTH peers get it -- a one-sided park is a phase offset by construction,
    # i.e. the bug, so a half-forwarded lever would measure nothing.
    # A/B SCAFFOLDING: delete this block when the switch is deleted.
    if os.environ.get("FM2K_CSS_PARK") is not None:
        common_env["FM2K_CSS_PARK"] = os.environ["FM2K_CSS_PARK"]
    # FM2K_SEAM_GUARD is RETIRED. It is still FORWARDED (so the hook's loud
    # "RETIRED and IGNORED" line lands in the Debug log) and warned about here,
    # because an old recipe that silently measures the default is exactly how a
    # bisect reaches the wrong conclusion.
    if os.environ.get("FM2K_SEAM_GUARD") is not None:
        common_env["FM2K_SEAM_GUARD"] = os.environ["FM2K_SEAM_GUARD"]
        print("WARNING: FM2K_SEAM_GUARD is RETIRED and has no effect. "
              "Use FM2K_SEAM_LEGACY_PARK=1 to restore the old blanket "
              "load-site park for an A/B.")

    # [CSS-WIN]/[CSS-OBJ] -- the character-select window gate (css_window.h).
    # DEFAULT ON in the harness, unlike the other diagnostics above: its
    # not-computed-must-fail rule is only meaningful if the term is normally
    # computed, and a gate nobody switches on is a gate that never sees its
    # bug (the ShadowArts shape). The cost is one log line per 30 in-window
    # frames per plane, on a screen that is not a hot path; the battle loop is
    # never reached (exact phase gate). Explicitly settable to 0 to A/B it.
    #
    # HOST (P1) + SPECTATORS ONLY -- deliberately NOT the guest (review B2).
    # The gate reads live_FM2K_P1_Debug.log for the host half and each
    # spectator's live log for the other; **P2's [CSS-WIN] lines are read by
    # nothing**, so carrying the instrument on the guest bought zero coverage.
    # What it did buy was a per-confirmed-CSS-frame 1024-slot pool walk plus a
    # synchronous log line every 30 frames on the guest process, during exactly
    # the phase whose wall-clock timing decides which frame each peer crosses
    # the match boundary on -- i.e. a self-inflicted timing perturbation, on one
    # side only, injected by default into the gate that is currently arbitrating
    # an open player-plane desync. Symmetry is what the HOST-vs-SPECTATOR
    # comparison needs; the guest is not a party to it.
    _css_win = os.environ.get("FM2K_CSS_WIN", "1")
    # Retune of the [CSS-OBJ] dump trigger only (px below the resting level) (never a verdict; the
    # harness FALL term compares against the host's own measured ceiling).
    _css_fall_delta = os.environ.get("FM2K_CSS_FALL_DELTA")
    # [CSS-ANIM] per-object script-ADVANCE census (Wave-2 review B1e: does the
    # spectator CSS park freeze the character previews the host animates?).
    # Dark by default. Forwarded to the HOST and the SPECTATORS and deliberately
    # NOT to the guest, for the same reason FM2K_CSS_WIN is host-only (review
    # B2): the comparison is host-vs-spectator, P2's lines are read by nothing,
    # and the extra per-frame pool walk is a one-sided timing perturbation in
    # exactly the phase that decides the open player-plane desync.
    _css_anim = os.environ.get("FM2K_CSS_ANIM")
    # O1 A/B LEVER (2026-08-18) -- the VIEWER-ONLY instruments-off arm.
    #
    # The open measurement: a deep-joining viewer mirrored the host's
    # character-select at 17.9 frames/s against the 65 frames/s the host had
    # produced them at (931 INPUT frames in 51.9s vs 14.3s), which is why
    # `boot -> deep-join APPLIED` reads 40-53s. Three candidate causes are not
    # separated: CSS sim frames are genuinely expensive, the playout loop is
    # hard-capped at 4 frames per outer tick, and THIS HARNESS ARMS
    # FM2K_CSS_WIN + FM2K_CSS_ANIM ON THE VIEWERS -- a per-character-select-frame
    # 1024-slot pool walk with synchronous logging, on the very plane whose
    # throughput is being measured.
    #
    # FM2K_CSS_WIN=0 alone is NOT that experiment: it is symmetric, so it also
    # un-arms the host and moves the thing being compared against. This lever
    # drops both instruments from the SPECTATOR env ONLY, leaving the host's
    # arming (and therefore the host's timing) exactly as the A arm has it. The
    # decisive comparison is `boot -> deep-join APPLIED` between the two arms on
    # the same seed; a material drop means the instrument owns the number and the
    # harness must stop arming a per-frame pool walk on the plane it measures.
    # Defaults ON (=1) so every existing run is unchanged.
    _spec_css_instruments = os.environ.get("FM2K_SPEC_CSS_INSTRUMENTS", "1") \
        not in ("0", "")

    p1_env = {**common_env,
              "FM2K_CSS_WIN": _css_win,
              "FM2K_LOCAL_PORT": str(P1_PORT),
              "FM2K_REMOTE_ADDR": f"127.0.0.1:{P2_PORT}",
              "FM2K_PARITY_RECORD_PATH": to_win(p1_pty)}
    if _css_fall_delta:
        p1_env["FM2K_CSS_FALL_DELTA"] = _css_fall_delta
    if _css_anim:
        p1_env["FM2K_CSS_ANIM"] = _css_anim
    # HOST-ONLY by design (Phase 6 settings-variance leg):
    #   FM2K_SOCD_MODE          -- a per-process input filter. On the host it is
    #                              the authoritative value HOST_CONFIG carries;
    #                              on any other plane it would be a local
    #                              setting, and the leg would stop testing
    #                              delivery. Also one of the five digest fields,
    #                              so a failure to deliver blocks battle entry.
    #   FM2K_SPEC_HOSTCFG_DROP  -- the RED-PROOF lever: the host stops sending
    #                              HOST_CONFIG to spectators (peer untouched).
    #                              Host-side only by construction.
    for _hk in ("FM2K_SOCD_MODE", "FM2K_SPEC_HOSTCFG_DROP"):
        if os.environ.get(_hk):
            p1_env[_hk] = os.environ[_hk]
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
    # decides session-start backfill (CSS-walk) vs battle snapshot
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
               # [CSS-WIN]/[CSS-OBJ] character-select window gate -- the OTHER
               # half of the HOST-side default above (the guest deliberately
               # does not carry it; see the FM2K_CSS_WIN comment there). The
               # whole point of the gate is the host-vs-spectator comparison, so
               # THIS default must stay symmetric with P1's or the run measures
               # one plane against nothing.
               "FM2K_CSS_WIN": (_css_win if _spec_css_instruments else "0")}
        if _css_fall_delta:
            env["FM2K_CSS_FALL_DELTA"] = _css_fall_delta
        # [CSS-ANIM] viewer half. The whole instrument is a host-vs-spectator
        # comparison, so this MUST stay symmetric with P1's above -- unless the
        # O1 viewer-only instruments-off arm is armed, which deliberately breaks
        # that symmetry and therefore also gives up the CSSANIM term for the run
        # (the term correctly refuses to score not-computed as a pass, so such a
        # run is a MEASUREMENT arm, not a gate run).
        if _css_anim and _spec_css_instruments:
            env["FM2K_CSS_ANIM"] = _css_anim
        # FM2K_SPEC_HOST_GONE_MS: NOT set here any more (H1, 2026-08-18).
        #
        # This harness used to hard-default it to 5000 "so the viewer exits ~5s
        # after the host's feed stops instead of spinning at q=0". The shipped
        # default is 12000 and it is the LAST rung of the spectator recovery
        # ladder -- RC stall repair at 3500, escalation #1 at 4000 (up to 7500
        # under active RC repair), escalation #2 4000ms behind that. At 5000 the
        # viewer kills itself BEFORE any of its own repairs can run, which the
        # hook warns about at boot in every single log this harness has ever
        # produced. laneC_triage filed it as Fix 6 on 2026-08-15; on 2026-08-18
        # its predicted consequence arrived and cost a soak cycle: a 6.26s
        # whole-machine stall (all four processes frozen to the millisecond)
        # sat inside the 12000 budget and outside the 5000 one, so both viewers
        # exited 2m16s before the host and the run was triaged as a starve it
        # was not. Truncating the thing under test is not a shorter teardown, it
        # is a different product.
        #
        # Deliberately still OVERRIDABLE from the environment (below), and
        # tools/spec_host_gone_test.sh keeps its own short value in-source with
        # the reason -- that stage must beat harness cleanup and is measuring
        # the give-up itself.
        for kk in ("FM2K_SPEC_HOST_GONE_MS",
                   # P1 red/green lever: one-shot whole-network freeze on the
                   # viewer (dark by default). Spectator-only by construction.
                   "FM2K_SPEC_TEST_SELFSTALL_MS",
                   # P1 kill switch (A/B scaffolding, default armed): the red
                   # arm of the self-stall credit. Presence-forwarded via the
                   # explicit block below so "0" is never dropped.
                   "FM2K_CSS_TRACE",
                   "FM2K_SPECTATOR_DEBUG", "FM2K_SPEC_CONNECT_TIMEOUT_MS",
                   "FM2K_NET_DELAY_MS", "FM2K_NET_JITTER_MS", "FM2K_NET_LOSS", "FM2K_NET_SEED",
                   "FM2K_NET_REORDER", "FM2K_NET_DUP",
                   # task #55: the RC transport family MUST match the host's
                   # or the pair runs a split-brain transport config (host
                   # streaming RC while the viewer runs TCP-primary logic
                   # incl. the TCP-fail give-up) -- this exact gap
                   # invalidated a night of A/B runs.
                   "FM2K_RC_STATS",
                   "FM2K_RC_FEC", "FM2K_RC_FEC_K",
                   # mid-join spectate desync hunt: per-region full-state fingerprint
                   "FM2K_FULLFP", "FM2K_POOLSET",
                   # Phase 4c match-start pool resync kill-switch (viewer half).
                   "FM2K_SPEC_POOL_SYNC",
                   # RC liveness kill-switch, viewer half -- this is the half
                   # that honours the holding advertisement and defers the
                   # starve escalation.
                   "FM2K_RC_LIVENESS",
                   # ORPHANED-PREVIEW FIX causality control, viewer half -- this
                   # is the half that actually runs the pin, so it MUST be here.
                   "FM2K_SPEC_CSS_UNLOAD",
                   # Phase 4e: the topology gate escape hatch, viewer half. It
                   # MUST be symmetric with the player list -- the POOL terms
                   # compare the two planes, so a one-sided hatch would silently
                   # disable the gate from whichever side got it.
                   "FM2K_CK_TOPOLOGY",
                   # Phase 4b: the SPECTATOR half of the diagnosis env. The
                   # whole point of the phase is to compare the two planes, so
                   # every probe listed for the players above must appear here
                   # too or the run measures the host against nothing.
                   # FM2K_HOST_TRACE is included because [SPEC-FP]'s sibling
                   # [HOST-FP] is gated on it and the pair is read together.
                   "FM2K_FACING_TRACE", "FM2K_FULL_CRCS", "FM2K_EB_DIAG",
                   "FM2K_SPEC_FINGERPRINT", "FM2K_HOST_TRACE",
                   "FM2K_SEAM_TRACE", "FM2K_SEAM_HASH",
                   # Envelope inversion phase 1 shadow mode, viewer half. Kept
                   # symmetric with the player list on principle. Honest note,
                   # same shape as the [BATTLE-OBJ] entry below: the instrument
                   # hangs off SaveState_Save, which Phase 4b established never
                   # runs on a spectator, so this is expected to produce an
                   # all-zero [ENVSHADOW] summary and no CSV on S*. If a
                   # spectator ever emits a non-zero one, that is itself a finding.
                   "FM2K_ENVELOPE_SHADOW", "FM2K_ENVELOPE_SHADOW_WITNESS",
                   # Phase 6 (a) stage switching, viewer half. Forwarded so the
                   # "the spectator NEVER rolls" assertion tests the code gate
                   # (Hook_LoadStageFile's g_spectator_mode exclusion) rather
                   # than the absence of the env -- see the player list above.
                   "FM2K_STAGE_RANDOM_SEED", "FM2K_STAGE_RANDOM_MIN",
                   "FM2K_STAGE_RANDOM_MAX",
                   # Story-only refusal, viewer half -- the spectator plane
                   # runs the SAME title path and must refuse identically.
                   "FM2K_NO_VS_REFUSE",
                   "FM2K_TITLE_FORCE_NO_VS",
                   "FM2K_TITLE_FORCE_MODE_INDEX",
                   # [CFG] three-plane settings stamp, viewer half. This is the
                   # plane the stamp EXISTS for: a spectator can neither emit a
                   # barrier packet nor be checked by one, so its line is the
                   # only evidence that it applied the host's settings.
                   "FM2K_CFG_TRACE",
                   # [BATTLE-OBJ] census, viewer half. Symmetric on principle
                   # (the recorded trap is a one-sided env list), with an
                   # honest note: the census hangs off SaveState_Save, which
                   # Phase 4b established never runs on a spectator, so this
                   # is expected to produce NO [BATTLE-OBJ] lines on S*. If it
                   # ever does, that is itself a finding.
                   "FM2K_BATTLE_F1"):
            if os.environ.get(kk):
                env[kk] = os.environ[kk]
        # FM2K_SPEC_SELFSTALL, viewer-only by construction (the watchdog it
        # gates runs on the spectator plane). Presence-forwarded, never
        # truthiness: `if os.environ.get(k)` drops "0" and turns every red arm
        # into a second measurement of the default.
        if os.environ.get("FM2K_SPEC_SELFSTALL") is not None:
            env["FM2K_SPEC_SELFSTALL"] = os.environ["FM2K_SPEC_SELFSTALL"]
        # FM2K_ROUNDS_RELATCH, viewer half -- NO LONGER INERT. It was, while the
        # only re-derive lived at the player battle-entry barrier (unreachable on
        # a spectator: hooks_update.cpp returns before the battle-sync block when
        # g_spectator_mode). The viewer now has its OWN re-derive at the
        # MATCH_START apply (spec_relatch.cpp, [SPEC-RELATCH] lines), under this
        # SAME switch, so "0" here disarms a real fix on this plane -- which is
        # exactly what the red arm needs. Presence-forwarded, never truthiness:
        # `if os.environ.get(k)` would drop "0" and turn every red arm into a
        # second measurement of the default.
        if os.environ.get("FM2K_ROUNDS_RELATCH") is not None:
            env["FM2K_ROUNDS_RELATCH"] = os.environ["FM2K_ROUNDS_RELATCH"]
        # FM2K_CSS_PARK, viewer half -- INERT BY CONSTRUCTION and forwarded
        # anyway. A spectator is pinned to LoopPhase::SPECTATOR_PLAYBACK by
        # ClassifyPhase, so it never calls Netplay_ProcessCSS and the park has
        # no site to act on; its character-select walk is driven entirely by
        # the host's recorded confirmed-input stream. Forwarded so a
        # kill-switch A/B is never split-brain across planes and so the
        # [CSS-RDV] contract line's absence on a viewer is a fact about the
        # plane rather than about the env. Presence-forwarded, never
        # truthiness. A/B SCAFFOLDING: delete with the switch.
        if os.environ.get("FM2K_CSS_PARK") is not None:
            env["FM2K_CSS_PARK"] = os.environ["FM2K_CSS_PARK"]
        # FM2K_TEST_ROUNDS_HOST_ONLY, viewer half. The per-frame force it
        # disables runs on the SPECTATOR too (hooks_update.cpp is reached before
        # the g_spectator_mode early return), and the spectator is the plane
        # whose round-count delivery has never been tested precisely because of
        # it. Presence-forwarded like the other kill switches.
        if os.environ.get("FM2K_TEST_ROUNDS_HOST_ONLY") is not None:
            env["FM2K_TEST_ROUNDS_HOST_ONLY"] = os.environ["FM2K_TEST_ROUNDS_HOST_ONLY"]
        # The spectator MUST run the same round count as the host, else a 1-round
        # host vs best-of-3 spectator diverges at the host's round-1 match-end.
        # Under FM2K_TEST_ROUNDS_HOST_ONLY this variable is INERT here (the force
        # that consumed it is off) and the value has to arrive via HOST_CONFIG --
        # which is the entire point of that switch. Still exported, so turning
        # the switch off restores the pre-Phase-6 behaviour byte-for-byte.
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
                      # Index of the earliest host match this viewer can
                      # possibly observe; filled in at dial-in (schedule_spec)
                      # and consumed by spectator_liveness. 0 = present from the
                      # session start, which is what every full-session joiner
                      # is and what the pre-2026-08 behaviour assumed for all.
                      "first_host_match": 0,
                      "rc": None})
    spec_pty = specs[0]["pty"]   # alias for the single-spec parity/replay code below

    start_ts = time.time()

    # STORY-ONLY EARLY EXIT. On a title with no VS mode the hook refuses and
    # terminates within ~1.5s of boot, but every wait in this file is keyed on
    # host progress that will now never happen, so the run would sit out its
    # full record-timeout (measured: players done at +60s, leg still waiting at
    # +300s) before the rc=3 verdict below could even be read. Fold the marker
    # into the completion predicates so the leg finishes in seconds.
    #
    # Safe by construction: the marker is emitted ONLY by the refusal path, on
    # content where every gate in this file is moot anyway.
    _refused_state = {"seen": False, "checked": 0.0}
    def session_refused():
        if _refused_state["seen"]:
            return True
        now = time.time()
        if now - _refused_state["checked"] < 1.0:
            return False
        _refused_state["checked"] = now
        for lf in ("FM2K_P1_Debug.log", "FM2K_P2_Debug.log"):
            try:
                with open(game_dir / "logs" / lf, "rb") as fh:
                    if b"[NOVSMODE] REFUSING SESSION" in fh.read():
                        _refused_state["seen"] = True
                        return True
            except OSError:
                continue
        return False

    def has_new_replay():
        if session_refused():
            return True
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
            if session_refused():
                return True
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

    t1.start()
    time.sleep(1.0)
    t2.start()

    spec_threads = []
    host_live = game_dir / "logs" / "FM2K_P1_Debug.log"
    def count_marker(marker):
        """Occurrences of `marker` in the host's LIVE debug log, counting ONLY
        what THIS run wrote. Defence in depth with the log-lifecycle block
        above: the baseline is normally 0 (the file was deleted), but when the
        unlink was refused we count past the recorded size, and if the file ever
        SHRINKS -- the new host truncating it -- the baseline drops back to 0 so
        this run's own markers can never be skipped."""
        base = stale_bases.get(host_live, 0)
        try:
            with open(host_live, "rb") as fh:
                fh.seek(0, 2)
                if fh.tell() < base:      # truncated => the new host owns it all
                    base = 0
                    stale_bases[host_live] = 0
                fh.seek(base)
                data = fh.read()
        except OSError:
            return 0
        return data.decode("utf-8", "replace").count(marker)

    def schedule_spec(s):
        # phase = "css[N]" or "battle[N]": dial in during the host's Nth CSS / Nth
        # battle (default N=1). css[N] -> a CSS-walk/seam join while the host is
        # in its Nth char-select; battle[N] -> a CURRENT_MATCH snapshot join mid the
        # host's Nth battle. Keyed off host-log markers so it tracks the real phase
        # under loss/jitter rather than a fixed wall clock.
        phase = s["phase"]
        kind = "css" if phase.startswith("css") else "battle"
        suffix = phase[len(kind):]
        n = int(suffix) if suffix.isdigit() else 1
        marker = "CSS: Entered" if kind == "css" else "GekkoNet battle session created"
        deadline = time.time() + args.record_timeout
        while (time.time() < deadline and count_marker(marker) < n
               and not session_refused()):
            time.sleep(0.25)
        # Settle: a css spec waits spec_join_delay into the CSS; a battle spec waits
        # battle_join_offset so the Nth battle session exists before the snapshot.
        time.sleep(args.spec_join_delay if kind == "css" else args.battle_join_offset)
        # Liveness baseline (--assert-spectator-live): the earliest host match
        # this viewer can possibly observe. A css[N] joiner dials in AFTER the
        # host's first N-1 battles, so its first observable match index is that
        # battle count; a battle[N] joiner dials into battle N itself, index
        # count-1. For a BOUNDED deep joiner (css[N>1] with
        # the bounded between-matches join) this is exactly the set of matches it is
        # designed never to replay.
        battles_seen = count_marker("GekkoNet battle session created")
        s["first_host_match"] = (battles_seen if kind == "css"
                                 else max(0, battles_seen - 1))
        print(f"[harness] {s['tag']} ({phase}) dialing in -- host reached {kind} "
              f"#{n} (host battles so far: {battles_seen}; liveness baseline: "
              f"first observable host match = {s['first_host_match']})")
        launch_spec(s)

    for s in specs:
        t = threading.Thread(target=schedule_spec, args=(s,)); t.start()
        spec_threads.append(t)

    t1.join(); t2.join()
    for t in spec_threads:
        t.join()
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

    # ---- MACHINE STALL: VOID, not PASS and not FAIL (rc=4) -------------------
    # See machine_stall_report's header for the rule and the capture that forced
    # it. Printed ALWAYS (a `MACHINE: 0 stall(s)` line on every clean run), and
    # checked HERE -- immediately after preservation, before a single product
    # term is computed. That placement is the point: a run whose four processes
    # each spent seconds not being scheduled did not measure the product, and
    # printing its terms anyway is how the 2026-08-18 leg got triaged as a
    # spectator starve. The stall table below IS the evidence; the raw logs are
    # preserved either way.
    try:
        _gone_ms = int(os.environ.get("FM2K_SPEC_HOST_GONE_MS") or "12000")
    except ValueError:
        _gone_ms = 12000
    _mach = machine_stall_report(OUT_DIR, host_gone_ms=_gone_ms)
    for _l in machine_stall_lines(_mach):
        print(_l)
    if _mach["void"]:
        # MACHINE-CONFIRMED vs UNCONFIRMED is the difference between "the box
        # is sick" and "something went quiet and we cannot prove what". Only
        # the confirmed form may be read as a hardware verdict -- soak_driver.sh
        # keys its machine-unfit SOAK_STOP on this line (review H5).
        _conf = "MACHINE-CONFIRMED" if _mach.get("confirmed") else \
                "MACHINE-UNCONFIRMED"
        print(f"[harness] OVERALL VOID (rc=4) [{_conf}]: MACHINE STALL -- "
              f"{_mach['reason']}. {MACHINE_MIN_PROCS}+ of "
              f"{len(_mach['procs'])} game processes stopped logging within "
              f"{MACHINE_ONSET_MAX_S * 1000:.0f}ms of each other; this run is "
              f"NOT a PASS and NOT a FAIL because it measured nothing. Re-run "
              f"this leg.")
        if _mach.get("confirmed"):
            print(f"[harness]   MACHINE-CONFIRMED: a viewer went silent while "
                  f"still HOLDING buffered content, which neither network "
                  f"starvation nor a host-side wedge can cause. A leg that "
                  f"VOIDs twice this way means the machine is unfit to measure "
                  f"with (memtest / close the scanner / drop the process "
                  f"count), not that the product regressed.")
        else:
            print(f"[harness]   MACHINE-UNCONFIRMED: the onsets coincided but "
                  f"the viewer-content discriminator did not confirm it (no "
                  f"[SPEC-Q] depth before the gap). Treat this as an "
                  f"unmeasured run, NOT as evidence about the hardware -- a "
                  f"reproducible PRODUCT hang must red, and if this VOID "
                  f"repeats, read the preserved logs before blaming the box.")
        return 4

    # ---- VIEWER SELF-STALL: a fact about the run, never silent ---------------
    # [SPEC-SELFSTALL] means a viewer process did not run for a while and
    # credited that silence back to its host-gone budget (P1). That credit is
    # the right behaviour, and it must never be a QUIET way for a bad run to
    # look good -- a viewer that spent seconds descheduled did not measure the
    # timings this harness reports. FATAL above the same 1.5s the machine-stall
    # detector uses (below it, a sub-second scheduling hiccup on a machine
    # running four games is noise and reporting it fatally would only make the
    # gate flaky); advisory-with-a-count below. Suppressed when the run itself
    # armed the freeze (FM2K_SPEC_TEST_SELFSTALL_MS -- the red/green arm).
    import re as _ss_re
    _selfstall_fatal = []
    _ss_pat = _ss_re.compile(r"\[SPEC-SELFSTALL\] this viewer did not run for (\d+)ms")
    # NOT bool(): the string "0" is TRUTHY in Python, so a DISARMED lever
    # (FM2K_SPEC_TEST_SELFSTALL_MS=0, which the hook reads as "no freeze")
    # would silently disarm this FATAL term. Same class of mistake this file
    # calls out three times elsewhere; caught by review 2026-08-18.
    _ss_armed = os.environ.get("FM2K_SPEC_TEST_SELFSTALL_MS") not in (None, "", "0")
    for _s in specs:
        _hits = []
        try:
            for _ln in open(_s["live"], errors="ignore"):
                _m = _ss_pat.search(_ln)
                if _m:
                    _hits.append(int(_m.group(1)))
        except OSError:
            continue
        if not _hits:
            continue
        _worst = max(_hits)
        _tag = "TEST-ARMED" if _ss_armed else ("FATAL" if _worst >= 1500 else "advisory")
        print(f"[harness] SELFSTALL {_s['tag']}: {len(_hits)} self-stall(s) "
              f"credited, worst {_worst}ms [{_tag}] -- this viewer process was "
              f"not scheduled; its latency numbers are not trustworthy for that "
              f"window")
        if _worst >= 1500 and not _ss_armed:
            _selfstall_fatal.append(f"{_s['tag']} froze for {_worst}ms")

    # ---- STORY-ONLY CONTENT: NOT-APPLICABLE, not FAIL (rc=3) -----------------
    # Eight of the 98 engine-identical games in the library are story-only:
    # their .kgt enables 1P/STORY and nothing else, so g_game_mode_flag can
    # never reach 1 (VS 1v1) and the hook now REFUSES to arm a netplay or
    # spectator session on them ([NOVSMODE] REFUSING SESSION, then terminate).
    # That refusal is the CORRECT behaviour, so a rotation leg pointed at such a
    # game must report NOT-APPLICABLE, never FAIL. A leg that stays red on a
    # documented non-goal gets disabled, taking its real coverage with it -- the
    # same argument the CSS-window advisory already won.
    #
    # It must also never be SILENT: rc=3 is distinct from both PASS (0) and FAIL
    # (1), and the reason is printed. Offline determinism keeps these games (see
    # multigame_determinism_sweep.sh) -- they legitimately pass there.
    _novs = []
    for _tag, _lp in ([("P1", OUT_DIR / "live_FM2K_P1_Debug.log"),
                       ("P2", OUT_DIR / "live_FM2K_P2_Debug.log")]
                      + [(s["tag"], s["live"]) for s in specs]):
        try:
            with open(_lp, errors="ignore") as fh:
                for ln in fh:
                    if "[NOVSMODE] REFUSING SESSION" in ln:
                        _novs.append((_tag, ln.strip()[:200]))
                        break
        except OSError:
            continue
    if _novs:
        print(f"[harness] NOT APPLICABLE (rc=3): this game has NO VS MODE -- the "
              f"hook refused the session on {len(_novs)} plane(s) "
              f"({', '.join(t for t, _ in _novs)}). Story-only content cannot "
              f"run netplay or spectating by construction (1P/STORY samples one "
              f"pad per battle frame and picks fighters from the story table), "
              f"so this is a DOCUMENTED NON-GOAL, not a regression. Offline play "
              f"and the offline determinism sweep are unaffected.")
        print(f"[harness]   {_novs[0][1]}")
        return 3

    # ---- LIVE-EDGE metric (the REAL "held the live edge" pass condition) ------
    # Computed from the PRESERVED live_ logs, right after preservation, so it runs
    # even if a later gate bails early. Always printed (informational); gated only
    # under --assert-spectator-live. This is a LIVENESS signal, kept SEPARATE from
    # the engine-desync gates and from the match-completion (.fm2krep) check.
    host_p1_log = OUT_DIR / "live_FM2K_P1_Debug.log"
    live_edge_fail = False
    # H4 (adversarial review, 2026-08-18): "the viewer left first" is a FACT
    # about the run, not an optional assertion, and it is kept in its own list
    # because it is fatal UNCONDITIONALLY -- exactly the treatment this batch
    # already gave SELFSTALL ("a viewer that stopped running for over 1.5s is a
    # fact about the run"). The same sentence applies verbatim to "the viewer
    # left 136 seconds before the host".
    #
    # WHY IT HAD TO CHANGE: G1 removed the CSS-SPEC false red that used to be
    # the ACCIDENTAL detector for this shape (a truncated viewer session scored
    # as `spec=None` DESYNC), on the grounds that the fact is LIVE-EDGE's to
    # report. But `live_edge_fail` only reached the verdict under
    # --assert-spectator-live, and soak_driver.sh does not pass it -- so on the
    # soak driver, the driver whose cycle-4 red this batch dispositions, an
    # early-exiting viewer would have gone from rc=1 to rc=0 PASS. One fact,
    # one place, but that place has to actually be armed.
    early_exit_fatal = []
    for s in specs:
        lv = spectator_liveness(host_p1_log, s["live"],
                                first_host_match=s.get("first_host_match", 0))
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
            lv2 = spectator_liveness(host_p1_log, s["live"],
                                     first_host_match=s.get("first_host_match", 0))
            if lv2["reached"]:
                print(f"[harness] LIVE-EDGE {s['tag']}: first parse read a "
                      f"mid-flush log (spec_max={lv['spec_max']}); re-parse of "
                      f"the settled log shows reached=True "
                      f"(spec_max={lv2['spec_max']}, gap={lv2['gap']}).")
                lv = lv2
        s["live_edge"] = lv
        # H3 (2026-08-18): the AXIS check comes first. `gap` is
        # host_final_frame - spec_max_frame, and those two numbers are only
        # comparable when the two processes ended at roughly the same moment.
        # When the viewer left first they are not, and printing a gap anyway
        # manufactures a "3376 frames behind" reading out of two frames taken
        # 136 seconds apart on different matches. Print WHAT HAPPENED instead.
        ax = viewer_exit_axis(host_p1_log, s["live"])
        s["exit_axis"] = ax
        if ax["early"]:
            print(f"[harness] LIVE-EDGE {s['tag']} ({s['phase']}): VIEWER LEFT "
                  f"FIRST at {_hms(ax['viewer_last'])}, host ran "
                  f"{ax['host_ran_more_s']:.0f}s more -- spec_max_frame "
                  f"({lv['spec_max']}) and the host's FINAL frame "
                  f"({lv['host_final']}) are NOT on a comparable axis, so NO GAP "
                  f"IS COMPUTED [FATAL: spectator_exited_early]")
            print(f"    matches host={lv['host_matches']} spec={lv['spec_matches']}; "
                  f"the viewer's last frame belongs to an EARLIER host match. "
                  f"Check the [harness] MACHINE line above FIRST: an all-processes "
                  f"stall makes this a VOID run, not a spectator failure.")
            early_exit_fatal.append(
                f"{s['tag']} left at {_hms(ax['viewer_last'])}, "
                f"{ax['host_ran_more_s']:.0f}s before the host")
            live_edge_fail = True
            continue
        verdict = "PASS" if lv["reached"] else "FAIL"
        print(f"[harness] LIVE-EDGE {s['tag']} ({s['phase']}): "
              f"host_final_frame={lv['host_final']} spec_max_frame={lv['spec_max']} "
              f"gap={lv['gap']} (tol {lv['tolerance']}) -> "
              f"spectator_reached_live={lv['reached']} [{verdict}]")
        print(f"    matches host={lv['host_matches']} spec={lv['spec_matches']} "
              f"(needed >= {lv['expected_segs']}, from first observable host "
              f"match {lv['first_host_match']}) followed_all={lv['followed_all']}; "
              f"SPEC-UDP admitted_max={lv['spec_udp_admitted']} "
              f"flatlined={lv['admitted_flatlined']}; stall_frame={lv['stall_frame']}")
        if not lv["reached"]:
            live_edge_fail = True

    # ---- CATCHUP metric (boot -> first played frame) --------------------------
    # Always printed; gated only under --assert-spectator-catchup. The rollout's
    # phase 2/3 soak reports this per deep join, so the harness owns it rather
    # than every sweep driver re-deriving it from log timestamps.
    catchup_fail = False
    for s in specs:
        cu = spectator_catchup(s["live"])
        s["catchup"] = cu
        secs = cu["seconds"]
        applied = cu["applied_seconds"]
        extra = "" if applied is None else f" boot->deep-join-APPLIED={applied:.1f}s"
        if secs is None:
            print(f"[harness] CATCHUP {s['tag']} ({s['phase']}): NO played frame "
                  f"observed (no [SPEC-Q] total>0 and no [CINPUT]){extra}")
        else:
            print(f"[harness] CATCHUP {s['tag']} ({s['phase']}): "
                  f"boot->first_played_frame={secs:.1f}s (source={cu['source']}, "
                  f"threshold {args.catchup_secs:.1f}s){extra}")
        if args.assert_spectator_catchup:
            if secs is None:
                catchup_fail = True
            elif cu["source"] == "spec-q" and secs > args.catchup_secs:
                catchup_fail = True
            elif cu["source"] != "spec-q":
                # The [CINPUT] fallback measures boot->first BATTLE frame, a much
                # larger number on the same run -- never fail a run on it.
                print(f"    (not gated: fallback source {cu['source']} measures "
                      "boot->first BATTLE frame, not boot->first played frame)")

    # CSS-phase parity (#66 Phase 1) -- computed HERE, before the liveness early-
    # return, so host==guest CSS determinism (the load-bearing rollback check,
    # spectator-independent) is always reported even when a spectator fails to
    # hold the live edge. Invariants live in the module-level _css_parity_gate.
    css_fail, _css_lines = _css_parity_gate(OUT_DIR, specs)
    for _l in _css_lines:
        print(_l)

    # CSS-WINDOW object-pool gate. Same placement rationale as the line above:
    # before every early return, off the preserved logs and the .pty captures
    # only. FATAL as of Wave 2 (see CSS_WIN_FATAL); `csswin_fail` was already
    # wired into the OVERALL verdict while the term was advisory, so flipping
    # the constant was the ONLY change the fix needed here.
    # `multi_match_recipe` is the recipe's INTENT, read off the same argument
    # that sets FM2K_AUTO_TERMINATE_TOTAL. It keys the NOT-APPLICABLE hatch, and
    # it is deliberately something no run-time failure can move (review B4a).
    csswin_fail, _csswin_lines = _css_window_gate(
        OUT_DIR, specs, p1_pty, multi_match_recipe=(args.total_frames > 0))
    for _l in _csswin_lines:
        print(_l)

    # The two ALWAYS-ON hook detectors on the character-select plane, parsed and
    # FATAL. Its own flag on purpose: it is not downgraded by FM2K_CSSWIN_FATAL
    # (see the docstring) and, like every other term that can set the verdict, it
    # must be able to print its OWN name on the OVERALL line.
    csspin_fail, _csspin_lines = _css_pin_gate(OUT_DIR, specs)
    for _l in _csspin_lines:
        print(_l)

    # ---- CORRECTNESS GATES: CINPUT + CHECKSUM + rng/hp trace -----------------
    # Run HERE -- after preservation, before EVERY early return. Until 2026-08
    # they ran at the very end, so any earlier bail (most often
    # --assert-spectator-live, which returned immediately) produced a run with
    # NO correctness verdict in either direction: 4 of 12 runs in the wave-4.1
    # sweep A round returned nothing at all. They read only the preserved live_
    # logs, so running them before the replay phase is also strictly safer (the
    # replay process overwrites the game-dir logs it would otherwise race).
    cin_fail, ck_fail, pvp_fail, teampin_fail = _parity_gates(OUT_DIR, specs)
    real_fail = (cin_fail or ck_fail or pvp_fail or css_fail or csswin_fail
                 or teampin_fail or csspin_fail
                 or any(s["gate"]["checked"] > 0 and not s["ok"] for s in specs))
    checked_any = any(s["gate"]["checked"] > 0 for s in specs)

    def _struct_fail(msg):
        """A STRUCTURAL failure (missing stream, missing coverage, missing
        replay file) reported after the correctness gates above have already
        printed their verdict, so the two are never confused."""
        print(msg)
        # Same rule as the OVERALL line (review B4c): a term that can set the
        # verdict prints its OWN name. A CSS-WIN-only failure is not a desync
        # and must never be reported as one.
        _csswin_only = (csswin_fail or csspin_fail) and not (
            cin_fail or ck_fail or pvp_fail or css_fail or teampin_fail)
        print("[harness] (correctness verdict from the gates above: "
              + ("CHARACTER-SELECT GATE FAILED (CSS-WINDOW and/or the CSSPIN/"
                 "CSSPARK-TRIP detectors; no desync term did)" if _csswin_only else
                 "DESYNC DETECTED" if real_fail else
                 "no desync detected" if checked_any else
                 "INCONCLUSIVE -- no spectator trace frames")
              + " -- the FAIL above is structural, not a desync.)")
        return 1

    if args.assert_spectator_live and live_edge_fail:
        print("[harness] --assert-spectator-live: FAILED -- a spectator did NOT "
              "reach/hold the host's live edge (fell behind / stalled -- see "
              "LIVE-EDGE above). This is a spectator liveness failure, NOT an "
              "engine desync and NOT a match-completion failure. Deferred to the "
              "OVERALL verdict; the run CONTINUES so the parity gates still "
              "produce a correctness verdict.")
    if args.assert_spectator_catchup and catchup_fail:
        print(f"[harness] --assert-spectator-catchup: FAILED -- a spectator took "
              f"longer than {args.catchup_secs:.1f}s from boot to its first played "
              "frame (or never played one). Deferred to the OVERALL verdict.")

    if not p1_pty.exists():
        return _struct_fail("[harness] FAIL: host parity missing")
    if not spec_pty.exists():
        return _struct_fail(
            "[harness] FAIL: spectator parity missing -- spectator never "
            f"joined or never captured (check .spec_selftest/spec0.log and "
            f"logs/{specs[0]['log']})")

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
        return _struct_fail(
            f"[harness] FAIL: spectator covered only {spec_n} frames "
            f"(< required {min_coverage}) -- stream stalled or join failed")

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
            return _struct_fail(
                "[harness] FAIL: no match-1 .fm2krep (neither a no-suffix "
                "canonical nor a fresh *_p{0,1}_harness) -- did match 1 "
                "actually complete?")
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
        # Only assert the follow when the HOST's battle 2 visibly ran long
        # enough for a delay-banked spectator to reach it before shutdown.
        # (Observed FP: host created battle session 2 right at the
        # --total-frames budget edge; the spec, a bank behind live, never
        # got there -- that's a harness margin problem, not a follow bug.)
        hd = p1_pty.read_bytes()[32:]
        h_segs, h_in = [], False
        for k in range(len(hd) // 260):
            ph  = _st.unpack_from('<i', hd, k * 260 + 16)[0]
            p1s = _st.unpack_from('<i', hd, k * 260 + 32)[0]
            b = (ph == 3000 and p1s != -1)
            if b and not h_in:
                h_segs.append(0)
            if b:
                h_segs[-1] += 1
            h_in = b
        host_seg2 = h_segs[1] if len(h_segs) >= 2 else 0
        if segs < 2:
            if host_seg2 < 600:
                print(f"[harness] ADVISORY: host battle 2 only ran "
                      f"{host_seg2} observable frames (budget edge) -- "
                      "skipping the spectator follow assert")
            else:
                print("[harness] FAIL: spectator did not follow into match 2 "
                      f"(host battle 2 ran {host_seg2} frames)")
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
            # Terminal-seam exemption: the harness TerminateProcess at
            # --total-frames can land moments after the host ENTERS its
            # final battle -- the pty registers that segment, but the feed
            # dies before the viewer can cross the boundary (observed
            # 2026-07-19: two 30-min runs, viewer applied PIN at all 24/25
            # crossings it was FED, outlived the host by the full
            # host-gone window, then failed this count on the host's
            # 1-sliver final segment). A crossing the host died inside is
            # not the viewer's miss.
            if h_segs and h_segs[-1] < 600:
                needed = max(0, needed - 1)
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
                if host_seg2 < 600:
                    print(f"[harness] ADVISORY: gate spans {gate_segs} segment(s) "
                          f"but host battle 2 only ran {host_seg2} observable "
                          "frames (budget edge) -- skipping the coverage assert")
                else:
                    print("[harness] FAIL: authoritative gate saw trace from only "
                          f"{gate_segs} battle segment(s) -- match 2 was never gated "
                          f"(vacuous multi-match pass; host battle 2 ran {host_seg2})")
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
            return _struct_fail(
                "[harness] FAIL: no p0 harness .fm2krep for the replay gate")
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

    # (The CSS-FP gate and the CINPUT/CHECKSUM/rng-hp gates BOTH ran earlier --
    #  see the _css_parity_gate + _parity_gates calls before the first early
    #  return. real_fail / checked_any were computed there.)

    # Host-no-hiccup report (host ran with FM2K_PERF_PROFILE on).
    if measure_host:
        report_host_pacing(OUT_DIR / "live_FM2K_P1_Debug.log", 0)

    # LIVENESS gates, folded in HERE rather than returning early: they are not
    # correctness verdicts and must never suppress one (see _parity_gates).
    liveness_fail = []
    # Unconditional (not behind an --assert flag): a viewer that stopped running
    # for over 1.5s is a fact about the run, not an optional assertion.
    if _selfstall_fatal:
        liveness_fail.append("viewer SELF-STALL (" + "; ".join(_selfstall_fatal)
                             + ") -- the viewer process was descheduled "
                             "mid-stream; see the SELFSTALL line above")
    # Unconditional for the same reason (H4): a viewer that left the session
    # long before the host did not watch the run this harness just scored.
    if early_exit_fatal:
        liveness_fail.append("spectator EXITED EARLY (" +
                             "; ".join(early_exit_fatal) +
                             ") -- the viewer stopped before the host, so no "
                             "live-edge gap is computable and every per-viewer "
                             "term below covers a shorter run than the host's; "
                             "see the LIVE-EDGE line above")
    if args.assert_spectator_live and live_edge_fail:
        liveness_fail.append("--assert-spectator-live (a spectator did not "
                             "reach/hold the host's live edge)")
    if args.assert_spectator_catchup and catchup_fail:
        liveness_fail.append(f"--assert-spectator-catchup (> {args.catchup_secs:.1f}s "
                             "from boot to first played frame)")

    if not args.keep and not real_fail and not liveness_fail and checked_any:
        cleanup = [p1_pty, replay_pty, OUT_DIR / "p1.log", OUT_DIR / "p2.log",
                   OUT_DIR / "replay.log"]
        cleanup += [s["pty"] for s in specs]
        cleanup += [OUT_DIR / f"spec{s['k']}.log" for s in specs]
        for f in cleanup:
            f.unlink(missing_ok=True)

    if real_fail:
        # The CSS-WIN branch is NOT decoration (review B4c): without it a run
        # that failed ONLY on the character-select window gate printed
        # "a spectator desynced from host -- rng/hp gate", which named a term
        # that had not failed and a plane that may not be involved at all. That
        # line has already had to be annotated as "must NOT be read as a result"
        # in one report. A term that can set the verdict prints its own name.
        why = ("CSSPIN orphaned character-select preview / CSSPARK-TRIP parked "
               "script VMs at battle frame 0 (see above)" if csspin_fail else
               "PVP player-vs-player object-pool divergence (see above)" if pvp_fail else
               "CHECKSUM full-state / POOL topology desync (see above)" if ck_fail else
               "CINPUT input-frame desync (see above)" if cin_fail else
               "CSS-FP cursor/selection desync (see above)" if css_fail else
               "TEAM-PIN: the session did not run VS 1v1, or the mode stamp was "
               "missing so the pin could not be checked (see above)" if teampin_fail else
               "CSS-WIN character-select window gate (falling object, or the "
               "term could not be computed -- see above)" if csswin_fail else
               "rng/hp gate")
        # The head names the PLANE. A PVP failure is the two PLAYERS disagreeing
        # with each other; saying "a spectator desynced from host" there would be
        # the exact mislabel review B4c fixed twice already.
        head = ("the CHARACTER-SELECT PIN detectors failed"
                if csspin_fail and not (ck_fail or cin_fail or css_fail
                                        or pvp_fail or teampin_fail)
                else "THE TWO PLAYERS DESYNCED FROM EACH OTHER" if pvp_fail else
                "the VS-1v1 MODE PIN failed"
                if teampin_fail and not (ck_fail or cin_fail or css_fail or csswin_fail)
                else "the CSS-WINDOW gate failed"
                if csswin_fail and not (ck_fail or cin_fail or css_fail)
                else "a spectator desynced from host")
        print(f"[harness] OVERALL FAIL: {head} -- {why}.")
        if liveness_fail:
            print(f"[harness]   (also failed: {'; '.join(liveness_fail)})")
        return 1
    if liveness_fail:
        verdict = ("no desync detected" if checked_any else
                   "INCONCLUSIVE -- no spectator trace frames")
        print(f"[harness] OVERALL FAIL: {'; '.join(liveness_fail)}. This is a "
              "spectator LIVENESS failure, NOT an engine desync and NOT a "
              f"match-completion failure -- the correctness gates ran and said: "
              f"{verdict}.")
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
