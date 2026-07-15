#!/usr/bin/env python3
"""fpk.py -- FM2K asset pack (.fpk) packer / reconstructor / verifier.

Packs the common-resource prefix of a .player/.stage/.demo into a slim .fpk:
  - sprites: SOLID zstd (lossless, byte-exact round-trip) -- preserves the ~7.9x
  - audio  : the WAV's PCM payload -> Opus (lossy: 96k stereo / 64k mono);
             the WAV CONTAINER (all header/chunk bytes around the data-chunk
             payload) is stored verbatim. Reconstruction splices decoded PCM
             back into the original container, padded/trimmed to the exact
             original payload length -- so every reconstructed sound is
             BYTE-LENGTH-IDENTICAL and byte-exact outside PCM sample values.
  - everything else (scripts, palettes, per-file tail): zstd, byte-exact

The reconstructor synthesizes the byte stream the engine's player_data_file_loader
would read. This is the pure-Python model of the runtime VFS. The `verify`
command proves the reconstruction is the SAME SIZE as the original and
byte-identical everywhere except PCM sample bytes inside opus-coded sounds,
which is the correctness gate for the whole runtime path.

Format history: FPK2 (current) = exact-length audio splice. FPK1 rewrote
whole WAVs at whatever size/format the decoder produced; the drift (e.g.
Nogaku.player -22840 B across 73 sounds) broke the engine's loader ("Player
Read error" -> half-cleared slot -> heap corruption). FPK1 files can still
be unpacked/inflated, but pack always writes FPK2.

Commands:
    fpk.py pack    IN.player  OUT.fpk   [--level 19] [--bitrate-stereo 96] [--bitrate-mono 64]
    fpk.py unpack  IN.fpk     OUT.bin
    fpk.py verify  IN.player            [--level 19]   # pack->reconstruct->compare
"""
from __future__ import annotations

import argparse
import io
import os
import struct
import subprocess
import sys
import tempfile

import zstandard as zstd
import kgt

MAGIC = b"FPK2"
MAGIC_V1 = b"FPK1"


# ── byte helpers ────────────────────────────────────────────────────────
def _u32(b, o): return struct.unpack_from("<I", b, o)[0]


def w_zblob(out, raw: bytes, level: int):
    c = zstd.ZstdCompressor(level=level).compress(raw)
    out.write(struct.pack("<I", len(c)))
    out.write(c)


def r_zblob(inp) -> bytes:
    (n,) = struct.unpack("<I", inp.read(4))
    return zstd.ZstdDecompressor().decompress(inp.read(n))


def w_raw(out, b: bytes):
    out.write(struct.pack("<I", len(b)))
    out.write(b)


def r_raw(inp) -> bytes:
    (n,) = struct.unpack("<I", inp.read(4))
    return inp.read(n)


# ── WAV / Opus ──────────────────────────────────────────────────────────
def wav_fmt(data: bytes):
    """(channels, sample_rate, bits) from a RIFF/WAVE blob, or None."""
    span = wav_data_span(data)
    return (span[1], span[2], span[3]) if span else None


def wav_data_span(data: bytes):
    """(format_tag, channels, sample_rate, bits, payload_off, payload_len)
    of a RIFF/WAVE blob's fmt + first data chunk, or None. Walks chunks the
    way the engine's ParseRIFFWaveFormat does (bounded by the RIFF size
    field, 2-byte aligned)."""
    if len(data) < 44 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return None
    end = min(8 + _u32(data, 4), len(data))
    pos, fmt, span = 12, None, None
    while pos + 8 <= end:
        cid = data[pos:pos + 4]
        csz = _u32(data, pos + 4)
        if cid == b"fmt " and fmt is None and csz >= 16:
            tag, ch, sr, _br, _ba, bits = struct.unpack_from("<HHIIHH", data, pos + 8)
            fmt = (tag, ch, sr, bits)
        elif cid == b"data" and span is None:
            span = (pos + 8, csz)
        pos += 8 + csz + (csz & 1)
    if fmt is None or span is None or span[0] + span[1] > len(data):
        return None
    return fmt + span


def wav_to_opus(wav: bytes, bitrate_k: int) -> bytes:
    with tempfile.TemporaryDirectory() as td:
        wp, op = os.path.join(td, "i.wav"), os.path.join(td, "o.opus")
        with open(wp, "wb") as f:
            f.write(wav)
        # timeout= is load-bearing: a pathological stream can make libopus spin forever. On timeout
        # subprocess.run raises TimeoutExpired AND kills the ffmpeg child (no orphan zombies), which
        # callers (cc_pool.encode_audio) catch -> raw fallback. Legit encodes finish in well under this.
        subprocess.run(
            ["ffmpeg", "-y", "-loglevel", "error", "-i", wp,
             "-c:a", "libopus", "-b:a", f"{bitrate_k}k", "-vbr", "on",
             "-application", "audio", op],
            check=True, timeout=240)
        with open(op, "rb") as f:
            return f.read()


def opus_to_wav(opus: bytes, sr: int, ch: int, bits: int = 16) -> bytes:
    codec = "pcm_u8" if bits == 8 else "pcm_s16le"
    with tempfile.TemporaryDirectory() as td:
        op, wp = os.path.join(td, "i.opus"), os.path.join(td, "o.wav")
        with open(op, "wb") as f:
            f.write(opus)
        subprocess.run(
            ["ffmpeg", "-y", "-loglevel", "error", "-i", op,
             "-ar", str(sr), "-ac", str(ch), "-c:a", codec, "-f", "wav", wp],
            check=True, timeout=240)
        with open(wp, "rb") as f:
            return f.read()


def opus_to_pcm_payload(opus: bytes, sr: int, ch: int, bits: int,
                        payload_len: int) -> bytes:
    """Decode Opus to raw PCM matching the original fmt, fitted to EXACTLY
    payload_len bytes (Opus pre-skip/padding drifts the sample count by a
    few frames; we trim or pad with digital silence -- 0x80 for 8-bit PCM,
    0x00 otherwise)."""
    wav = opus_to_wav(opus, sr, ch, bits)
    span = wav_data_span(wav)
    if span is None:
        raise ValueError("decoder produced an unreadable WAV")
    off, ln = span[4], span[5]
    pcm = wav[off:off + ln]
    if len(pcm) > payload_len:
        return pcm[:payload_len]
    return pcm + (b"\x80" if bits == 8 else b"\x00") * (payload_len - len(pcm))


# ── parse the common-resource prefix, recording exact byte regions ──────
class Container:
    def __init__(self, data: bytes):
        self.data = data
        buf = io.BytesIO(data)
        sig = kgt._bytes(buf, 16)
        if sig[:7] not in (b"2DKGT2K", b"2DKGT2G"):
            raise ValueError(f"unsupported signature {sig[:7]!r}")
        kgt._bytes(buf, 256)
        self.header = data[0:272]

        p0 = buf.tell()
        sc_n = kgt._i32(buf)
        for _ in range(sc_n):
            kgt.Script.parse(buf)
        self.scripts_region = data[p0:buf.tell()]

        p0 = buf.tell()
        si_n = kgt._i32(buf)
        for _ in range(si_n):
            kgt.ScriptItem.parse(buf)
        self.script_items_region = data[p0:buf.tell()]

        sf_n = kgt._i32(buf)
        self.sprite_count_pos = buf.tell()  # already consumed the count
        self.sprite_headers = []
        self.sprite_contents = []
        for _ in range(sf_n):
            hstart = buf.tell()
            sf = kgt.SpriteFrame.parse(buf)
            # 20-byte header is the first 20 bytes of this frame's region
            self.sprite_headers.append(data[hstart:hstart + 20])
            self.sprite_contents.append(sf.frame_content)
        self.sprite_n = sf_n

        p0 = buf.tell()
        for _ in range(8):
            kgt.Palette.parse(buf)
        self.palettes_region = data[p0:buf.tell()]

        snd_n = kgt._i32(buf)
        self.sounds = []
        for _ in range(snd_n):
            hstart = buf.tell()
            sd = kgt.Sound.parse(buf)
            self.sounds.append((data[hstart:hstart + 42], sd))
        self.sound_n = snd_n

        # everything after the sound table to EOF = opaque per-file tail
        self.tail = data[buf.tell():]


# ── pack ────────────────────────────────────────────────────────────────
def pack(in_path: str, out_path: str, level: int, br_st: int, br_mo: int):
    with open(in_path, "rb") as f:
        c = Container(f.read())

    sprite_contents_blob = b"".join(c.sprite_contents)
    sprite_headers_blob = b"".join(c.sprite_headers)

    stats = {"opus": 0, "opus_in": 0, "raw_audio": 0, "audio_fail": 0}

    out = io.BytesIO()
    out.write(MAGIC)
    out.write(struct.pack("<I", level))
    w_zblob(out, c.header, level)
    w_zblob(out, c.scripts_region, level)
    w_zblob(out, c.script_items_region, level)

    out.write(struct.pack("<I", c.sprite_n))
    w_zblob(out, sprite_headers_blob, level)
    w_zblob(out, sprite_contents_blob, level)   # the big solid win

    w_zblob(out, c.palettes_region, level)

    out.write(struct.pack("<I", c.sound_n))
    for hdr42, sd in c.sounds:
        low = sd.sound_type & 0x0F
        span = wav_data_span(sd.data) if low == 1 else None
        encoded = None
        if span is not None:
            tag, ch, sr, bits, p_off, p_len = span
            # only plain PCM 8/16-bit takes the lossy path; anything odd
            # (ADPCM, float, 24-bit, tiny) passes through verbatim
            if tag == 1 and bits in (8, 16) and p_len > 4096:
                br = br_st if ch >= 2 else br_mo
                try:
                    encoded = wav_to_opus(sd.data, br)
                    stats["opus"] += len(encoded)
                    stats["opus_in"] += len(sd.data)
                except Exception as e:
                    sys.stderr.write(f"  opus encode failed ({e}); storing raw\n")
                    encoded = None
        if encoded is not None:
            out.write(struct.pack("<B", 1))          # codec 1 = opus splice
            out.write(hdr42)
            # payload span + original fmt (decode target)
            out.write(struct.pack("<IIIHH", p_off, p_len, sr, ch, bits))
            # original WAV minus the PCM payload, verbatim (split at p_off)
            w_zblob(out, sd.data[:p_off] + sd.data[p_off + p_len:], level)
            w_raw(out, encoded)
        else:
            out.write(struct.pack("<B", 0))          # codec 0 = raw passthrough
            out.write(hdr42)
            w_raw(out, sd.data)
            stats["raw_audio"] += len(sd.data)

    w_zblob(out, c.tail, level)

    blob = out.getvalue()
    with open(out_path, "wb") as f:
        f.write(blob)

    orig = len(c.data)
    print(f"  {os.path.basename(in_path)}: {orig/1048576:.1f} MB -> "
          f"{len(blob)/1048576:.1f} MB  ({orig/len(blob):.2f}x)")
    if stats["opus_in"]:
        print(f"    audio: {stats['opus_in']/1048576:.1f} MB WAVE -> "
              f"{stats['opus']/1048576:.1f} MB Opus "
              f"({stats['opus_in']/max(stats['opus'],1):.1f}x)")
    return blob


# ── reconstruct (the runtime-VFS model) ────────────────────────────────
def reconstruct(fpk: bytes) -> bytes:
    inp = io.BytesIO(fpk)
    magic = inp.read(4)
    if magic not in (MAGIC, MAGIC_V1):
        raise ValueError("not an .fpk")
    v1 = magic == MAGIC_V1
    (_level,) = struct.unpack("<I", inp.read(4))

    out = io.BytesIO()
    out.write(r_zblob(inp))                       # header
    out.write(r_zblob(inp))                       # scripts region
    out.write(r_zblob(inp))                       # script_items region

    (sprite_n,) = struct.unpack("<I", inp.read(4))
    headers_blob = r_zblob(inp)
    contents_blob = r_zblob(inp)
    out.write(struct.pack("<i", sprite_n))
    cpos = 0
    for i in range(sprite_n):
        hdr = headers_blob[i * 20:(i + 1) * 20]
        _uf, w, h, hpp, size = struct.unpack("<iiiii", hdr)
        if size == 0:
            t = w * h
            n = (t + (1024 if hpp else 0)) if t > 0 else 0
        else:
            n = size
        out.write(hdr)
        out.write(contents_blob[cpos:cpos + n])
        cpos += n

    out.write(r_zblob(inp))                       # palettes region

    (sound_n,) = struct.unpack("<I", inp.read(4))
    out.write(struct.pack("<i", sound_n))
    for _ in range(sound_n):
        (codec,) = struct.unpack("<B", inp.read(1))
        hdr42 = inp.read(42)
        if codec == 1 and v1:
            # legacy FPK1: whole-WAV rewrite; size drifts (known-broken for
            # the engine, kept only so old .fpk files remain inspectable)
            sr, ch = struct.unpack("<IH", inp.read(6))
            opus = r_raw(inp)
            wav = opus_to_wav(opus, sr, ch)
            hdr = bytearray(hdr42)
            struct.pack_into("<i", hdr, 36, len(wav))
            out.write(bytes(hdr))
            out.write(wav)
        elif codec == 1:
            p_off, p_len, sr, ch, bits = struct.unpack("<IIIHH", inp.read(16))
            container = r_zblob(inp)
            opus = r_raw(inp)
            pcm = opus_to_pcm_payload(opus, sr, ch, bits, p_len)
            out.write(hdr42)  # verbatim: size field is the original size
            out.write(container[:p_off])
            out.write(pcm)
            out.write(container[p_off:])
        else:
            data = r_raw(inp)
            out.write(hdr42)
            out.write(data)

    out.write(r_zblob(inp))                       # tail
    return out.getvalue()


# ── verify: pack -> reconstruct -> byte-compare the lossless regions ────
def verify(in_path: str, level: int):
    with open(in_path, "rb") as f:
        orig_data = f.read()
    src = Container(orig_data)

    with tempfile.NamedTemporaryFile(suffix=".fpk", delete=False) as tf:
        fpk_path = tf.name
    try:
        blob = pack(in_path, fpk_path, level, 96, 64)
        recon = reconstruct(blob)
    finally:
        os.unlink(fpk_path)

    rc = Container(recon)

    def eq(name, a, b, hard):
        ok = (a == b)
        flag = "OK " if ok else ("FAIL" if hard else "diff")
        extra = "" if ok else f"  ({len(a)} vs {len(b)} B)"
        print(f"  [{flag}] {name}{extra}")
        return ok or not hard

    print(f"\n=== verify {os.path.basename(in_path)} ===")
    allok = True
    allok &= eq("header (sig+name)", src.header, rc.header, True)
    allok &= eq("scripts region", src.scripts_region, rc.scripts_region, True)
    allok &= eq("script_items region", src.script_items_region, rc.script_items_region, True)
    # sprites: the determinism-critical, must be byte-exact
    sp_ok = (src.sprite_n == rc.sprite_n
             and src.sprite_headers == rc.sprite_headers
             and src.sprite_contents == rc.sprite_contents)
    print(f"  [{'OK ' if sp_ok else 'FAIL'}] sprites byte-exact "
          f"({src.sprite_n} frames, {sum(len(x) for x in src.sprite_contents)/1048576:.1f} MB)")
    allok &= sp_ok
    allok &= eq("palettes region", src.palettes_region, rc.palettes_region, True)
    allok &= eq("tail region", src.tail, rc.tail, True)

    # audio HARD gate: every sound must be byte-length-identical with an
    # identical 42B header, and byte-exact outside the PCM payload of
    # opus-coded sounds (raw-codec sounds must be fully identical)
    aud_ok = (src.sound_n == rc.sound_n)
    lossy = 0
    for i, ((sh, ss), (rh, rs)) in enumerate(zip(src.sounds, rc.sounds)):
        if sh != rh or len(ss.data) != len(rs.data):
            print(f"  [FAIL] sound[{i}]: header/size drift "
                  f"({len(ss.data)} vs {len(rs.data)} B)")
            aud_ok = False
            continue
        if ss.data == rs.data:
            continue
        span = wav_data_span(ss.data)
        if span is None:
            print(f"  [FAIL] sound[{i}]: bytes differ on a non-spliceable sound")
            aud_ok = False
            continue
        p_off, p_len = span[4], span[5]
        if (ss.data[:p_off] != rs.data[:p_off]
                or ss.data[p_off + p_len:] != rs.data[p_off + p_len:]):
            print(f"  [FAIL] sound[{i}]: container bytes differ outside "
                  f"the PCM payload")
            aud_ok = False
        else:
            lossy += 1
    size_ok = len(orig_data) == len(recon)
    print(f"  [{'OK ' if aud_ok else 'FAIL'}] audio: {src.sound_n} sounds, "
          f"{lossy} lossy PCM payloads, containers/headers/lengths exact")
    print(f"  [{'OK ' if size_ok else 'FAIL'}] total size: "
          f"{len(orig_data)} -> {len(recon)} bytes")
    allok &= aud_ok and size_ok

    verdict = "ALL PASS -- runtime VFS path is byte-safe" if allok else "FAILURES ABOVE"
    print(f"  => {verdict}")
    return allok


def main(argv):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack"); p.add_argument("inp"); p.add_argument("out")
    p.add_argument("--level", type=int, default=19)
    p.add_argument("--bitrate-stereo", type=int, default=96)
    p.add_argument("--bitrate-mono", type=int, default=64)
    u = sub.add_parser("unpack"); u.add_argument("inp"); u.add_argument("out")
    v = sub.add_parser("verify"); v.add_argument("inp"); v.add_argument("--level", type=int, default=19)
    a = ap.parse_args(argv[1:])

    if a.cmd == "pack":
        pack(a.inp, a.out, a.level, a.bitrate_stereo, a.bitrate_mono)
    elif a.cmd == "unpack":
        with open(a.inp, "rb") as f:
            recon = reconstruct(f.read())
        with open(a.out, "wb") as f:
            f.write(recon)
        print(f"  reconstructed {len(recon)/1048576:.1f} MB -> {a.out}")
    elif a.cmd == "verify":
        ok = verify(a.inp, a.level)
        return 0 if ok else 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
