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
    for the BOUNDED deep join (FM2K_SPEC_DEEP_JOIN): a viewer that dials in
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

    def sessions(path):
        # per CSS session: (sel-cell nav before both-confirm, locked sel). A
        # session ends at the first act==(1,1) latch; the frozen post-confirm
        # tail is dropped; a reset to (0,0) after a latch opens the next session.
        segs, cur, locked, in_tail, prev = [], [], None, False, None
        try: fh = open(path, errors="ignore")
        except OSError: return segs
        for ln in fh:
            m = _sel.search(ln)
            if not m: continue
            sel, act = m.group(1), (int(m.group(2)), int(m.group(3)))
            key = (sel, act)
            if key == prev: continue
            prev = key
            if act == (1, 1):
                if not in_tail:
                    locked = sel; in_tail = True
            elif act == (0, 0) and in_tail:
                segs.append((cur, locked)); cur, locked, in_tail = [], None, False
                cur.append(sel)
            elif not in_tail:
                cur.append(sel)
        segs.append((cur, locked))
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
    Hs = sessions(out_dir / "live_FM2K_P1_Debug.log")
    for s in specs:
        Ss = sessions(s["live"])
        if not Ss:
            lines.append(f"[harness] CSS-SPEC {s['tag']}: no CSS sessions observed -- skipped")
            continue
        for si, (snav, slk) in enumerate(Ss):
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
            for hi, (hnav, hlk) in enumerate(Hs):
                L = lcs_len(hnav, snav)
                non_embed = min(len(hnav), len(snav)) - L
                pfx = 0
                for a, b in zip(hnav, snav):
                    if a != b: break
                    pfx += 1
                cands.append(((non_embed, -L, -pfx), hi, L, len(hnav), hlk))
            bkey = min(c[0] for c in cands)
            tied = [c for c in cands if c[0] == bkey]
            _, hi, L, hlen, hlk = min(tied, key=lambda c: (abs(c[1] - si), c[1]))
            non_embed = bkey[0]
            ambig = (f" [AMBIGUOUS: {len(tied)} host sessions tie on this "
                     f"{len(snav)}-cell segment]" if len(tied) > 1 else "")
            shorter = min(hlen, len(snav))
            tol = max(2, (shorter + 49) // 50)   # ~2% seam slack (snapshot join)
            lock_ok, nav_ok = (slk == hlk), (non_embed <= tol)
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
    return fail, lines


# The CSS-window gate is FATAL as of Wave 2 (2026-08-15).
#
# It was ADVISORY while the falling-object bug it measures was known-present and
# unfixed -- a fatal term would have reddened every run on every build and
# nobody could have told a regression from the backlog. The fix landed in
# FM2KHook/src/netplay/spec_css_park.cpp (the spectator parks its type-4 script
# VMs across the character-select window, so the fighters stop running their
# battle-entry scripts under gravity while the CSS is up), so the diversion is
# gone: FALL and CSSPOOL failures, and the not-computed-must-fail rule, now set
# the run verdict like every other term.
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
    if hwin and not not_applicable:
        lines.append(f"[harness] CSS-WIN: host has {len(hwin)} character-select "
                     f"window(s)")
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
            for j in range(npair):
                hlo, hhi = hwin[len(hwin) - npair + j]
                lo, hi   = swin[len(swin) - npair + j]
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
                    ceil = max(hl)
                    over = sum(1 for y in sv if y > ceil + TOL)
                    rest = ",".join(f"{y/65536.0:.1f}" for y in sorted(hl))
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
                            f"-> CSS FALLING OBJECT (near spec idx {at})"
                            f"{pnote}" + adv)
                    else:
                        lines.append(
                            f"[harness] CSS-WIN FALL {s['tag']} win{j} ({lo}-{hi}) "
                            f"{k}: host rests at {{{rest}}} px, spectator max "
                            f"{max(sv)/65536.0:.1f} px -> OK{pnote}")
            lines.append(f"[harness] CSS-WIN FALL {s['tag']}: {npair} paired "
                         f"window(s) ({len(swin)} spectator / {len(hwin)} host), "
                         f"{bad} with a falling object"
                         + (" -> FAIL" + adv if bad else " -> PASS"))
        if verdicts == 0:
            _fail("[harness] CSS-WIN FALL: not one window/player pair produced a "
                  "verdict (no host resting level anywhere) -- NOT COMPUTED. A "
                  "term that saw nothing must not read as a pass")

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

    if not CSS_WIN_FATAL:
        lines.append("[harness] CSS-WIN: ADVISORY -- explicitly downgraded by "
                     "FM2K_CSSWIN_FATAL=0, so these terms do not affect the run "
                     "verdict. The default is FATAL as of Wave 2 (the "
                     "character-select falling-object bug is fixed in "
                     "spec_css_park.cpp); unset the variable to gate on them")
    return fail, lines


def _parity_gates(out_dir, specs):
    """The three correctness gates, over the PRESERVED live_ logs only.

    CINPUT (primary, frame-keyed input identity), CHECKSUM (the full-state
    fencepost) and the rng/hp trace GATE. Returns (cin_fail, ck_fail) and sets
    `gate`/`ok` on each spec dict; prints its verdict lines as it goes.

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
            for si, sseg in enumerate(_ck_spec(s["live"])):
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
                if n == 0:
                    ck_fail = True
                    print(f"[harness] CHECKSUM {s['tag']} seg{si}: NO OVERLAP with "
                          f"any host match -> FULL-STATE DESYNC")
                elif mx > 3:
                    ck_fail = True
                    hcrc = _ck_crc(ck_H[hi])
                    print(f"[harness] CHECKSUM {s['tag']} seg{si}: vs host-match{hi} "
                          f"off{O} {mm}/{n} CRC mismatches (longest run {mx}) -> "
                          f"FULL-STATE DESYNC (first spec-bf={fb} host-f={fb + O}: "
                          f"spec=0x{scrc[fb]:08X} host=0x{hcrc[fb + O]:08X}){tie}")
                else:
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
    return cin_fail, ck_fail


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
    for k in ("FM2K_LOCAL_DELAY", "FM2K_PRED_WINDOW", "FM2K_PREDICTION_WINDOW", "FM2K_RUNAHEAD", "FM2K_SPEC_UDP", "FM2K_AUTOPLAY_CSS_DWELL", "FM2K_SPECTATOR_DEBUG", "FM2K_HOST_TRACE", "FM2K_FA_TRACE", "FM2K_TEST_BATTLE_SEED",
              # host-clock sync + rift frame pacing A/B
              "FM2K_HOST_CLOCK",
              # ReliableChannel spectator A/B transport (reliable-ordered+FEC over UDP)
              "FM2K_SPEC_RC", "FM2K_SPEC_RC_SNAPSHOT", "FM2K_RC_FEC", "FM2K_RC_FEC_K",
              # task #55: dead-TCP simulation (spectator dials a dead port);
              # with RC default-on the spectate must fully work regardless.
              "FM2K_TEST_SPEC_TCP_BLACKHOLE",
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
              # Wave 2 spectator CSS-window park kill-switch. Default ON in the
              # hook; forwarded so the causality control run (=0 reproduces the
              # falling objects) is reachable from a WSL-side invocation. Listed
              # on the player side too even though only the spectator plane arms
              # it -- a one-sided env list is how a probe silently measures one
              # plane against nothing.
              "FM2K_SPEC_CSS_PARK",
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
              "FM2K_SPEC_FINGERPRINT"):
        if os.environ.get(k):
            common_env[k] = os.environ[k]
    # FM2K_SPEC_DEEP_JOIN is forwarded SEPARATELY and by presence, not by
    # truthiness. The hook now defaults the bounded deep join ON and this
    # variable is the KILL-SWITCH, so the value that matters most is "0" --
    # which does survive the `if os.environ.get(k)` filter above (the STRING
    # "0" is truthy in Python) but only by accident, and one refactor to
    # `int(...)`/`== "1"` would silently stop forwarding the OFF direction,
    # making every "kill-switch" run measure the default and reach the exact
    # wrong triage conclusion. The empty string is forwarded too: the hook
    # treats empty/whitespace as unset, so it round-trips faithfully.
    # BOTH sides need it (host decides eligibility, viewer obeys the grant) --
    # see the spectator list below for the other half.
    if os.environ.get("FM2K_SPEC_DEEP_JOIN") is not None:
        common_env["FM2K_SPEC_DEEP_JOIN"] = os.environ["FM2K_SPEC_DEEP_JOIN"]
    # FM2K_SEAM_LEGACY_PARK: same presence-not-truthiness rule. It is the
    # DIAGNOSTIC A/B lever that restores the deleted blanket load-site park
    # (i.e. reinstates the match-end-seam desync on purpose), default OFF in
    # the hook. Forwarding BY PRESENCE keeps "0"/"" round-tripping faithfully
    # instead of relying on Python's "0" being truthy.
    if os.environ.get("FM2K_SEAM_LEGACY_PARK") is not None:
        common_env["FM2K_SEAM_LEGACY_PARK"] = os.environ["FM2K_SEAM_LEGACY_PARK"]
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

    p1_env = {**common_env,
              "FM2K_CSS_WIN": _css_win,
              "FM2K_LOCAL_PORT": str(P1_PORT),
              "FM2K_REMOTE_ADDR": f"127.0.0.1:{P2_PORT}",
              "FM2K_PARITY_RECORD_PATH": to_win(p1_pty)}
    if _css_fall_delta:
        p1_env["FM2K_CSS_FALL_DELTA"] = _css_fall_delta
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
               "FM2K_SPEC_HOST_GONE_MS": os.environ.get("FM2K_SPEC_HOST_GONE_MS", "5000"),
               # [CSS-WIN]/[CSS-OBJ] character-select window gate -- the OTHER
               # half of the HOST-side default above (the guest deliberately
               # does not carry it; see the FM2K_CSS_WIN comment there). The
               # whole point of the gate is the host-vs-spectator comparison, so
               # THIS default must stay symmetric with P1's or the run measures
               # one plane against nothing.
               "FM2K_CSS_WIN": _css_win}
        if _css_fall_delta:
            env["FM2K_CSS_FALL_DELTA"] = _css_fall_delta
        for kk in ("FM2K_SPEC_DROP", "FM2K_SPEC_DROP_SEED", "FM2K_CSS_TRACE",
                   "FM2K_SPECTATOR_DEBUG", "FM2K_SPEC_CONNECT_TIMEOUT_MS",
                   "FM2K_NET_DELAY_MS", "FM2K_NET_JITTER_MS", "FM2K_NET_LOSS", "FM2K_NET_SEED",
                   "FM2K_NET_REORDER", "FM2K_NET_DUP",
                   # task #55: the RC transport family MUST match the host's
                   # or the pair runs a split-brain transport config (host
                   # streaming RC while the viewer runs TCP-primary logic
                   # incl. the TCP-fail give-up) -- this exact gap
                   # invalidated a night of A/B runs.
                   "FM2K_SPEC_RC", "FM2K_SPEC_RC_SNAPSHOT", "FM2K_RC_STATS",
                   "FM2K_RC_FEC", "FM2K_RC_FEC_K",
                   "FM2K_TEST_SPEC_TCP_BLACKHOLE",
                   # mid-join spectate desync hunt: per-region full-state fingerprint
                   "FM2K_FULLFP", "FM2K_POOLSET",
                   # Phase 4c match-start pool resync kill-switch (viewer half).
                   "FM2K_SPEC_POOL_SYNC",
                   # Wave 2 CSS-window park kill-switch, viewer half -- this is
                   # the half that actually arms the window.
                   "FM2K_SPEC_CSS_PARK",
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
                   "FM2K_SEAM_TRACE"):
            if os.environ.get(kk):
                env[kk] = os.environ[kk]
        # Bounded deep join -- the viewer only obeys the grant, but it must be
        # able to obey it. Presence-forwarded for the same kill-switch reason as
        # the host side above; "0" must reach BOTH ends or a kill-switch run is
        # a split-brain run.
        if os.environ.get("FM2K_SPEC_DEEP_JOIN") is not None:
            env["FM2K_SPEC_DEEP_JOIN"] = os.environ["FM2K_SPEC_DEEP_JOIN"]
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
                      # Index of the earliest host match this viewer can
                      # possibly observe; filled in at dial-in (schedule_spec)
                      # and consumed by spectator_liveness. 0 = present from the
                      # session start, which is what every full-session joiner
                      # is and what the pre-2026-08 behaviour assumed for all.
                      "first_host_match": 0,
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
        # Liveness baseline (--assert-spectator-live): the earliest host match
        # this viewer can possibly observe. A css[N] joiner dials in AFTER the
        # host's first N-1 battles, so its first observable match index is that
        # battle count; a battle[N] joiner dials into battle N itself, index
        # count-1. For a BOUNDED deep joiner (css[N>1] with
        # FM2K_SPEC_DEEP_JOIN=1) this is exactly the set of matches it is
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

    # ---- CORRECTNESS GATES: CINPUT + CHECKSUM + rng/hp trace -----------------
    # Run HERE -- after preservation, before EVERY early return. Until 2026-08
    # they ran at the very end, so any earlier bail (most often
    # --assert-spectator-live, which returned immediately) produced a run with
    # NO correctness verdict in either direction: 4 of 12 runs in the wave-4.1
    # sweep A round returned nothing at all. They read only the preserved live_
    # logs, so running them before the replay phase is also strictly safer (the
    # replay process overwrites the game-dir logs it would otherwise race).
    cin_fail, ck_fail = _parity_gates(OUT_DIR, specs)
    real_fail = (cin_fail or ck_fail or css_fail or csswin_fail
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
        _csswin_only = csswin_fail and not (cin_fail or ck_fail or css_fail)
        print("[harness] (correctness verdict from the gates above: "
              + ("CSS-WINDOW GATE FAILED (no desync term did)" if _csswin_only else
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

    # Phase 3: host-no-hiccup report (host ran with FM2K_PERF_PROFILE on).
    if measure_host:
        report_host_pacing(OUT_DIR / "live_FM2K_P1_Debug.log", args.fake_spectators)

    # LIVENESS gates, folded in HERE rather than returning early: they are not
    # correctness verdicts and must never suppress one (see _parity_gates).
    liveness_fail = []
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
        why = ("CHECKSUM full-state / POOL topology desync (see above)" if ck_fail else
               "CINPUT input-frame desync (see above)" if cin_fail else
               "CSS-FP cursor/selection desync (see above)" if css_fail else
               "CSS-WIN character-select window gate (falling object, or the "
               "term could not be computed -- see above)" if csswin_fail else
               "rng/hp gate")
        head = ("the CSS-WINDOW gate failed"
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
