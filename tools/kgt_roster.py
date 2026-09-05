#!/usr/bin/env python3
"""Read a FM2K .kgt's character roster: slot id -> .player file name.

The roster is what maps a replay's p1_char_id / p2_char_id onto an actual
character, so anything that wants to show who was playing needs it first.

Mirrors ParseKgtSummary's FM2K branch (launcher/game/FM2K_KgtParser.cpp).
FM2K's .kgt is a stream of variable-size sections, NOT the fixed-offset
block FM95 uses -- so the name array cannot be read from a constant
offset, the common-resource part has to be walked past first. Reading
FM95's 0x110 out of an FM2K file yields convincing-looking garbage, which
is exactly the failure this docstring exists to prevent.
"""
import struct
import sys
from pathlib import Path

KGT_HEADER_SIZE     = 16 + 256   # signature + project name
SCRIPT_SIZE         = 39
SCRIPT_ITEM_SIZE    = 16
PICTURE_HEADER_SIZE = 20
PALETTE_SIZE        = 1024
SOUND_HEADER_SIZE   = 42
NAME_INFO_SIZE      = 256
MAX_PLAYER          = 50
MAX_REACTION        = 200
REACTION_ITEM_SIZE  = 36
RECOVER_TIME_SIZE   = 4


def _skip_common_resource(b, pos):
    """Walk past scripts, script items, pictures, palettes and sounds."""
    (n,) = struct.unpack_from("<i", b, pos); pos += 4
    pos += n * SCRIPT_SIZE
    (n,) = struct.unpack_from("<i", b, pos); pos += 4
    pos += n * SCRIPT_ITEM_SIZE

    (n,) = struct.unpack_from("<i", b, pos); pos += 4
    for _ in range(n):
        _, w, h, has_priv, size = struct.unpack_from("<iiiii", b, pos)
        pos += PICTURE_HEADER_SIZE
        # The explicit size when it is set, else the pixel area plus a
        # private palette if the picture carries one.
        pos += size if size != 0 else (w * h + (1024 if has_priv else 0))

    # 8 shared palettes, each with an 8-int trailer.
    pos += 8 * (PALETTE_SIZE + 8 * 4)

    (n,) = struct.unpack_from("<i", b, pos); pos += 4
    for _ in range(n):
        # SoundItemHeader: int unknown, char name[32], int size, ...
        # -- the size field is at 36, not 38.
        size = struct.unpack_from("<i", b, pos + 36)[0]
        pos += SOUND_HEADER_SIZE
        if size > 0:
            pos += size
    return pos


def roster(kgt_path):
    """[(slot, name)] for every non-empty player slot, in slot order."""
    b = Path(kgt_path).read_bytes()
    sig = b[:7]
    if sig not in (b"2DKGT2K", b"2DKGT2G"):
        raise ValueError("not an FM2K .kgt (signature %r)" % sig)

    pos = _skip_common_resource(b, KGT_HEADER_SIZE)
    pos += 4  # the +4 the original parser skips here

    out = []
    for i in range(MAX_PLAYER):
        raw = b[pos:pos + NAME_INFO_SIZE]
        pos += NAME_INFO_SIZE
        z = raw.find(b"\x00")
        s = raw[:z if z >= 0 else NAME_INFO_SIZE]
        if s:
            # Names are CP932: these are Japanese tools and the file
            # stores whatever the author typed.
            out.append((i, s.decode("cp932", "replace")))
    return out


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: kgt_roster.py <file.kgt>", file=sys.stderr)
        raise SystemExit(2)
    for slot, name in roster(sys.argv[1]):
        print("%2d  %s" % (slot, name))
