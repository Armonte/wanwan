#!/usr/bin/env python3
"""seam_ring_check.py -- offline bit-exact-resim verifier for FM2K_P*_seamring.csv.

THE CRITERION
-------------
GekkoNet's health check only compares ONE frame per tick (current - pw - 1), so
"no DESYNC line" is a weak, sampled statement. The seam ring records EVERY
SaveState_Save in append order -- the forward save of a frame and every resim
re-save of that same frame -- which supports a much stronger, comparator-
independent test:

    for every frame recorded more than once, the LAST save of that frame must
    be bit-identical to the FIRST save of that frame.

That is the rollback contract itself: a resim of a confirmed frame must
reproduce the forward pass exactly. It is what would have caught the 967f89f
match-end-seam desync at frame 2530 of run 1 instead of at the session kill,
and (per the Phase 2b review) it is also the check that catches a re-frozen RNG
even when no comparator sample happens to land on a divergent frame.

WHY GROUPING IS BY (kind, match, seg, frame)
--------------------------------------------
g_netplay_frame RESTARTS at every battle, so frame 2530 of match 1 and frame
2530 of match 2 are different frames. Rows carry their match index and, in the
seam-window buffer, the arming segment. Comparing across either would produce
pure nonsense.

  SV rows = the primary per-match ring (reset at Netplay_StartBattle).
  WN rows = the seam-window buffer, which survives that reset. On a GREEN
            multi-match run the WN rows are the only ones that still contain
            the earlier matches' completed seams.

Usage:
    python3 tools/seam_ring_check.py FM2K_P1_seamring.csv [FM2K_P2_seamring.csv ...]

Exit 0 = every multiply-recorded frame reproduced. Exit 1 = at least one
violation (a per-frame diff table is printed). Exit 2 = usage / parse problem.
"""

import sys
import os
from collections import OrderedDict

# Column order emitted by SeamTrace_Dump for SV/WN rows.
FIELDS = ["kind", "match", "seg", "frame", "replay", "rb", "fingerprint",
          "rng", "p1_hp", "p2_hp", "round_timer", "game_timer", "buf_idx",
          "p1_input", "p2_input", "vm_hash", "vm_total", "vm_live",
          "game_mode"]

# The state terms compared. `fingerprint` is what gekko actually hashes; the
# rest are its inputs, carried so a failure names the field instead of just a
# hash. vm_live is included because the whole 967f89f mechanism was "the resim
# ran with the VMs parked" -- a bare fingerprint diff would not say that.
COMPARE = ["fingerprint", "rng", "p1_hp", "p2_hp", "round_timer",
           "game_timer", "buf_idx", "p1_input", "p2_input", "vm_live"]


def parse(path):
    """Return (rows, header_comment). Rows are dicts for SV/WN kinds only."""
    rows = []
    header = ""
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            if line.startswith("#"):
                if not header and "seamring" in line:
                    header = line.lstrip("# ").strip()
                continue
            parts = line.split(",")
            kind = parts[0]
            if kind not in ("SV", "WN"):
                continue          # EP episode rows and both column headers
            if len(parts) != len(FIELDS):
                raise ValueError(
                    "%s: %s row has %d fields, expected %d -- CSV produced by a "
                    "different hook build?" % (path, kind, len(parts), len(FIELDS)))
            rows.append(dict(zip(FIELDS, parts)))
    return rows, header


# Battle frame 0 is excluded from the criterion, and the reason is specific:
# it is the one frame label that does NOT identify a single sim state. At a
# fresh battle session the save slot still holds the PREVIOUS match's CRC
# snapshot, so SaveState_Save's `is_replay_save` test
# (state->frame_number == frame && saved_region_crcs.valid) misfires on the
# first saves of the new session, and the pre-init state (hp 780/780,
# buf_idx 0, vm_live 0) and the post-init state (hp 0/0, buf_idx 1,
# vm_live 73) both get recorded under frame 0. Observed on BOTH peers with
# byte-identical values -- i.e. deterministic and cross-peer identical, which
# is the opposite of a rollback violation. Every other frame label is a real
# key and is checked.
SKIP_FRAMES = {"0"}


def check_file(path, verbose=False):
    rows, header = parse(path)
    groups = OrderedDict()
    skipped = 0
    for i, r in enumerate(rows):
        if r["frame"] in SKIP_FRAMES:
            skipped += 1
            continue
        key = (r["kind"], r["match"], r["seg"], r["frame"])
        groups.setdefault(key, []).append((i, r))

    multi = [(k, v) for k, v in groups.items() if len(v) > 1]
    violations = []
    for key, entries in multi:
        first = entries[0][1]
        last = entries[-1][1]
        diff = [f for f in COMPARE if first[f] != last[f]]
        if diff:
            violations.append((key, entries, diff))

    print("== %s" % path)
    if header:
        print("   %s" % header)
    print("   rows=%d skipped_frame0=%d groups=%d multi-recorded=%d violations=%d"
          % (len(rows), skipped, len(groups), len(multi), len(violations)))
    if not multi:
        # Loud, because it is the failure mode the seam-window buffer exists to
        # prevent: a file with no resimmed frame proves nothing at all.
        print("   WARNING: no frame was recorded more than once -- this file "
              "cannot support the criterion (no resim observed). Check that "
              "FM2K_SEAM_TRACE=1 was set and that the run crossed a seam.")

    for key, entries, diff in violations:
        kind, match, seg, frame = key
        print("   VIOLATION %s match=%s seg=%s frame=%s: %s"
              % (kind, match, seg, frame, ",".join(diff)))
        print("      %-4s %-6s %-4s %s"
              % ("idx", "replay", "rb", " ".join("%-12s" % f for f in COMPARE)))
        for idx, r in entries:
            print("      %-4d %-6s %-4s %s"
                  % (idx, r["replay"], r["rb"],
                     " ".join("%-12s" % r[f] for f in COMPARE)))
    if verbose and not violations and multi:
        print("   OK: %d multiply-recorded frames all reproduced" % len(multi))
    return len(violations), len(multi)


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("-")]
    verbose = "-v" in argv or "--verbose" in argv
    if not args:
        print(__doc__)
        return 2
    total_v = 0
    total_m = 0
    missing = 0
    for path in args:
        if not os.path.exists(path):
            print("== %s: MISSING" % path)
            missing += 1
            continue
        try:
            v, m = check_file(path, verbose)
        except ValueError as exc:
            print("== parse error: %s" % exc)
            return 2
        total_v += v
        total_m += m
    print("SEAM-RING-CHECK: files=%d missing=%d multi_recorded=%d violations=%d -- %s"
          % (len(args), missing, total_m, total_v,
             "FAIL" if total_v else "PASS"))
    return 1 if total_v else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
