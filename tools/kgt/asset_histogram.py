#!/usr/bin/env python3
"""asset_histogram.py -- measure the asset-class byte split of FM2K containers.

Parses ONLY the common-resource prefix (scripts -> script_items -> sprite_frames
-> 8 palettes -> sounds) which is byte-identical across .kgt / .player / .stage /
.demo. Stops after the sound table (the per-file tail does not hold bulk assets).

Reuses the byte-exact record parsers from kgt.py. Purpose: validate the
compression thesis -- is audio actually the dominant byte source? -- and project
the packed size under aggressive Opus (audio) + zstd-ish (sprites) before we
write a single codec.

Usage:
    python3 asset_histogram.py FILE [FILE ...]
"""
from __future__ import annotations

import io
import os
import struct
import sys

import kgt  # same directory; reuse Script/ScriptItem/SpriteFrame/Palette/Sound


def _mb(n: int) -> float:
    return n / (1024.0 * 1024.0)


# --- Opus projection knobs (aggressive, per the locked decision) ---
OPUS_BPS_STEREO = 96_000   # cutscene voice / BGM
OPUS_BPS_MONO   = 64_000   # SFX / short mono
SPRITE_ZSTD_FACTOR = 0.45  # conservative: native-RLE/raw sprite -> zstd-dict est.
                           # (real number comes from the packer; this is a placeholder
                           #  so the projection is not wildly optimistic)


def parse_wav_duration(data: bytes):
    """Return (seconds, channels, sample_rate, bits) for a RIFF/WAVE blob, or None."""
    if len(data) < 44 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return None
    pos = 12
    fmt = None
    data_bytes = 0
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        csz = struct.unpack_from("<I", data, pos + 4)[0]
        body = pos + 8
        if cid == b"fmt " and body + 16 <= len(data):
            _afmt, ch, sr, _br, _ba, bits = struct.unpack_from("<HHIIHH", data, body)
            fmt = (ch, sr, bits)
        elif cid == b"data":
            data_bytes = csz
        pos = body + csz + (csz & 1)
    if not fmt or data_bytes == 0:
        return None
    ch, sr, bits = fmt
    byte_rate = sr * ch * max(bits // 8, 1)
    if byte_rate == 0:
        return None
    return (data_bytes / byte_rate, ch, sr, bits)


def opus_estimate(data: bytes, raw_size: int) -> int:
    info = parse_wav_duration(data)
    if info is None:
        # not a parseable WAV (MIDI/CDDA handled by caller); assume 4x as a floor
        return raw_size // 4
    seconds, ch, _sr, _bits = info
    bps = OPUS_BPS_STEREO if ch >= 2 else OPUS_BPS_MONO
    return int(seconds * bps / 8) + 512  # +container overhead


def histogram(path: str) -> dict:
    with open(path, "rb") as f:
        data = f.read()
    buf = io.BytesIO(data)

    sig = kgt._bytes(buf, 16)
    if sig[:7] not in (b"2DKGT2K", b"2DKGT2G"):
        raise ValueError(f"{path}: unsupported signature {sig[:7]!r}")
    _name = kgt._bytes(buf, 256)

    acc = {
        "file_size": len(data),
        "scripts_bytes": 0, "scripts_n": 0,
        "script_items_bytes": 0, "script_items_n": 0,
        "sprite_bytes": 0, "sprite_n": 0,
        "sprite_compressed_bytes": 0, "sprite_compressed_n": 0,
        "sprite_raw_bytes": 0, "sprite_raw_n": 0,
        "palette_bytes": 0, "palette_n": 0,
        "sound_bytes": 0, "sound_n": 0,
        "sound_wave_bytes": 0, "sound_wave_n": 0,
        "sound_midi_bytes": 0, "sound_midi_n": 0,
        "sound_cdda_bytes": 0, "sound_cdda_n": 0,
        # projections
        "proj_sprite_bytes": 0,
        "proj_sound_bytes": 0,
    }

    sc_n = kgt._i32(buf)
    for _ in range(sc_n):
        s = kgt.Script.parse(buf)
        acc["scripts_bytes"] += len(s.pack()); acc["scripts_n"] += 1

    si_n = kgt._i32(buf)
    for _ in range(si_n):
        si = kgt.ScriptItem.parse(buf)
        acc["script_items_bytes"] += len(si.pack()); acc["script_items_n"] += 1

    sf_n = kgt._i32(buf)
    for _ in range(sf_n):
        sf = kgt.SpriteFrame.parse(buf)
        body = len(sf.frame_content)
        acc["sprite_bytes"] += body + 20; acc["sprite_n"] += 1
        if sf.size != 0:
            acc["sprite_compressed_bytes"] += body; acc["sprite_compressed_n"] += 1
        elif body:
            acc["sprite_raw_bytes"] += body; acc["sprite_raw_n"] += 1
        # sprites stay lossless: project zstd-dict factor on the on-disk body
        acc["proj_sprite_bytes"] += int(body * SPRITE_ZSTD_FACTOR)

    for _ in range(8):
        p = kgt.Palette.parse(buf)
        acc["palette_bytes"] += len(p.pack()); acc["palette_n"] += 1

    snd_n = kgt._i32(buf)
    for _ in range(snd_n):
        sd = kgt.Sound.parse(buf)
        body = len(sd.data)
        acc["sound_bytes"] += body + 42; acc["sound_n"] += 1
        low = sd.sound_type & 0x0F
        if low == 1:  # WAVE
            acc["sound_wave_bytes"] += body; acc["sound_wave_n"] += 1
            acc["proj_sound_bytes"] += opus_estimate(sd.data, body)
        elif low == 2:  # MIDI -- already tiny, keep raw
            acc["sound_midi_bytes"] += body; acc["sound_midi_n"] += 1
            acc["proj_sound_bytes"] += body
        elif low == 3:  # CDDA -- track ref, tiny
            acc["sound_cdda_bytes"] += body; acc["sound_cdda_n"] += 1
            acc["proj_sound_bytes"] += body
        else:
            acc["proj_sound_bytes"] += body
    return acc


def print_one(path: str, a: dict) -> None:
    fs = a["file_size"]
    print(f"\n=== {os.path.basename(path)}  ({_mb(fs):.1f} MB) ===")
    rows = [
        ("scripts",      a["scripts_bytes"],      a["scripts_n"]),
        ("script_items", a["script_items_bytes"], a["script_items_n"]),
        ("sprites",      a["sprite_bytes"],        a["sprite_n"]),
        ("  +compressed",a["sprite_compressed_bytes"], a["sprite_compressed_n"]),
        ("  +raw(size=0)",a["sprite_raw_bytes"],   a["sprite_raw_n"]),
        ("palettes",     a["palette_bytes"],       a["palette_n"]),
        ("sounds",       a["sound_bytes"],         a["sound_n"]),
        ("  +WAVE",      a["sound_wave_bytes"],     a["sound_wave_n"]),
        ("  +MIDI",      a["sound_midi_bytes"],     a["sound_midi_n"]),
        ("  +CDDA",      a["sound_cdda_bytes"],     a["sound_cdda_n"]),
    ]
    print(f"  {'class':<16}{'MB':>10}{'% file':>9}{'count':>9}")
    for name, b, n in rows:
        pct = (100.0 * b / fs) if fs else 0.0
        print(f"  {name:<16}{_mb(b):>10.1f}{pct:>8.1f}%{n:>9}")
    proj = a["proj_sprite_bytes"] + a["proj_sound_bytes"] + a["scripts_bytes"] \
        + a["script_items_bytes"] + a["palette_bytes"]
    print(f"  {'-'*42}")
    print(f"  projected packed (pre-dedup): {_mb(proj):.1f} MB "
          f"({100.0*proj/fs:.1f}% of file, {fs/max(proj,1):.1f}x)")
    print(f"    sprites -> {_mb(a['proj_sprite_bytes']):.1f} MB,  "
          f"audio(Opus) -> {_mb(a['proj_sound_bytes']):.1f} MB")


def main(argv):
    if len(argv) < 2:
        print(__doc__); return 2
    total = None
    proj_total = 0
    file_total = 0
    for path in argv[1:]:
        try:
            a = histogram(path)
        except Exception as e:
            sys.stderr.write(f"SKIP {path}: {e}\n")
            continue
        print_one(path, a)
        file_total += a["file_size"]
        proj_total += (a["proj_sprite_bytes"] + a["proj_sound_bytes"]
                       + a["scripts_bytes"] + a["script_items_bytes"]
                       + a["palette_bytes"])
        if total is None:
            total = {k: 0 for k in a}
        for k, v in a.items():
            total[k] += v
    if total is not None and len(argv) > 2:
        print("\n" + "=" * 50)
        print(f"GRAND TOTAL across {len(argv)-1} files: {_mb(file_total):.1f} MB")
        print(f"  WAVE audio:    {_mb(total['sound_wave_bytes']):.1f} MB "
              f"({100.0*total['sound_wave_bytes']/max(file_total,1):.1f}%)")
        print(f"  sprites:       {_mb(total['sprite_bytes']):.1f} MB "
              f"({100.0*total['sprite_bytes']/max(file_total,1):.1f}%)")
        print(f"  projected pack (pre-dedup): {_mb(proj_total):.1f} MB "
              f"({file_total/max(proj_total,1):.1f}x)  [dedup shrinks further]")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
