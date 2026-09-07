#!/usr/bin/env python3
"""Diff two peers' script random-branch draw traces.

Every gameplay game_rand draw in a netplay match comes from one site: the
script VM's random-branch opcode at 0x4139A3 (measured, see
docs/dev/desync_campaign_state.md). scriptrand_trace records one row per draw:
{frame, object slot, entity kind, script_id, script cursor}.

THE ROLLBACK PROBLEM. A peer re-simulates a frame many times (the host of the
2026-09-06 capture saved 2471 times across 229 confirmed frames), and every
re-simulation re-emits that frame's draws. So the two files have wildly
different row counts and cannot be compared row-for-row, and any CUMULATIVE
statistic over them -- a running hash included -- differs between peers for
entirely benign reasons.

What IS comparable: for each frame, the LAST contiguous block of rows carrying
that frame number, which is that peer's final (confirmed) simulation of it.
Draws within one simulation of a frame are contiguous by construction, since
they happen while that frame is being simulated. This compares those blocks.
"""

import argparse
import csv
import sys


def load_blocks(path):
    """{frame: [rows...]} keeping only the LAST contiguous block per frame."""
    rows = []
    with open(path) as fh:
        body = [l for l in fh if not l.startswith("#")]
    for r in csv.DictReader(body):
        # `value` is included deliberately: two peers that entered a frame
        # with the same seed roll the same numbers, so a matching context with
        # a DIFFERENT value would mean the seed had already diverged, and a
        # matching value with a different branch outcome means the difference
        # is in the script operands rather than the randomness.
        rows.append((int(r["frame"]), r["slot"], r["kind"],
                     r.get("player", "?"), r["script_id"], r["cursor"],
                     r.get("value", "?")))

    blocks, cur_frame, cur = {}, None, []
    for row in rows:
        f = row[0]
        if f != cur_frame:
            if cur_frame is not None:
                blocks[cur_frame] = cur     # later block replaces earlier
            cur_frame, cur = f, []
        cur.append(row[1:])
    if cur_frame is not None:
        blocks[cur_frame] = cur
    return rows, blocks


def all_blocks(path):
    """[(frame, [rows...])] -- EVERY contiguous block, re-simulations included."""
    rows = []
    with open(path) as fh:
        body = [l for l in fh if not l.startswith("#")]
    for r in csv.DictReader(body):
        rows.append((int(r["frame"]), r["slot"], r["kind"],
                     r.get("player", "?"), r["script_id"], r["cursor"],
                     r.get("value", "?")))
    out, cur_frame, cur = [], None, []
    for row in rows:
        if row[0] != cur_frame:
            if cur_frame is not None:
                out.append((cur_frame, cur))
            cur_frame, cur = row[0], []
        cur.append(row[1:])
    if cur_frame is not None:
        out.append((cur_frame, cur))
    return out


def report_resim(path, context):
    """Compare successive simulations of the SAME frame within ONE file.

    THIS IS THE STRESS-MODE CHECK. FM2K_STRESS_MODE=1 rolls back every
    check_distance frames on a single instance with the SAME inputs, so two
    simulations of one frame must produce identical draws. Any difference is a
    local rollback-determinism bug -- reproducible without the netplay lottery,
    where the live desync shows up in about 1 run in 14.

    Do NOT read this mode's output from a live netplay capture: there a frame is
    legitimately re-simulated because the INPUTS changed, so differing blocks
    are expected and mean nothing.
    """
    blocks = all_blocks(path)
    # DROP THE FIRST BLOCK. The ring keeps only the last N draws, so the file
    # almost always begins PART-WAY THROUGH a frame's draws. Treating that
    # fragment as a complete simulation makes it disagree with every later
    # replay of the same frame -- which is exactly the false "9 frames
    # re-simulated differently, rollback determinism bug" this tool reported
    # before the raw rows were read by hand.
    truncated = blocks[0][0] if blocks else None
    blocks = blocks[1:]
    print(f"== {path}\n   {len(blocks)} simulated frame-blocks "
          f"(dropped the leading fragment of f={truncated}: ring-wrap, not a "
          f"whole frame)")
    seen, bad = {}, []
    for frame, rows in blocks:
        if frame in seen:
            # One entry per FRAME, not per re-simulation: a frame replayed 8
            # times would otherwise print the same disagreement 8 times.
            if seen[frame] != rows and not any(b[0] == frame for b in bad):
                bad.append((frame, seen[frame], rows))
        else:
            seen[frame] = rows
    resimmed = len(blocks) - len(seen)
    print(f"   {len(seen)} distinct frames, {resimmed} re-simulations")
    if resimmed == 0:
        print("   NO re-simulation happened -- this run cannot test rollback "
              "determinism. Is FM2K_STRESS_MODE=1 set, and is check_distance "
              "small enough to force a rollback?")
        return 1
    if not bad:
        print(f"   every re-simulated frame reproduced its draws EXACTLY "
              f"({resimmed} re-sims) -- rollback is deterministic on this path")
        return 0
    print(f"   {len(bad)} frame(s) re-simulated DIFFERENTLY -- rollback "
          f"determinism bug")
    for frame, first, again in bad[:context]:
        print(f"\n   f={frame}: first pass drew {len(first)}, re-sim drew {len(again)}")
        print("     slot,kind,player,script_id,cursor,value")
        for i in range(max(len(first), len(again))):
            a = ",".join(first[i]) if i < len(first) else "--"
            b = ",".join(again[i]) if i < len(again) else "--"
            mark = "" if a == b else "   <-- differs"
            print(f"       [{i}] first={a:28s} resim={b:28s}{mark}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b", nargs="?")
    ap.add_argument("--context", type=int, default=6,
                    help="draws to print around the first disagreement")
    ap.add_argument("--resim", action="store_true",
                    help="single-file stress check: compare successive "
                         "simulations of the same frame (see report_resim)")
    args = ap.parse_args()
    if args.resim:
        rc = 0
        for p in [args.a] + ([args.b] if args.b else []):
            rc |= report_resim(p, args.context)
        return rc

    ra, ba = load_blocks(args.a)
    rb, bb = load_blocks(args.b)
    print(f"A {args.a}\n  {len(ra)} draws, {len(ba)} frames")
    print(f"B {args.b}\n  {len(rb)} draws, {len(bb)} frames")

    common = sorted(set(ba) & set(bb))
    if not common:
        print("!! no frames in common")
        return 1
    print(f"frames in common: {len(common)} ({common[0]}..{common[-1]})")

    first = None
    ndiff = 0
    for f in common:
        if ba[f] != bb[f]:
            ndiff += 1
            if first is None:
                first = f
    if first is None:
        print("\nthe two peers made IDENTICAL script draws on every common "
              "frame -- this term does not explain the divergence")
        return 0

    print(f"\nframes whose final simulation differs: {ndiff} of {len(common)}")
    print(f"FIRST at f={first}")
    A, B = ba[first], bb[first]
    print(f"  A drew {len(A)}, B drew {len(B)}")
    print("  slot,kind,player,script_id,cursor,value")
    for i in range(max(len(A), len(B))):
        sa = ",".join(A[i]) if i < len(A) else "--"
        sb = ",".join(B[i]) if i < len(B) else "--"
        mark = "" if sa == sb else "   <-- differs"
        print(f"    [{i}] A={sa:28s} B={sb:28s}{mark}")

    # Where a frame's draw COUNT differs, the extra draw is the whole desync,
    # so list those separately -- they are the actionable ones.
    cnt = [(f, len(ba[f]), len(bb[f])) for f in common if len(ba[f]) != len(bb[f])]
    print(f"\nframes where the DRAW COUNT differs: {len(cnt)}")
    for f, na, nb in cnt[:20]:
        print(f"    f={f:5d}  A={na:3d}  B={nb:3d}")
    if len(cnt) > 20:
        print(f"    ... and {len(cnt)-20} more")
    return 0


if __name__ == "__main__":
    sys.exit(main())
