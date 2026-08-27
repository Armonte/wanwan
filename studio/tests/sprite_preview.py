#!/usr/bin/env python3
# sprite_preview.py -- render decoded sprites to a PNG contact sheet.
#
# The hash test (sprite_oracle + sprite_oracle.py) proves the INDEX PLANE
# matches the reference decompressor byte for byte. It cannot catch a
# palette bug: an inverted BGRA swizzle, an honored alpha sentinel, or
# swapped width/height all leave the index plane identical and still render
# garbage. This is the eyeball check for that half, and how the decoder was
# confirmed originally -- Mikyaku sprites 97/101/103/18 came out as the HUD
# digits "0" "2" "3" and the "Perfect!" banner.
#
# Ground is magenta so keyed (transparent) pixels are unmistakable.
#
#   ./tests/sprite_preview.py FILE.kgt 97 101 103 18 [-o out.png]
import struct, subprocess, sys, zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent

def main():
    args = [a for a in sys.argv[1:]]
    out = Path("sprites.png")
    if "-o" in args:
        i = args.index("-o"); out = Path(args[i + 1]); del args[i:i + 2]
    if len(args) < 2:
        print(__doc__); return 2
    src, idxs = args[0], [int(a) for a in args[1:]]

    tiles = []
    for idx in idxs:
        raw = HERE / f".s{idx}.raw"
        r = subprocess.run([str(HERE / "sprite_oracle"), src, "--rgba", str(idx), str(raw)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(f"sprite {idx}: {r.stderr.strip()}"); continue
        w, h = map(int, r.stdout.split())
        tiles.append((idx, w, h, raw.read_bytes()))
        raw.unlink()
    if not tiles:
        print("nothing decoded"); return 1

    gw = sum(t[1] for t in tiles) + 10 * (len(tiles) + 1)
    gh = max(t[2] for t in tiles) + 20
    px = [[(255, 0, 255)] * gw for _ in range(gh)]
    x = 10
    for _idx, w, h, data in tiles:
        for row in range(h):
            for col in range(w):
                o = (row * w + col) * 4
                if data[o + 3]:
                    px[10 + row][x + col] = (data[o], data[o + 1], data[o + 2])
        x += w + 10

    rows = b"".join(b"\x00" + bytes(v for p in r for v in p) for r in px)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    out.write_bytes(b"\x89PNG\r\n\x1a\n"
                    + chunk(b"IHDR", struct.pack(">IIBBBBB", gw, gh, 8, 2, 0, 0, 0))
                    + chunk(b"IDAT", zlib.compress(rows, 9))
                    + chunk(b"IEND", b""))
    print(f"wrote {out}  {gw}x{gh}  tiles={[(t[0], t[1], t[2]) for t in tiles]}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
