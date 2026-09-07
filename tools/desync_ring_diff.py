#!/usr/bin/env python3
"""Attribute a player-vs-player desync from desync_ring CSVs.

Two modes:

  desync_ring_diff.py FILE...          per-file profile (works with ONE peer)
  desync_ring_diff.py --pair A.csv B.csv   cross-peer diff

WHY A SINGLE FILE IS ALREADY USEFUL. The `rng` column is the raw LCG seed
(seed = seed*0x343FD + 0x269EC3 mod 2^32). The LCG is a bijection, so the
number of draws between consecutive frames is recovered by stepping one row's
seed until it reaches the next row's. That turns the seed column into an exact
per-frame gameplay-draw profile without needing the peer's file at all -- which
matters, because a desync only ever gives you both halves if both processes
dumped in the same run.

The same walk applied to the two peers' seeds AT THE DIVERGING FRAME gives the
draw-count delta between them. Measured on the 2026-09-06 f=194 capture:
host=0x84B91826, guest=0xE4D76E51, and the guest is the host advanced by
EXACTLY ONE draw. So that desync is one extra gameplay draw on one peer, with
p1_hp/p2_hp/round still identical -- not a corrupted seed and not a different
value.

The site_* columns (cumulative draws per exact game_rand call site) turn that
into a name: difference them across the diverging frame and the non-zero one is
the site that drew. Older captures predate those columns and simply omit them.
"""

import argparse
import csv
import sys
from collections import Counter

A, C, M = 0x343FD, 0x269EC3, 0xFFFFFFFF

SITES = ["site_camera", "site_hitdet", "site_csm",
         "site_scriptrand", "site_ai", "site_other"]


def steps(a, b, limit=8192):
    """How many LCG draws take seed `a` to seed `b`, or None if > limit."""
    s = a
    for k in range(limit + 1):
        if s == b:
            return k
        s = (s * A + C) & M
    return None


def load(path):
    header = ""
    with open(path) as fh:
        lines = []
        for line in fh:
            if line.startswith("#"):
                if not header:
                    header = line.strip().lstrip("# ")
                continue
            lines.append(line)
    rows = list(csv.DictReader(lines))
    # Rows with frame > confirmed are speculative runahead saves: the same
    # frame gets re-simulated later, so comparing them across peers compares
    # predictions, not agreed state.
    #
    # A frame can also appear MORE THAN ONCE among the confirmed rows, because
    # rollback re-simulates it. The host does this constantly (a 136-frame
    # baseline run produced 2088 rows). Keep the LAST row for each frame: that
    # is the value the peer finally agreed on.
    conf, resim = {}, 0
    for r in rows:
        f = int(r["frame"])
        if f <= int(r["confirmed"]):
            if f in conf:
                resim += 1          # rollback re-simulated an already-saved frame
            conf[f] = r
        else:
            resim += 1              # speculative runahead save, later rolled back
    return header, rows, [conf[f] for f in sorted(conf)], resim


def profile(conf):
    """{frame: draws made during that frame} via the LCG walk."""
    prof, prev = {}, None
    for r in conf:
        f, seed = int(r["frame"]), int(r["rng"], 16)
        if prev is not None and f == prev[0] + 1:
            prof[f] = steps(prev[1], seed)
        prev = (f, seed)
    return prof


def site_deltas(conf):
    """{frame: {site: draws}} for files that carry the site columns."""
    if not conf or SITES[0] not in conf[0]:
        return {}
    out, prev = {}, None
    for r in conf:
        f = int(r["frame"])
        cur = {s: int(r[s]) for s in SITES}
        if prev is not None and f == prev[0] + 1:
            out[f] = {s: cur[s] - prev[1][s] for s in SITES}
        prev = (f, cur)
    return out


def report_one(path):
    header, rows, conf, resim = load(path)
    print(f"== {path}")
    print(f"   {header}")
    print(f"   rows={len(rows)} confirmed={len(conf)}")
    if not conf:
        print("   no confirmed rows")
        return
    prof = profile(conf)
    has_gp = "gp_calls" in conf[0]

    # Validate the LCG walk against the hook's own counter. A mismatch means a
    # gameplay-seed advance happened that Hook_GameRand did not see -- i.e. a
    # draw bypassing the hook, or render isolation leaking onto the gameplay
    # seed. Measured 0 mismatches in 246 frames on 2026-09-06, which is what
    # retired the "render draw missing g_in_render_rng" hypothesis.
    #
    # ONLY MEANINGFUL ON A PEER THAT SAVED ONCE PER FRAME. Two ways it stops
    # being meaningful, both seen on real captures:
    #   - parity_recorder zeroes g_gameplay_rand_calls every Capture(), so on a
    #     peer recording parity the column is per-frame, not cumulative;
    #   - speculative runahead and rollback re-simulation both draw randoms
    #     that are then thrown away: gp_calls keeps counting them, while the
    #     seed is restored, so the delta between two confirmed frames
    #     legitimately exceeds the seed walk. Only a peer that saved each frame
    #     exactly once and never speculated can be cross-checked this way.
    # Either way the seed walk is the truth and this cross-check is skipped,
    # rather than reporting phantom "draws bypassing the hook" (the host peer
    # of the 21:32 baseline showed 117 of those before this guard existed).
    gps = [int(r["gp_calls"]) for r in conf] if has_gp else []
    cumulative = all(y >= x for x, y in zip(gps, gps[1:]))
    if has_gp and not cumulative:
        print("   LCG walk vs gp_calls: skipped -- gp_calls is per-frame on "
              "this peer (parity recorder resets it), not cumulative")
    elif has_gp and resim:
        print(f"   LCG walk vs gp_calls: skipped -- {resim} speculative/"
              f"re-simulated saves, whose draws gp_calls counts but the "
              f"restored seed does not show")
    elif has_gp:
        bad, prev = 0, None
        for r in conf:
            f, gp = int(r["frame"]), int(r["gp_calls"])
            if prev is not None and f == prev[0] + 1 and prof.get(f) is not None:
                if gp - prev[1] != prof[f]:
                    bad += 1
            prev = (f, gp)
        verdict = "OK -- every seed advance is a hooked draw" if not bad else \
                  f"{bad} MISMATCHES -- draws are bypassing the hook"
        print(f"   LCG walk vs gp_calls: {verdict}")

    vals = [v for v in prof.values() if v is not None]
    print(f"   per-frame draw counts: {dict(sorted(Counter(vals).items()))}"
          f"  unresolved={sum(1 for v in prof.values() if v is None)}")

    sd = site_deltas(conf)
    if sd:
        tot = Counter()
        for d in sd.values():
            tot.update(d)
        print(f"   draws by call site: {dict(tot)}")
    else:
        print("   (no site_* columns -- capture predates exact site attribution)")


def sanity_check(path, rows):
    """Catch a header/emit column shift by checking values against their meaning.

    The ring's CSV header has drifted out of step with the emit order twice,
    and both times the shifted columns produced a confident WRONG answer -- the
    worst one read the always-zero call-site counters as the spawn handles and
    concluded those handles never differ. A column count check would not have
    caught it (the counts matched), so check what the values MEAN instead.
    """
    if not rows:
        return
    r = rows[len(rows) // 2]
    bad = []
    if "spawn0" in r:
        for k in ("spawn0", "spawn1"):
            h = r[k]
            vals = [int(h[i * 8:i * 8 + 8], 16) for i in range(len(h) // 8)]
            # Each handle is either null or a pointer into the object pool.
            if any(v and not (0x4701E0 <= v < 0x4701E0 + 382 * 1024) for v in vals):
                bad.append(f"{k} holds values that are neither null nor "
                           f"object-pool pointers")
    if "gs_src" in r and int(r["gs_src"]) > 100:
        bad.append(f"gs_src={r['gs_src']} is not a plausible game speed")
    if "pool_live" in r and not (0 < int(r["pool_live"]) <= 1024):
        bad.append(f"pool_live={r['pool_live']} is out of range")
    # The strongest invariant, and the one that actually catches a shift:
    # `objs` is the engine's own count of objects it dispatched this frame and
    # `pool_live` is our count of non-zero-type slots. They describe the same
    # population and must be close. A shifted header sends these wildly apart
    # (a real capture read 393 against 1).
    if "objs" in r and "pool_live" in r:
        o, pl = int(r["objs"]), int(r["pool_live"])
        if pl and abs(o - pl) > max(8, pl // 4):
            bad.append(f"objs={o} disagrees with pool_live={pl}; these count "
                       f"the same objects, so the columns are shifted")
    if bad:
        print(f"!! {path}: COLUMNS LOOK MISALIGNED -- do not trust this file:")
        for b in bad:
            print(f"     {b}")


def report_pair(pa, pb):
    ha, ra, ca, _ = load(pa)
    hb, rb, cb, _ = load(pb)
    sanity_check(pa, ra)
    sanity_check(pb, rb)
    print(f"== A {pa}\n   {ha}")
    print(f"== B {pb}\n   {hb}")
    a = {int(r["frame"]): r for r in ca}
    b = {int(r["frame"]): r for r in cb}
    common = sorted(set(a) & set(b))
    if not common:
        print("!! no confirmed frames in common -- are these from the SAME run?")
        return 1
    print(f"   confirmed frames in common: {len(common)} "
          f"({common[0]}..{common[-1]})")

    # objs (live objects dispatched that frame) is checked FIRST in spirit: the
    # script VM random branch is where every gameplay draw comes from, and it
    # runs inside dispatched objects, so an object-count difference is a more
    # upstream cause than the rng difference it produces.
    # sr_hash (script-execution context hash) is the most upstream term: it
    # says the two sims stopped running the same script instructions, which is
    # what produces the extra draw that moves rng. objs is next. rng is the
    # symptom GekkoNet reports.
    # POSITIONS FIRST. They are now part of the wire fingerprint, and in the
    # game-speed stale-latch desync they diverged at f=72 against an rng
    # divergence at f=194 -- i.e. they are the earliest signal by ~120 frames.
    # Synthesised from pool_bits + pool_xs/pool_ys so it slots into the same
    # first-divergence ordering as the scalar fields.
    def posmap(row):
        bits = row.get("pool_bits", "")
        if not bits:
            return None
        slots = []
        for w in range(len(bits) // 8):
            word = int(bits[w * 8:(w + 1) * 8], 16)
            for bit in range(32):
                if word & (1 << bit):
                    slots.append(w * 32 + bit)
        hx, hy = row.get("pool_xs", ""), row.get("pool_ys", "")
        xs = [hx[i * 8:i * 8 + 8] for i in range(len(hx) // 8)]
        ys = [hy[i * 8:i * 8 + 8] for i in range(len(hy) // 8)]
        return {s_: (xs[i], ys[i]) for i, s_ in enumerate(slots)
                if i < len(xs) and i < len(ys)}

    if "pool_xs" in a[common[0]] and "pool_xs" in b[common[0]]:
        for f in common:
            pa_, pb_ = posmap(a[f]), posmap(b[f])
            if pa_ is not None and pa_ != pb_:
                if all(posmap(a[g]) == posmap(b[g]) for g in common if g > f):
                    continue
                print(f"   positions first diverge (and never re-agree) at f={f}:")
                for s_ in sorted(set(pa_) | set(pb_)):
                    if pa_.get(s_) != pb_.get(s_):
                        va, vb = pa_.get(s_), pb_.get(s_)
                        def show(v):
                            if not v:
                                return "absent"
                            x = int(v[0], 16); y = int(v[1], 16)
                            x -= (1 << 32) if x >> 31 else 0
                            y -= (1 << 32) if y >> 31 else 0
                            return f"({x/65536:.3f},{y/65536:.3f})"
                        print(f"      slot {s_:4d}: A={show(va)} B={show(vb)}")
                break

    fields = ["pool", "objs", "rng", "p1_hp", "p2_hp", "round"]
    fields = [f for f in fields if f in a[common[0]] and f in b[common[0]]]
    first = {}
    for f in common:
        for k in fields:
            if k not in first and a[f][k] != b[f][k]:
                # A field that differs then re-agrees is a rollback artifact.
                if all(a[g][k] == b[g][k] for g in common if g > f):
                    continue
                first[k] = f
    if not first:
        print("   no field diverges -- these two peers agree on every "
              "confirmed frame")
        return 0

    for k, f in sorted(first.items(), key=lambda kv: kv[1]):
        print(f"   {k:8s} first diverges (and never re-agrees) at f={f}: "
              f"A={a[f][k]} B={b[f][k]}")

    fd = min(first.values())
    print(f"\n   -- divergence frame f={fd} --")
    sa, sb = int(a[fd]["rng"], 16), int(b[fd]["rng"], 16)
    fwd, rev = steps(sa, sb), steps(sb, sa)
    if fwd is not None:
        print(f"   B's seed is A's advanced by {fwd} draw(s)")
    elif rev is not None:
        print(f"   A's seed is B's advanced by {rev} draw(s)")
    else:
        print("   seeds are not within 8192 draws of each other -- not a "
              "simple draw-count difference")

    # WHICH SLOTS the two pools disagree about. `pool` (the digest) says only
    # THAT they differ; pool_bits is the live-slot bitmap itself, so this names
    # the objects. This is the earliest divergence in every capture so far --
    # earlier than the rng by about a hundred frames -- so it is where the
    # cause lives.
    if "pool_bits" in a[fd] and "pool_bits" in b[fd]:
        def slots(row):
            bits = row["pool_bits"]
            out = set()
            for w in range(len(bits) // 8):
                word = int(bits[w * 8:(w + 1) * 8], 16)
                for bit in range(32):
                    if word & (1 << bit):
                        out.add(w * 32 + bit)
            return out
        sa, sb = slots(a[fd]), slots(b[fd])
        only_a, only_b = sorted(sa - sb), sorted(sb - sa)
        print(f"   live object slots at f={fd}: A has {len(sa)}, B has {len(sb)}")
        print(f"      only on A: {only_a}")
        print(f"      only on B: {only_b}")
        # Same live set but a different digest means a slot holds a different
        # object TYPE. pool_types lists the type of each live slot in ascending
        # slot order, so zipping it against the bitmap names the slot.
        if "pool_types" in a[fd] and "pool_types" in b[fd] and not only_a and not only_b:
            def typemap(row, slotset):
                hexs = row["pool_types"]
                types = [int(hexs[i*2:i*2+2], 16) for i in range(len(hexs)//2)]
                return dict(zip(sorted(slotset), types))
            ta, tb = typemap(a[fd], sa), typemap(b[fd], sb)
            def scriptmap(row, slotset):
                hexs = row.get("pool_scripts", "")
                if not hexs:
                    return {}
                v = [int(hexs[i*4:i*4+4], 16) for i in range(len(hexs)//4)]
                return dict(zip(sorted(slotset), v))
            ca_, cb_ = scriptmap(a[fd], sa), scriptmap(b[fd], sb)
            diffs = [(s, ta[s], tb[s]) for s in sorted(ta)
                     if s in tb and ta[s] != tb[s]]
            if diffs:
                print(f"   the live sets MATCH, but {len(diffs)} slot(s) hold a "
                      f"different object TYPE:")
                for s_, x, y in diffs:
                    extra = ""
                    if s_ in ca_ and s_ in cb_:
                        extra = f"   script A={ca_[s_]} B={cb_[s_]}"
                    print(f"      slot {s_:4d}:  A type={x}  B type={y}{extra}")
            else:
                print("   live sets and types both match -- the digest "
                      "difference is not explained by this row")

    # WHICH SITES DRAW AT ALL. This is the valid cross-peer statement: a site
    # whose cumulative total is zero on both peers cannot be responsible.
    # Per-frame site DELTAS are NOT comparable across peers -- the columns are
    # cumulative and a peer that speculates or rolls back accumulates draws the
    # restored seed never shows, so the host can read 54 where the guest reads
    # 6 on a frame whose real difference is a single draw. Use the seed walk
    # above for the count and this for the location.
    if SITES[0] in ca[0] and SITES[0] in cb[0]:
        print("   sites that drew at all (cumulative over the capture):")
        for site in SITES:
            ta = int(ca[-1][site]) - int(ca[0][site])
            tb = int(cb[-1][site]) - int(cb[0][site])
            note = "" if (ta or tb) else "   (never draws -- excluded)"
            print(f"      {site:16s} A={ta:7d}  B={tb:7d}{note}")
    else:
        print("   (no site_* columns in one or both files -- rebuild the hook "
              "and re-capture to name the call site)")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--pair", action="store_true",
                    help="treat the two files as the two peers of ONE run")
    args = ap.parse_args()
    if args.pair:
        if len(args.files) != 2:
            ap.error("--pair needs exactly two files")
        return report_pair(*args.files)
    for p in args.files:
        report_one(p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
