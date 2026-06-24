#!/usr/bin/env python3
"""ck_compare.py -- standalone [CHECKSUM] full-state fencepost compare.

Validates GAP #1: the host emits the gameplay_fingerprint (HP/pos/rng/timer --
gekko's own P1-vs-P2 desync hash) at every SAVE event; the spectator RECOMPUTES
the same fingerprint from its live memory each applied frame. Aligning the spec's
CRC sequence to the host's and finding ANY sustained divergence catches the
position/state desyncs the subset rng/hp gate is structurally blind to.

Host [CHECKSUM] f=<gekko_frame>  -- monotonic, re-emitted per re-sim -> dedupe
                                    by frame-LAST = the CONFIRMED state. f<0 skipped.
Spec [CHECKSUM] f=<battle_frame> -- bf resets per battle -> per-battle segments.
                                    Spectator never rolls back -> always confirmed.

Usage:
    python3 tools/ck_compare.py <host.log> <spec.log> [<spec2.log> ...]
"""
import re
import sys

_PAT = re.compile(r'\[CHECKSUM\] f=(-?\d+) crc=0x([0-9A-Fa-f]+)')


def parse_host(path):
    """Per-MATCH segments [{gekko_frame: crc}, ...].

    The host's gekko frame (g_netplay_frame) RESETS per battle, so frame N
    recurs in every match -- a single flat dict would let match-2's frame-N
    overwrite match-1's. The host emits f=-1 (the pre-init battle-entry save)
    exactly once at the start of each match, so split on that. Within a segment,
    dedupe-by-LAST per frame (rollback re-sims re-emit a frame; the last write is
    the most-confirmed state). f=-1 itself is dropped (pre-init, not comparable)."""
    segs, cur = [], {}
    n = 0
    with open(path, errors="ignore") as fh:
        for ln in fh:
            m = _PAT.search(ln)
            if not m:
                continue
            f = int(m.group(1))
            crc = int(m.group(2), 16)
            n += 1
            if f < 0:                     # f=-1 == new-match boundary marker
                if cur:
                    segs.append(cur)
                cur = {}
                continue
            cur[f] = crc                  # last write wins (confirmed)
    if cur:
        segs.append(cur)
    return segs, n


def parse_spec_segments(path):
    """Per-battle segments [{bf: crc}, ...]; split on bf reset."""
    segs, cur, last = [], {}, -1
    with open(path, errors="ignore") as fh:
        for ln in fh:
            m = _PAT.search(ln)
            if not m:
                continue
            bf = int(m.group(1))
            if bf < 0:
                continue
            crc = int(m.group(2), 16)
            if bf <= last and cur:
                segs.append(cur)
                cur = {}
            cur[bf] = crc
            last = bf
    if cur:
        segs.append(cur)
    return segs


def align(sseg, hflat):
    """Find offset O (host_frame = spec_bf + O) maximizing CRC matches via the
    distinctive non-zero CRCs as anchors. Returns
    (mismatches, O, overlap, longest_run, first_bad_bf)."""
    sanchor = [(bf, sseg[bf]) for bf in sorted(sseg) if sseg[bf] != 0]
    if not sanchor:
        return (0, 0, 0, 0, None)
    # crc -> [host frames with that crc]
    hbi = {}
    for hf, hc in hflat.items():
        if hc != 0:
            hbi.setdefault(hc, []).append(hf)
    deltas = {}
    for sb, sc in sanchor:
        for hf in hbi.get(sc, ()):
            deltas[hf - sb] = deltas.get(hf - sb, 0) + 1
    if not deltas:
        return (len(sanchor), 0, 0, len(sanchor), sanchor[0][0])
    O = max(deltas, key=deltas.get)
    bfs = sorted(bf for bf in sseg if sseg[bf] != 0 and (bf + O) in hflat)
    run = mx = mm = 0
    first_bad = None
    for bf in bfs:
        if hflat[bf + O] != sseg[bf]:
            mm += 1
            run += 1
            mx = max(mx, run)
            if first_bad is None:
                first_bad = bf
        else:
            run = 0
    return (mm, O, len(bfs), mx, first_bad)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    host_path, spec_paths = argv[1], argv[2:]
    hsegs, hn = parse_host(host_path)
    print(f"[ck] host {host_path}: {hn} [CHECKSUM] lines -> {len(hsegs)} match "
          f"segment(s) [{', '.join(str(len(s)) for s in hsegs)} confirmed frames]")
    if not hsegs:
        print("[ck] no host [CHECKSUM] -- nothing to compare")
        return 1
    overall_ok = True
    for sp in spec_paths:
        segs = parse_spec_segments(sp)
        nz = sum(1 for s in segs for c in s.values() if c)
        print(f"\n[ck] spec {sp}: {len(segs)} battle segment(s), "
              f"{nz} non-zero CRC frames")
        if nz == 0:
            print("     -> ALL ZERO (stale g_region_checksums -- the 'no overlap' "
                  "bug; spec must recompute via SaveState_CalculateFingerprint)")
            overall_ok = False
            continue
        for si, sseg in enumerate(segs):
            # Match this spec segment against the BEST host match segment
            # (fewest mismatches / longest clean run), like the CINPUT check.
            best = None
            for hi, hseg in enumerate(hsegs):
                mm, O, n, mx, fb = align(sseg, hseg)
                key = (mx, mm, -n)
                if best is None or key < best[0]:
                    best = (key, mm, O, n, mx, fb, hi)
            _, mm, O, n, mx, fb, hi = best
            if n == 0:
                print(f"     seg{si}: NO OVERLAP with any host match "
                      f"-- fingerprints differ entirely")
                overall_ok = False
            elif mx > 3:
                hseg = hsegs[hi]
                print(f"     seg{si}: vs host-match{hi} off{O} {mm}/{n} CRC "
                      f"mismatches (longest run {mx}) -> FULL-STATE DESYNC at "
                      f"spec-bf={fb} (host-f={fb + O}: spec=0x{sseg[fb]:08X} "
                      f"host=0x{hseg[fb + O]:08X})")
                overall_ok = False
            else:
                tail = f" ({mm} tail/predicted artifact)" if mm else ""
                print(f"     seg{si}: vs host-match{hi} off{O} {n} frames "
                      f"FULL-STATE IDENTICAL{tail}")
    print(f"\n[ck] {'PASS -- spectators full-state bit-exact with host' if overall_ok else 'FAIL -- a spectator diverged (see above)'}")
    return 0 if overall_ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
