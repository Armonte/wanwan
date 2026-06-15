#!/usr/bin/env python3
"""sprite_compress_probe.py -- measure REAL lossless shrink on FM2K sprite data.

Sprites turned out to be ~74% of the bytes and are mostly stored uncompressed
(size==0). This probes what we actually recover, losslessly, with:
  1. exact-frame dedup (hash each frame body; identical frames stored once)
  2. cross-FILE dedup (cutscenes/.demo re-embed character art?)
  3. solid stream compression (lzma as a portable stand-in for zstd-19 dict)

Reuses kgt.py record parsers. lzma is built-in; if `zstandard` is importable we
also report zstd-19 (what we'd actually ship on disk).

Usage: python3 sprite_compress_probe.py FILE [FILE ...]
"""
from __future__ import annotations

import hashlib
import io
import lzma
import os
import sys

import kgt

try:
    import zstandard as _zstd  # optional
except Exception:
    _zstd = None


def _mb(n): return n / (1024.0 * 1024.0)


def collect_sprites(path):
    """Yield each non-empty sprite frame_content blob from a container's
    common-resource prefix."""
    with open(path, "rb") as f:
        data = f.read()
    buf = io.BytesIO(data)
    sig = kgt._bytes(buf, 16)
    if sig[:7] not in (b"2DKGT2K", b"2DKGT2G"):
        raise ValueError(f"bad sig {sig[:7]!r}")
    kgt._bytes(buf, 256)
    sc_n = kgt._i32(buf)
    for _ in range(sc_n): kgt.Script.parse(buf)
    si_n = kgt._i32(buf)
    for _ in range(si_n): kgt.ScriptItem.parse(buf)
    sf_n = kgt._i32(buf)
    blobs = []
    for _ in range(sf_n):
        sf = kgt.SpriteFrame.parse(buf)
        if sf.frame_content:
            blobs.append(sf.frame_content)
    return blobs


def lzma_size(blob: bytes) -> int:
    # preset 6 is a good speed/ratio midpoint; raw filter, no container overhead
    f = [{"id": lzma.FILTER_LZMA2, "preset": 9}]
    return len(lzma.compress(blob, format=lzma.FORMAT_RAW, filters=f))


def zstd_size(blob: bytes, level=19) -> int:
    if _zstd is None:
        return -1
    return len(_zstd.ZstdCompressor(level=level).compress(blob))


def main(argv):
    if len(argv) < 2:
        print(__doc__); return 2

    global_hashes = {}      # hash -> size (cross-file unique set)
    grand_raw = 0
    grand_files_raw = 0

    per_file = []
    for path in argv[1:]:
        try:
            blobs = collect_sprites(path)
        except Exception as e:
            sys.stderr.write(f"SKIP {path}: {e}\n"); continue
        raw = sum(len(b) for b in blobs)
        # intra-file dedup
        seen = {}
        for b in blobs:
            h = hashlib.sha1(b).digest()
            seen.setdefault(h, len(b))
            if h not in global_hashes:
                global_hashes[h] = len(b)
        dedup = sum(seen.values())
        per_file.append((path, len(blobs), len(seen), raw, dedup))
        grand_raw += raw
        grand_files_raw += raw

        print(f"\n=== {os.path.basename(path)} ===")
        print(f"  frames: {len(blobs)}  unique(in-file): {len(seen)}")
        print(f"  raw sprite bytes:        {_mb(raw):8.1f} MB")
        print(f"  after in-file dedup:     {_mb(dedup):8.1f} MB  "
              f"({raw/max(dedup,1):.2f}x)")

    # global cross-file dedup size
    global_unique = sum(global_hashes.values())

    # compress the GLOBAL unique sprite set as one solid stream (sampled if huge)
    print("\n" + "=" * 52)
    print(f"GRAND TOTAL sprite bytes (raw):     {_mb(grand_files_raw):8.1f} MB")
    print(f"cross-file unique sprite bytes:    {_mb(global_unique):8.1f} MB  "
          f"({grand_files_raw/max(global_unique,1):.2f}x from dedup alone)")

    # Build the solid stream of unique blobs and compress. Cap the sample so the
    # probe stays fast on multi-hundred-MB sets; ratio is representative.
    SAMPLE_CAP = 96 * 1024 * 1024
    stream = bytearray()
    for path in argv[1:]:
        try:
            for b in collect_sprites(path):
                stream += b
                if len(stream) >= SAMPLE_CAP:
                    break
        except Exception:
            continue
        if len(stream) >= SAMPLE_CAP:
            break
    sample = bytes(stream)
    lz = lzma_size(sample)
    print(f"\nsolid-stream compression on {_mb(len(sample)):.0f} MB sample "
          f"(NOT deduped, raw frames concatenated):")
    print(f"  lzma -9:   {_mb(lz):8.1f} MB  ({len(sample)/max(lz,1):.2f}x)")
    if _zstd is not None:
        zz = zstd_size(sample, 19)
        print(f"  zstd-19:   {_mb(zz):8.1f} MB  ({len(sample)/max(zz,1):.2f}x)")
    else:
        print("  (install `zstandard` for the zstd-19 number we'd actually ship)")

    # combined estimate: dedup ratio * solid-compress ratio
    dedup_ratio = grand_files_raw / max(global_unique, 1)
    comp_ratio = len(sample) / max(lz, 1)
    combined = grand_files_raw / (dedup_ratio * comp_ratio)
    print(f"\nrough combined (dedup x compress): "
          f"{_mb(grand_files_raw):.0f} MB sprites -> ~{_mb(combined):.0f} MB "
          f"({dedup_ratio*comp_ratio:.1f}x)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
