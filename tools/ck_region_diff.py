#!/usr/bin/env python3
"""Which part of the state diverged FIRST, host versus spectator?

The [CHECKSUM] crc is a fold of rng, HP/positions, timers and ring inputs, so
when a spectator desyncs it says only "state differs" -- which is where this
investigation kept stalling. With FM2K_CK_REGIONS=1 the hook also emits the
regions the crc is built from, and this reports the first frame each one
moves. The EARLIEST region is the cause; everything after it is downstream.

Usage:
  python3 tools/ck_region_diff.py <host_log> <spec_log> [--offset N]

--offset is the spectator-to-host frame offset the harness reports (e.g.
"off-1" means spectator frame N lines up with host frame N-1). Without it the
tool tries every offset in -3..3 and picks the one that matches longest,
because comparing misaligned frames makes everything look broken.
"""
import re
import sys
from collections import OrderedDict

PAT = re.compile(
    r"\[CHECKSUM\] f=(-?\d+) crc=0x([0-9A-Fa-f]+).*?"
    r"rrng=0x([0-9A-Fa-f]+) rgs=0x([0-9A-Fa-f]+) rop=0x([0-9A-Fa-f]+) "
    r"rcd=0x([0-9A-Fa-f]+) rit=0x([0-9A-Fa-f]+) re1=0x([0-9A-Fa-f]+) "
    r"re2=0x([0-9A-Fa-f]+) rsh=0x([0-9A-Fa-f]+)")

TERMS = ["crc", "rng", "game_state", "object_pool", "char_dynamic",
         "input_tracking", "effect_sys1", "effect_sys2", "shake"]


def load(path):
    rows = OrderedDict()
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = PAT.search(line)
            if not m:
                continue
            f = int(m.group(1))
            rows[f] = tuple(int(m.group(i), 16) for i in range(2, 11))
    return rows


def score(host, spec, off):
    """How many frames agree on the crc at this offset."""
    n = 0
    for f, sv in spec.items():
        hv = host.get(f + off)
        if hv and hv[0] == sv[0]:
            n += 1
    return n


def main():
    if len(sys.argv) < 3:
        print(__doc__); return 2
    host = load(sys.argv[1])
    spec = load(sys.argv[2])
    if not host or not spec:
        print(f"no region-bearing [CHECKSUM] lines "
              f"(host={len(host)} spec={len(spec)}) -- was FM2K_CK_REGIONS=1 set?")
        return 2

    off = None
    if "--offset" in sys.argv:
        off = int(sys.argv[sys.argv.index("--offset") + 1])
    else:
        off = max(range(-3, 4), key=lambda o: score(host, spec, o))
    print(f"host frames={len(host)} spec frames={len(spec)} offset={off:+d} "
          f"(crc agrees on {score(host, spec, off)} frames)")

    first = {t: None for t in TERMS}
    compared = 0
    for f in sorted(spec):
        hv = host.get(f + off)
        if not hv:
            continue
        compared += 1
        sv = spec[f]
        for i, t in enumerate(TERMS):
            if first[t] is None and hv[i] != sv[i]:
                first[t] = (f, hv[i], sv[i])
    print(f"compared {compared} aligned frames\n")

    order = sorted((v[0], t) for t, v in first.items() if v)
    if not order:
        print("every term agreed on every aligned frame -- no divergence here.")
        return 0
    print("first divergence per term, earliest first:")
    for frame, t in order:
        f, hv, sv = first[t]
        print(f"  {t:15s} spec-f={f:<6d} host=0x{hv:08X} spec=0x{sv:08X}")
    clean = [t for t in TERMS if first[t] is None]
    if clean:
        print(f"\nnever diverged: {', '.join(clean)}")
    print(f"\nEARLIEST: {order[0][1]} at spec-f={order[0][0]} -- "
          "everything later is downstream of it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
