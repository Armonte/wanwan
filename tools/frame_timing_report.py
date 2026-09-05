#!/usr/bin/env python3
"""frame_timing_report.py -- turn one or two FM2K_P*_frametiming.csv files into
the percentile table that answers ONE question: was this box slow, or was it
the connection?

WHY THIS TOOL EXISTS. Field logs prove the achieved rate sags to 85-96 fps
against a 10 ms budget and that BOTH peers sag together. That single fact is
consistent with two causes wanting opposite fixes, and nothing we had could
tell them apart:

  * THE BOX -- the frame's own work (sim + render) does not fit in 10 ms.
    SleepToTarget can only pad UP TO the target, never claw back time already
    spent, so the loop free-runs at whatever the machine manages and `sleep`
    goes to zero.
  * THE CONNECTION -- the work fits easily, but gekko emits no Advance (the
    frame is SKIPPED: a stall), or the drift brake is lengthening the frame on
    purpose. Braking ONE peer slows BOTH, because the other can only advance as
    fast as inputs arrive. That is why both peers always sag together, and why
    "both peers sagged" was never evidence about whose box was at fault.

The split is trivial once the frame is broken up, which is the whole point of
the instrument (FM2KHook/src/core/frame_timing.cpp, FM2K_FRAME_TIMING=1).

PERCENTILES, NOT AVERAGES. A mean frame time hides exactly the events players
complain about: 297 good frames and 3 catastrophic ones average to "fine". So
this reports p50/p99/p100 and the count over budget, and it reports STALLS
separately with their RUN LENGTHS, because a stall is a DROPPED frame and five
in a row is a 50 ms freeze that no percentile of frame time will show.

TWO FILES = A CROSS-PEER READ. Give it both peers' CSVs and it says whether the
sag is symmetric (both boxes slow, or one shared network cause) or one-sided
(that peer's machine). A one-sided work profile with symmetric stalls is the
signature of one slow box dragging the session, which is the case the [BEAT]
line could never distinguish.

Usage:
  python3 tools/frame_timing_report.py FM2K_P1_frametiming.csv
  python3 tools/frame_timing_report.py P1.csv P2.csv
  python3 tools/frame_timing_report.py P1.csv --phase bat
  python3 tools/frame_timing_report.py P1.csv --match 2 --worst 20
"""

from __future__ import annotations

import argparse
import sys
from collections import defaultdict
from pathlib import Path

# The pacing target every phase is judged against. Read from the CSV header
# when present (budget_us=), so a future retarget cannot leave this stale.
DEFAULT_BUDGET_US = 10000

# When a WALL-CLOCK frame counts as LATE. Deliberately not the budget:
# SleepToTarget busy-waits to the first QPC tick PAST its deadline, so a
# healthy 100 fps run sits a microsecond or two over 10 ms on nearly every
# frame and a strict test reports 100% late on a run with nothing wrong with
# it. 10.5 ms is 95 fps -- large enough to be a frame rate rather than a
# rounding artifact, and inside the 85-96 fps band the field reports arrive in.
# `work` keeps the strict budget test: work over budget is over budget.
LATE_MARGIN = 1.05

INT_COLS = {
    "seq", "match", "bf", "frame_us", "work_us", "sim_us", "render_us",
    "sleep_us", "sim_steps", "adv", "rb_count", "rb_depth", "brake_us",
}
FLOAT_COLS = {"fa"}

# Only netplay-battle rows can stall: gekko is the thing that declines to
# advance, and it is not in the loop on any other phase. Counting an offline
# or menu frame's always-true `adv` alongside them would dilute the one rate
# that matters.
STALLABLE = {"bat"}


def load(path: Path) -> tuple[list[dict], dict]:
    rows: list[dict] = []
    meta: dict = {}
    cols: list[str] | None = None
    with path.open(encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\r\n")
            if not line:
                continue
            if line.startswith("#"):
                for tok in line[1:].split():
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        meta.setdefault(k, v)
                continue
            parts = line.split(",")
            if cols is None:
                cols = parts
                continue
            if len(parts) != len(cols):
                continue
            r: dict = {}
            for k, v in zip(cols, parts):
                if k in INT_COLS:
                    try:
                        r[k] = int(v, 0)
                    except ValueError:
                        r[k] = 0
                elif k in FLOAT_COLS:
                    try:
                        r[k] = float(v)
                    except ValueError:
                        r[k] = 0.0
                else:
                    r[k] = v
            rows.append(r)
    if cols is None:
        raise SystemExit(f"{path}: no CSV header -- is this a frametiming file?")
    return rows, meta


def pct(values: list[int], q: float) -> int:
    """Nearest-rank percentile. Deliberately NOT interpolated: every value here
    is a real measured frame, and the honest answer to "what was the 99th
    percentile frame" is one of the frames that happened, not a number between
    two of them."""
    if not values:
        return 0
    s = sorted(values)
    i = int(round(q * (len(s) - 1)))
    return s[i]


def col(rows: list[dict], name: str) -> list[int]:
    return [r[name] for r in rows if name in r]


def table(rows: list[dict], budget_us: int) -> None:
    """The percentile table. `frame` skips zeros because the last row of a
    session has no successor to close its duration -- that 0 is the ABSENCE of
    a measurement, not a zero-length frame, and leaving it in drags the p50
    down.

    `frame` is tested against LATE_MARGIN * budget and `work` against the
    budget itself; see LATE_MARGIN for why. The threshold used is printed on
    every row so no reader has to guess which test produced a count."""
    late_us = int(budget_us * LATE_MARGIN)
    print(f"    {'metric':<8} {'p50':>8} {'p99':>8} {'p100':>9}  "
          f"{'fps p50/p99':>16}   over")
    for name, skip_zero, thresh in (("frame", True, late_us),
                                    ("work", False, budget_us),
                                    ("sim", False, 0),
                                    ("render", False, 0),
                                    ("sleep", False, 0)):
        vals = [v for v in col(rows, f"{name}_us") if not (skip_zero and v == 0)]
        if not vals:
            continue
        p50, p99, p100 = pct(vals, 0.50), pct(vals, 0.99), pct(vals, 1.00)
        over_s = ""
        if thresh:
            over = sum(1 for v in vals if v > thresh)
            over_s = (f"> {thresh / 1000.0:.1f}ms: {over}/{len(vals)} "
                      f"({100.0 * over / len(vals):.1f}%)")
        # The achieved rate, spelled out on the frame row only: field reports
        # arrive as "85-96 fps" and a microsecond table the reader has to
        # invert in their head is a table nobody compares against them.
        fps_s = (f"{1000000.0 / p50:6.1f} /{1000000.0 / p99:7.1f}"
                 if name == "frame" and p50 and p99 else "")
        print(f"    {name:<8} {p50:>8} {p99:>8} {p100:>9}  {fps_s:>16}   {over_s}")


def stalls(rows: list[dict]) -> dict:
    """Stall count AND run lengths. A stall is a dropped frame; the experience
    of N isolated stalls and one run of N is completely different, and only the
    run length says which happened."""
    n = runs = longest = cur = 0
    hist: dict[int, int] = defaultdict(int)
    for r in rows:
        if r.get("phase") not in STALLABLE:
            if cur:
                hist[cur] += 1
            cur = 0
            continue
        if r.get("adv", 1):
            if cur:
                hist[cur] += 1
            cur = 0
            continue
        n += 1
        if cur == 0:
            runs += 1
        cur += 1
        if cur > longest:
            longest = cur
    if cur:
        hist[cur] += 1
    return {"n": n, "runs": runs, "longest": longest, "hist": dict(hist)}


def summarise(label: str, rows: list[dict], meta: dict, args) -> dict:
    budget = int(meta.get("budget_us", DEFAULT_BUDGET_US))
    print(f"\n================ {label} ================")
    print(f"  rows={len(rows)} dropped={meta.get('dropped', '?')} "
          f"matches={meta.get('matches', '?')} reason={meta.get('reason', '?')}")
    if meta.get("dropped", "0") not in ("0", "?"):
        print(f"  !! {meta['dropped']} frames were DROPPED at the ring cap -- "
              f"this file is TRUNCATED and the percentiles describe only the "
              f"first {len(rows)} frames.")

    by_phase: dict[str, list[dict]] = defaultdict(list)
    for r in rows:
        by_phase[r.get("phase", "?")].append(r)
    print("  phases: " + ", ".join(f"{k}={len(v)}" for k, v in
                                   sorted(by_phase.items(),
                                          key=lambda kv: -len(kv[1]))))

    if not rows:
        return {}

    print("\n  --- ALL ROWS ---")
    table(rows, budget)
    # The battle rows are the ones a player is complaining about. Reported
    # separately because a session's menu and CSS frames are cheap and would
    # otherwise pull every percentile toward "fine".
    bat = by_phase.get("bat", []) + by_phase.get("off", []) + by_phase.get("str", [])
    if bat and len(bat) != len(rows):
        print("\n  --- BATTLE ROWS ONLY (bat+off+str) ---")
        table(bat, budget)

    st = stalls(rows)
    net_rows = len(by_phase.get("bat", []))
    print(f"\n  STALLS: {st['n']} dropped frames over {net_rows} netplay-battle "
          f"rows ({100.0 * st['n'] / net_rows if net_rows else 0.0:.2f}%) in "
          f"{st['runs']} runs, longest {st['longest']} frames "
          f"(= {st['longest'] * budget // 1000} ms of frozen game)")
    if st["hist"]:
        top = sorted(st["hist"].items(), key=lambda kv: -kv[0])[:6]
        print("  stall run lengths: " +
              ", ".join(f"{k}f x{v}" for k, v in top))

    rb_frames = [r for r in rows if r.get("rb_count", 0)]
    rb_total = sum(r["rb_count"] for r in rb_frames)
    max_depth = max((r.get("rb_depth", 0) for r in rows), default=0)
    braked = [r for r in rows if r.get("brake_us", 0)]
    brake_sum = sum(r["brake_us"] for r in braked)
    fa_max = max((r.get("fa", 0.0) for r in rows), default=0.0)
    zero_sleep = sum(1 for r in rows if r.get("sleep_us", 1) == 0)
    print(f"  rollbacks: {rb_total} over {len(rb_frames)} frames, worst rewind "
          f"{max_depth}")
    print(f"  brake: {len(braked)}/{len(rows)} frames "
          f"({100.0 * len(braked) / len(rows):.1f}%), {brake_sum / 1000.0:.1f} ms "
          f"total, worst {max((r['brake_us'] for r in braked), default=0)} us | "
          f"FAmax={fa_max:+.1f}")
    print(f"  sleep==0 on {zero_sleep}/{len(rows)} frames "
          f"({100.0 * zero_sleep / len(rows):.1f}%) -- the pacer had nothing "
          f"left to pad, i.e. the frame was already spent")

    if args.worst:
        worst = sorted(rows, key=lambda r: -r.get("frame_us", 0))[:args.worst]
        print(f"\n  WORST {len(worst)} FRAMES by wall clock:")
        print(f"    {'seq':>8} {'ph':>4} {'bf':>7} {'frame':>7} {'work':>7} "
              f"{'sim':>7} {'rend':>7} {'sleep':>7} {'st':>3} {'adv':>3} "
              f"{'rb':>3} {'dep':>4} {'brake':>6}")
        for r in worst:
            print(f"    {r['seq']:>8} {r.get('phase', '?'):>4} {r.get('bf', 0):>7} "
                  f"{r.get('frame_us', 0):>7} {r.get('work_us', 0):>7} "
                  f"{r.get('sim_us', 0):>7} {r.get('render_us', 0):>7} "
                  f"{r.get('sleep_us', 0):>7} {r.get('sim_steps', 0):>3} "
                  f"{r.get('adv', 1):>3} {r.get('rb_count', 0):>3} "
                  f"{r.get('rb_depth', 0):>4} {r.get('brake_us', 0):>6}")

    # ---- the verdict ------------------------------------------------------
    # Deliberately the SAME rule the hook applies in FrameTiming_Dump, so a
    # live F7 read and this report can never disagree about what a run said.
    work = col(rows, "work_us")
    work_over_pct = 100.0 * sum(1 for v in work if v > budget) / len(work)
    stall_pct = 100.0 * st["n"] / net_rows if net_rows else 0.0
    brake_pct = 100.0 * len(braked) / len(rows)
    if work_over_pct >= 5.0:
        verdict = "THE BOX"
        why = ("sim+render misses the budget on its own, before any network "
               "effect -- the pacer cannot pad what is already spent")
    elif stall_pct >= 0.5 or brake_pct >= 20.0:
        verdict = "THE CONNECTION"
        why = ("the frame's own work fits; the loss is stalled frames and/or "
               "the drift brake")
    else:
        verdict = "NEITHER"
        why = ("the loop met its budget in this window; if it felt sluggish, "
               "the cause is not in these rows")
    print(f"\n  VERDICT: {verdict} -- {why}")
    print(f"           (work over budget {work_over_pct:.1f}%, stalls "
          f"{stall_pct:.2f}%, braked {brake_pct:.1f}%)")

    return {
        "label": label,
        "verdict": verdict,
        "work_over_pct": work_over_pct,
        "stall_pct": stall_pct,
        "brake_pct": brake_pct,
        "work_p99": pct(work, 0.99),
        "frame_p50": pct([v for v in col(rows, "frame_us") if v], 0.50),
        "sim_p50": pct(col(rows, "sim_us"), 0.50),
        "render_p50": pct(col(rows, "render_us"), 0.50),
        "sleep_p50": pct(col(rows, "sleep_us"), 0.50),
        "rows": len(rows),
    }


def cross_peer(a: dict, b: dict) -> None:
    """Two peers, one session. The asymmetry is the finding: a shared network
    cause hits both sides; a slow machine does not."""
    print("\n================ CROSS-PEER ================")
    if not a or not b:
        print("  (one side has no rows -- nothing to compare)")
        return
    print(f"  {'':<14} {a['label']:>16} {b['label']:>16}")
    for k, name in (("frame_p50", "frame p50 us"), ("work_p99", "work p99 us"),
                    ("sim_p50", "sim p50 us"), ("render_p50", "render p50 us"),
                    ("sleep_p50", "sleep p50 us"),
                    ("work_over_pct", "work over %"),
                    ("stall_pct", "stall %"), ("brake_pct", "braked %")):
        print(f"  {name:<14} {a[k]:>16} {b[k]:>16}"
              if isinstance(a[k], int) else
              f"  {name:<14} {a[k]:>16.2f} {b[k]:>16.2f}")

    # A work profile that differs by more than half a millisecond at p99 is one
    # machine doing measurably more work per frame than the other on the same
    # content -- which is the one thing a symmetric network cause cannot
    # produce.
    dwork = abs(a["work_p99"] - b["work_p99"])
    dstall = abs(a["stall_pct"] - b["stall_pct"])
    dbrake = abs(a["brake_pct"] - b["brake_pct"])
    print()
    # The brake is asymmetric BY DESIGN -- only the peer that reads itself
    # ahead lengthens its frame -- so a lopsided brake rate is not a defect,
    # it names which peer was persistently ahead. Reported before the verdicts
    # below because a summary that called this "symmetric" would be false.
    if dbrake > 5.0:
        ahead = a["label"] if a["brake_pct"] > b["brake_pct"] else b["label"]
        other = b["label"] if ahead == a["label"] else a["label"]
        print(f"  BRAKE IS ONE-SIDED: {ahead} braked on "
              f"{max(a['brake_pct'], b['brake_pct']):.1f}% of frames against "
              f"{min(a['brake_pct'], b['brake_pct']):.1f}% on {other}. Only the "
              f"peer that reads itself AHEAD brakes, so {ahead} was running in "
              f"front for most of this session -- and because braking one peer "
              f"slows both, {other} inherited the reduced rate with none of the "
              f"cause visible on its own side.")
    if a["verdict"] == b["verdict"] == "THE CONNECTION":
        print("  BOTH peers read THE CONNECTION and their work profiles are "
              "comparable -- this is a network-bound session, and no amount of "
              "work on either machine will change it.")
    elif dwork > 500 and (a["work_over_pct"] > 5.0) != (b["work_over_pct"] > 5.0):
        slow = a["label"] if a["work_p99"] > b["work_p99"] else b["label"]
        print(f"  ONE-SIDED: {slow} spends {dwork} us more per frame at p99 "
              f"and is the only side missing the budget. That peer's MACHINE "
              f"is dragging the session -- and because braking one peer slows "
              f"both, the other side sees the same sag with none of the cause.")
    elif dstall > 1.0:
        worse = a["label"] if a["stall_pct"] > b["stall_pct"] else b["label"]
        print(f"  ASYMMETRIC STALLS: {worse} stalls {dstall:.2f} points more "
              f"often. Stalls are gekko running out of prediction, so an "
              f"asymmetry here is an asymmetric input PATH (one direction "
              f"losing or delaying), not a slow box.")
    elif dbrake > 5.0:
        print("  Work and stalls are otherwise comparable: the asymmetry in "
              "this session is the frame ADVANTAGE above, not either machine.")
    else:
        print("  SYMMETRIC: the two peers look alike on work, stalls and "
              "brake. Whatever this session did, it did to both sides equally.")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Percentile report over FM2K per-frame timing CSVs.")
    ap.add_argument("csv", nargs="+", type=Path,
                    help="one CSV (single-peer report) or two (cross-peer)")
    ap.add_argument("--phase", default="",
                    help="restrict to one phase code: bat, off, str, css, nat, "
                         "spec. Default: every row, with battle rows also "
                         "reported separately.")
    ap.add_argument("--match", type=int, default=-1,
                    help="restrict to one match index (0-based, as recorded)")
    ap.add_argument("--worst", type=int, default=10,
                    help="list the N worst frames by wall clock (0 to skip)")
    args = ap.parse_args()

    if len(args.csv) > 2:
        print("At most two CSVs (one per peer).", file=sys.stderr)
        return 2

    summaries = []
    for path in args.csv:
        if not path.exists():
            print(f"FATAL: {path} does not exist. An armed run writes "
                  f"FM2K_P<N>_frametiming.csv into the GAME directory; if it "
                  f"is missing, FM2K_FRAME_TIMING=1 never reached that "
                  f"process, or the run never reached a dump path (match end, "
                  f"WM_QUIT, harness auto-terminate, or F7).", file=sys.stderr)
            return 2
        rows, meta = load(path)
        if args.phase:
            rows = [r for r in rows if r.get("phase") == args.phase]
        if args.match >= 0:
            rows = [r for r in rows if r.get("match") == args.match]
        summaries.append(summarise(path.name, rows, meta, args))

    if len(summaries) == 2:
        cross_peer(summaries[0], summaries[1])
    return 0


if __name__ == "__main__":
    sys.exit(main())
