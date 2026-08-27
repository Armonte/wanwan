#!/usr/bin/env python3
# sprite_oracle.py -- Python side of the sprite_decode differential test.
# Prints the same lines as tests/sprite_oracle (C++) so the two can be
# diffed directly. Reference implementation is tools/dump_player_pics.py,
# itself a byte-exact port of the engine's _2dfm::decompress.
import importlib.util, sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("dpp", REPO / "tools" / "dump_player_pics.py")
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

def fnv1a(b):
    h = 1469598103934665603
    for x in b:
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h

parsed = m.parse_player_or_kgt(Path(sys.argv[1]))
for i, p in enumerate(parsed["pictures"]):
    if p.width <= 0 or p.height <= 0:
        print(f"{i} EMPTY"); continue
    raw = p.raw
    if len(raw) < p.width * p.height:
        print(f"{i} FAIL"); continue
    print(f"{i} {p.width} {p.height} {1 if p.has_private_palette else 0} "
          f"{1 if p.compressed else 0} {fnv1a(raw):016x}")
