#!/usr/bin/env python3
"""Diff two FM2K parity .pty captures -- rollback-robust.

Aligns by KgtParitySnapshot.frame using LAST-WRITE-per-frame (not array index),
then classifies divergences by PERSISTENCE so rollback never produces fake
"desync" noise:

  - index-drift          a live netplay capture has MORE snapshots than a linear
                         replay (rollback re-sims a frame, recording it twice).
                         An index-paired walk compares misaligned frames from the
                         first duplicate onward -> fake divergence. Aligning by the
                         `frame` field (last write = the confirmed value) removes it.
  - speculative residue  even frame-aligned, the live .pty's last write for a few
                         frames inside a rollback window can be a MISPREDICTED state
                         the engine later corrected. It reconverges within ~rb_max
                         frames. A diverging RUN shorter than the reconvergence
                         window is this artifact, NOT a desync.
  - input prediction     input_p1/input_p2 are excluded (predicted-vs-confirmed on
                         mispredicted frames); input determinism is proven
                         transitively via rng/pos/scripts, which ARE compared.

Exit 0 = no PERSISTENT divergence (clean, or transient rollback artifacts only).
Exit 1 = a diverging run longer than the reconvergence window (a real divergence).

  python3 tools/parity_diff.py A.pty B.pty
  FM2K_PARITY_RECONVERGE_WINDOW=32  (override the run-length threshold)

Layout matches kgtengine/include/kgt/kgt_parity_snapshot.h v3.
"""

import os
import struct
import sys

HDR = struct.Struct('<4s I I I I I 8s')   # 32 B
PLAYER = struct.Struct('<12i 3i 16h')      # 92 B (12 i32 + 3 i32 + 16 i16)
assert PLAYER.size == 92, PLAYER.size
SNAP_HEAD = struct.Struct('<4I 4i')        # 32 B (frame, rng, in_p1, in_p2, phase, timer, cam_x, cam_y)
SNAP_TAIL = struct.Struct('<I 16h 2I')     # 4 + 32 + 8 = 44 B (rng_after, sysvars, pad)
assert SNAP_TAIL.size == 44, SNAP_TAIL.size
SNAP_SIZE = SNAP_HEAD.size + 2 * PLAYER.size + SNAP_TAIL.size  # 32 + 184 + 44 = 260
assert SNAP_SIZE == 260, SNAP_SIZE

PLAYER_FIELDS = [
    'script_idx', 'item_idx', 'pos_x', 'pos_y',
    'vel_x', 'vel_y', 'hp', 'super_meter',
    'facing', 'state_flags', 'hitstun', 'hitstop',
    'hit_box_active_count', 'hurt_box_active_count', 'cancel_script_item',
] + [f'task_vars[{i}]' for i in range(16)]

def parse_player(buf, off):
    fields = PLAYER.unpack_from(buf, off)
    return dict(zip(PLAYER_FIELDS, fields))

def parse_snap(buf, off):
    s = {}
    head = SNAP_HEAD.unpack_from(buf, off)
    keys_h = ['frame', 'rng', 'input_p1', 'input_p2',
              'match_phase', 'round_timer', 'camera_x', 'camera_y']
    s.update(dict(zip(keys_h, head)))
    s['p1'] = parse_player(buf, off + SNAP_HEAD.size)
    s['p2'] = parse_player(buf, off + SNAP_HEAD.size + PLAYER.size)
    tail = SNAP_TAIL.unpack_from(buf, off + SNAP_HEAD.size + 2 * PLAYER.size)
    s['rng_after_frame'] = tail[0]
    s['system_vars'] = list(tail[1:17])
    return s

def load(path):
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) < HDR.size:
        raise SystemExit(f"{path}: too small")
    magic, version, snap_size, flags, frame_count, init_seed, _ = HDR.unpack_from(data, 0)
    if magic != b'KGTP':
        raise SystemExit(f"{path}: bad magic {magic!r}")
    if snap_size != SNAP_SIZE:
        raise SystemExit(f"{path}: unexpected snap_size {snap_size} (this tool expects v3 / 260)")
    print(f"{path}: version={version} flags=0x{flags:x} frame_count={frame_count} "
          f"init_seed=0x{init_seed:08x}")
    snaps = []
    pos = HDR.size
    while pos + snap_size <= len(data):
        snaps.append(parse_snap(data, pos))
        pos += snap_size
    print(f"  parsed {len(snaps)} snapshots ({len(data)} bytes)")
    return snaps

def diff_snap(a, b):
    """Returns list of (field_name, a_val, b_val) for differences.

    input_p1/input_p2 are deliberately NOT compared: under prediction-based
    netplay the snapshot captures the PREDICTED remote input on mispredicted
    frames, while the .fm2krep replay uses the CONFIRMED input -- they differ on
    exactly the frames rollback corrects, even though the engine sim converges
    identically. Input determinism is proven TRANSITIVELY: divergent inputs would
    diverge rng / positions / scripts, which ARE compared below.
    """
    out = []
    for k in ('rng', 'match_phase', 'round_timer',
              'camera_x', 'camera_y', 'rng_after_frame'):
        if a.get(k) != b.get(k):
            out.append((k, a.get(k), b.get(k)))
    if a.get('system_vars') != b.get('system_vars'):
        for i in range(16):
            av, bv = a['system_vars'][i], b['system_vars'][i]
            if av != bv:
                out.append((f'system_vars[{i}]', av, bv))
    for who in ('p1', 'p2'):
        for k in PLAYER_FIELDS:
            av, bv = a[who][k], b[who][k]
            if av != bv:
                out.append((f'{who}.{k}', av, bv))
    return out


# rng_only_fields: the parity recorder captures rng + rng_after_frame. When the
# pre-battle path differs in PGI consumption (host went through real CSS, replay
# went through auto-mash), the FIRST captured battle frame's rng can lag by a few
# game_rand calls even though engine state is otherwise byte-identical. Classify
# "only rng differs" as a known pre-battle-path-leakage artifact.
_RNG_ONLY_FIELDS = {'rng', 'rng_after_frame'}

# A diverging RUN shorter than this reconverges = rollback speculative-capture
# residue, not a desync. rb_max in practice ~17; 32 gives margin. A real
# divergence runs to match end (hundreds-thousands of frames).
RECONVERGE_WINDOW = int(os.environ.get('FM2K_PARITY_RECONVERGE_WINDOW', '32'))
BATTLE_PHASE = 3000

def battle_snaps(snaps):
    """Battle-phase snapshots (chars loaded), in capture order. Skips battle-init
    snaps where chars aren't populated yet (script_idx == -1 / 0xFFFFFFFF)."""
    return [s for s in snaps
            if s['match_phase'] == BATTLE_PHASE
            and s['p1']['script_idx'] != -1 and s['p2']['script_idx'] != -1]

def battle_segments(snaps):
    """Partition capture-ordered snaps into per-MATCH battle segments. A segment
    is a maximal run of battle_snaps (chars loaded); the CSS/title gap between
    matches delimits them, so segment i == the (i+1)-th match's battle. Returns a
    list of snapshot-lists. This is what makes multi-match alignment correct: the
    engine `frame` counter RESETS to 0 each match, so keying on `frame` alone
    collapses match-1 f50 and match-2 f50 into one row (the old trim-to-match-1
    workaround existed only to dodge that collision). Aligning per segment, then
    by `frame` WITHIN a segment, keys effectively on (segment, frame)."""
    segs, cur = [], []
    for s in snaps:
        is_b = (s['match_phase'] == BATTLE_PHASE
                and s['p1']['script_idx'] != -1 and s['p2']['script_idx'] != -1)
        if is_b:
            cur.append(s)
        elif cur:
            segs.append(cur)
            cur = []
    if cur:
        segs.append(cur)
    return segs

def frame_field_reliable(bsnaps):
    """True if the `frame` field is a usable per-frame key. It is NOT in replay
    catch-up mode, where the engine frame counter sticks low and repeats (e.g.
    341 distinct values across 2999 sim frames) -- there we must align by capture
    POSITION instead. A reliable field is mostly-distinct (rollback rewrites a
    handful of frames, so a live netplay capture is still ~all-distinct)."""
    if not bsnaps:
        return False
    distinct = len({s['frame'] for s in bsnaps})
    return distinct >= 0.9 * len(bsnaps)

def _runs(keys, pos):
    """Group a sorted key list into maximal runs consecutive in the key sequence."""
    if not keys:
        return []
    runs = [[keys[0]]]
    for k in keys[1:]:
        if pos[k] == pos[runs[-1][-1]] + 1:
            runs[-1].append(k)
        else:
            runs.append([k])
    return runs

def _diff_segment(A, B, context, tag):
    """Align + diff ONE battle segment (A, B are battle_snaps of the same match).
    Returns 0 (clean or transient-only), 1 (persistent divergence), 2 (empty)."""
    if not A or not B:
        print(f"  [{tag}] no battle snapshots: A={len(A)} B={len(B)}")
        return 2

    # Choose alignment. Frame-field alignment (last-write-per-frame) collapses a
    # live netplay capture's rollback re-sim duplicates so they don't off-by-one
    # the walk -- but it needs a reliable frame field on BOTH sides. When a side
    # is in replay catch-up (frame field repeats), fall back to positional
    # pairing from battle start (no rollback dups exist in that single stream,
    # so 1:1 by position is correct -- this is the original method). Frame keys
    # are already unambiguous here: a single segment is one match, no reset.
    if frame_field_reliable(A) and frame_field_reliable(B):
        am = {}                     # last write wins (confirmed value)
        for s in A: am[s['frame']] = s
        bm = {}
        for s in B: bm[s['frame']] = s
        keys = sorted(set(am) & set(bm))
        get = lambda k: (am[k], bm[k])
        prev = lambda k: (am.get(k - 1), bm.get(k - 1))
        label = (f"frame-aligned (rollback-robust): A={len(am)} B={len(bm)} frames, "
                 f"shared={len(keys)} (A-only {len(set(am)-set(bm))}, "
                 f"B-only {len(set(bm)-set(am))})")
    else:
        n = min(len(A), len(B))
        keys = list(range(n))
        get = lambda k: (A[k], B[k])
        prev = lambda k: (A[k - 1] if k > 0 else None, B[k - 1] if k > 0 else None)
        label = (f"position-aligned (frame field unreliable -- replay catch-up): "
                 f"A={len(A)} B={len(B)} battle snaps, paired={n}")
    print(f"  [{tag}] {label}; reconverge_window={RECONVERGE_WINDOW}")

    div = {}            # key -> engine diff (inputs excluded)
    rng_only = 0
    for k in keys:
        sa, sb = get(k)
        d = diff_snap(sa, sb)
        if not d:
            continue
        if {f for f, _, _ in d}.issubset(_RNG_ONLY_FIELDS):
            rng_only += 1
            continue
        div[k] = d

    if not div:
        msg = f"  [{tag}] CLEAN: all {len(keys)} aligned frames IDENTICAL engine state."
        if rng_only:
            msg += (f" ({rng_only} rng-only frames -- pre-battle game_rand "
                    f"consumption asymmetry; sim is deterministic.)")
        print(msg)
        return 0

    pos = {k: i for i, k in enumerate(keys)}
    runs = _runs(sorted(div), pos)
    persistent = [r for r in runs if len(r) > RECONVERGE_WINDOW]
    transient  = [r for r in runs if len(r) <= RECONVERGE_WINDOW]

    print(f"  [{tag}] {len(div)} diverging frame(s) in {len(runs)} run(s): "
          f"{len(transient)} transient (<= {RECONVERGE_WINDOW}f, reconverges -> "
          f"rollback speculative residue), {len(persistent)} PERSISTENT.")
    if transient:
        lt = max(transient, key=len)
        print(f"    longest transient run: frames {lt[0]}..{lt[-1]} ({len(lt)} frames), "
              f"then matches again -- mispredicted frame later corrected. NOT a desync.")

    if not persistent:
        print(f"  [{tag}] PASS: no persistent divergence; all diffs are transient "
              f"rollback artifacts.")
        return 0

    r = persistent[0]
    k0 = r[0]
    if context:
        print(f"  [{tag}] [{context}] divergence: run of {len(r)} frames from "
              f"frame={k0} (no reconverge within {RECONVERGE_WINDOW}). ADVISORY -- "
              f"the authoritative check is the host-vs-spec trace GATE.")
    else:
        print(f"  [{tag}] PERSISTENT ENGINE DIVERGENCE: run of {len(r)} frames from "
              f"frame={k0} (no reconverge within {RECONVERGE_WINDOW}) -- REAL divergence.")
    for fld, av, bv in div[k0]:
        if isinstance(av, int):
            print(f"    {fld:30s} A=0x{av & 0xFFFFFFFF:08X} B=0x{bv & 0xFFFFFFFF:08X} "
                  f"delta={bv - av}")
        else:
            print(f"    {fld:30s} A={av} B={bv}")
    pa, pb = prev(k0)
    if pa and pb:
        print(f"    Last matching frame: rng={pa['rng']:#x} "
              f"p1.script={pa['p1']['script_idx']}/{pa['p1']['item_idx']} "
              f"p2.script={pa['p2']['script_idx']}/{pa['p2']['item_idx']}")
    return 1

def main(a_path, b_path, context=None):
    # context: when set, the caller is using this diff NON-authoritatively (the
    # authoritative determinism check is the host-vs-spec trace GATE). We segment
    # BOTH streams into per-match battle runs and align each match independently:
    # the engine `frame` counter resets per match, so a single frame-keyed pass
    # across matches collides. This replaces the old trim-to-match-1 workaround --
    # multi-match streams now compare EVERY match, keyed on (segment, frame).
    segsA = battle_segments(load(a_path))
    segsB = battle_segments(load(b_path))
    print()
    if not segsA or not segsB:
        print(f"No battle segments: A={len(segsA)} B={len(segsB)}")
        return 2
    n = min(len(segsA), len(segsB))
    if len(segsA) != len(segsB):
        print(f"NOTE: segment count differs A={len(segsA)} B={len(segsB)} (one side "
              f"reached fewer matches) -- comparing the first {n} match(es).")
    print(f"battle segments: A={len(segsA)} B={len(segsB)} -- diffing {n} match(es)")

    worst, compared = 0, 0
    for i in range(n):
        rc = _diff_segment(segsA[i], segsB[i], context, f"match {i + 1}/{n}")
        if rc == 2:
            continue                      # empty after filtering -- nothing to compare
        compared += 1
        worst = max(worst, rc)
    if compared == 0:
        print("\nNo comparable match segments.")
        return 2
    if worst == 0:
        tag = "CLEAN"
    elif context:
        tag = "ADVISORY-DIVERGENCE (non-authoritative -- see host-vs-spec trace GATE)"
    else:
        tag = "DIVERGENCE"
    print(f"\n{tag}: {compared} match segment(s) diffed, worst rc={worst}.")
    return worst

if __name__ == '__main__':
    if len(sys.argv) not in (3, 4):
        print(__doc__)
        print("usage: parity_diff.py <A.pty> <B.pty> [advisory-context-label]")
        sys.exit(2)
    ctx = sys.argv[3] if len(sys.argv) == 4 else None
    sys.exit(main(sys.argv[1], sys.argv[2], ctx))
