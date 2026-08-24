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
          "game_mode", "shake", "fx1", "fx2", "rand_total", "rand_render",
          "h_gs", "h_it", "h_obj", "h_char", "h_ai", "h_lists"]

# DIAGNOSTIC-ONLY columns. shake / fx1 / fx2 are the three regions
# savestate_fm2k_save.cpp deliberately zeroes in the saved copy and skips on
# load, so they free-run across rollback BY DESIGN and differing here is
# expected, not a fault -- they must never enter COMPARE or every rollback
# would red. rand_total is cumulative, so it differs between any two rows by
# construction. They are printed in the violation table because the open
# question for the intermittent violation is whether a group whose only
# compared difference is `rng` ALSO differs in one of these: that is what
# distinguishes "free-running effect state steered the gameplay draw count"
# from "the sim itself diverged".
DIAG = ["shake", "fx1", "fx2", "rand_total", "rand_render",
        "h_gs", "h_it", "h_obj", "h_char", "h_ai", "h_lists"]

# The state terms compared. `fingerprint` is what gekko actually hashes; the
# rest are its inputs, carried so a failure names the field instead of just a
# hash. vm_live is included because the whole 967f89f mechanism was "the resim
# ran with the VMs parked" -- a bare fingerprint diff would not say that.
COMPARE = ["fingerprint", "rng", "p1_hp", "p2_hp", "round_timer",
           "game_timer", "buf_idx", "p1_input", "p2_input", "vm_live"]

# ---------------------------------------------------------------------------
# THE MISPREDICTION BLIND SPOT (Lane A, 2026-08-15) -- why not every difference
# between the first and last save of a frame is a determinism violation.
#
# The criterion above silently assumes the FORWARD save of a frame ran on
# CONFIRMED inputs. Under rollback it often does not: the forward pass saves a
# frame using a PREDICTED remote input, the real input arrives, gekko rolls
# back, and the resim re-saves the same frame with the corrected input. First
# save != last save, entirely correctly -- that is rollback working, not
# breaking. The state that had already been computed is identical; only the
# input word the frame was run with, and the fingerprint that hashes it,
# changed.
#
# The wanwan corpus satisfied the assumption by luck (2190 multiply-recorded
# groups, 0 input-differing) because its harsher profile happened to always
# confirm before the save. vanpri does not: run1 of the Lane A series failed
# 4 groups at battle-entry frames whose ONLY differing fields were `p1_input`
# and the fingerprint that follows it, with rng, both HP, both timers and
# vm_live bit-identical -- and the same recipe produced 0 in runs 2-5. A gate
# term that flakes on a correct rollback is worse than no term.
#
# So: a group is reclassified as INPUT-CORRECTION, reported separately and NOT
# counted as a violation, when ALL of these hold:
#   * every DETERMINISM_CRITICAL field is identical across the group, and
#   * at least one input field differs, and
#   * nothing outside {fingerprint, p1_input, p2_input} differs.
# The fingerprint is allowed to differ ONLY because the inputs it hashes did:
# a fingerprint that moves while both inputs agree stays a VIOLATION, which is
# exactly the 967f89f signature (frozen rng, vm_live 146 -> 0, inputs agreeing).
#
# WHAT THIS TERM NO LONGER COVERS (named, not hidden -- Wave-2 review B5). Within
# ONE peer, first-save-vs-last-save is the only view this ring has, so it cannot
# tell "prediction corrected by the REAL input" from "the resim applied a
# DIFFERENT WRONG input" -- the e5fe11f speculative-input-leak / input-indexing
# class. Before the classifier any such group redded here; it is now advisory.
# The class is still covered from outside: gekko's own fingerprint hashes BOTH
# inputs, and the harness's CINPUT term compares the two peers frame by frame.
# The `input_corrections` count is also unbounded and never fatal at any count --
# an unexpected CLUSTER is meant to be read off the printed list below.
DETERMINISM_CRITICAL = ["rng", "p1_hp", "p2_hp", "round_timer", "game_timer",
                        "buf_idx", "vm_live"]
INPUT_FIELDS = ["p1_input", "p2_input"]


def classify(first, last, diff):
    """'ok' | 'input' (mispredicted forward save) | 'violation'."""
    if not diff:
        return "ok"
    if any(f in diff for f in DETERMINISM_CRITICAL):
        return "violation"
    if not any(f in diff for f in INPUT_FIELDS):
        return "violation"          # fingerprint moved with inputs agreeing
    if set(diff) - set(INPUT_FIELDS) - {"fingerprint"}:
        return "violation"
    return "input"


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
            # Two accepted widths: the current one, and the pre-DIAG width
            # from before shake/fx1/fx2/rand_total were added. Archived
            # evidence from an older hook must stay readable -- the whole
            # point of keeping failing runs is comparing them later. Any
            # OTHER width is still a hard error: silent truncation would let
            # a genuinely mismatched build score as a pass.
            if len(parts) == len(FIELDS) - len(DIAG):
                rows.append(dict(zip(FIELDS, parts)))   # legacy: DIAG absent
                continue
            if len(parts) != len(FIELDS):
                raise ValueError(
                    "%s: %s row has %d fields, expected %d (or the legacy %d) "
                    "-- CSV produced by a different hook build?"
                    % (path, kind, len(parts), len(FIELDS),
                       len(FIELDS) - len(DIAG)))
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
    corrections = []
    for key, entries in multi:
        first = entries[0][1]
        last = entries[-1][1]
        diff = [f for f in COMPARE if first[f] != last[f]]
        verdict = classify(first, last, diff)
        if verdict == "violation":
            violations.append((key, entries, diff))
        elif verdict == "input":
            corrections.append((key, entries, diff))

    print("== %s" % path)
    if header:
        print("   %s" % header)
    print("   rows=%d skipped_frame0=%d groups=%d multi-recorded=%d "
          "input_corrections=%d violations=%d"
          % (len(rows), skipped, len(groups), len(multi), len(corrections),
             len(violations)))
    if corrections:
        # Reported, never fatal. Named individually so an unexpected cluster is
        # visible rather than hidden behind a count.
        print("   INPUT-CORRECTION (mispredicted forward save, corrected by "
              "resim -- NOT a determinism violation):")
        for key, entries, diff in corrections[:8]:
            kind, match, seg, frame = key
            print("      %s match=%s seg=%s frame=%s: %s (%d saves)"
                  % (kind, match, seg, frame, ",".join(diff), len(entries)))
        if len(corrections) > 8:
            print("      ... and %d more" % (len(corrections) - 8))
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
        # DIAG columns ride along (separated by '|') so the table answers
        # "did the free-running effect state differ too?" without a second
        # pass over the CSV. Older CSVs predate them; fall back to "-".
        cols = COMPARE + ["|"] + DIAG
        def cell(r, f):
            return "-" if f == "|" else r.get(f, "-")
        print("      %-4s %-6s %-4s %s"
              % ("idx", "replay", "rb",
                 " ".join("%-12s" % (f if f != "|" else "|") for f in cols)))
        for idx, r in entries:
            print("      %-4d %-6s %-4s %s"
                  % (idx, r["replay"], r["rb"],
                     " ".join("%-12s" % cell(r, f) for f in cols)))
    if verbose and not violations and multi:
        print("   OK: %d multiply-recorded frames all reproduced (%d were "
              "input corrections)" % (len(multi), len(corrections)))
    return len(violations), len(multi), len(corrections)


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("-")]
    verbose = "-v" in argv or "--verbose" in argv
    if not args:
        print(__doc__)
        return 2
    total_v = 0
    total_m = 0
    total_c = 0
    missing = 0
    for path in args:
        if not os.path.exists(path):
            print("== %s: MISSING" % path)
            missing += 1
            continue
        try:
            v, m, c = check_file(path, verbose)
        except ValueError as exc:
            print("== parse error: %s" % exc)
            return 2
        total_v += v
        total_m += m
        total_c += c
    print("SEAM-RING-CHECK: files=%d missing=%d multi_recorded=%d "
          "input_corrections=%d violations=%d -- %s"
          % (len(args), missing, total_m, total_c, total_v,
             "FAIL" if total_v else "PASS"))
    return 1 if total_v else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
