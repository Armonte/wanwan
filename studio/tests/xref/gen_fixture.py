#!/usr/bin/env python3
"""gen_fixture.py -- dump a compact JSON fixture for the sound-xref test.

Uses the oracle parsers in tools/kgt (fm2nd.parse_file, byte-exact
round-trip proven over the local corpus) so the fixture is derived from
the validated Python side, never from the C++ under test. Names are
DECODED here (CP932 -> str, kgt._try_decode) so the C++ test compares
plain UTF-8 strings without needing kgt::DecodeName.

Fixture shape (fixed; xref_test.cpp's ad-hoc reader depends on it):
  {
    "source":  "<basename of the input file>",
    "scripts": [{"name": str, "script_index": int}, ...],
    "items":   [{"t": int, "p": "<30 hex chars = 15B payload>"}, ...],
    "sounds":  [{"name": str, "size": int}, ...]
  }

Usage:
  gen_fixture.py [input.player] [output.json]
Defaults: /mnt/d/games/fm2k/_NODEV/pkmncc/Bewear.player ->
          <this dir>/bewear_fixture.json
"""
from __future__ import annotations

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools", "kgt"))

import fm2nd            # noqa: E402  (path hack above)
from kgt import _try_decode  # noqa: E402

DEFAULT_INPUT = "/mnt/d/games/fm2k/_NODEV/pkmncc/Bewear.player"
DEFAULT_OUTPUT = os.path.join(HERE, "bewear_fixture.json")


def main() -> int:
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_INPUT
    dst = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_OUTPUT

    ftype, obj = fm2nd.parse_file(src)
    if ftype.endswith("-fm95"):
        print(f"refusing FM95 file: {src}", file=sys.stderr)
        return 1
    h = obj.header

    scripts = [{"name": _try_decode(s.script_name),
                "script_index": s.script_index} for s in h.scripts]
    items = [{"t": si.script_type, "p": si.payload.hex()}
             for si in h.script_items]
    sounds = [{"name": _try_decode(s.name), "size": s.size}
              for s in h.sounds]

    def rows(objs):
        return ",\n  ".join(
            json.dumps(o, ensure_ascii=False, separators=(",", ":"))
            for o in objs)

    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write("{\n")
        f.write(f'"source":{json.dumps(os.path.basename(src))},\n')
        f.write(f'"scripts":[\n  {rows(scripts)}\n],\n')
        f.write(f'"items":[\n  {rows(items)}\n],\n')
        f.write(f'"sounds":[\n  {rows(sounds)}\n]\n')
        f.write("}\n")

    print(f"wrote {dst}: {len(scripts)} scripts, {len(items)} items, "
          f"{len(sounds)} sounds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
