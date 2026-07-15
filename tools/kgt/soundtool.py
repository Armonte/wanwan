#!/usr/bin/env python3
"""soundtool.py -- list / extract / replace sounds in FM2K files.

Works on .player / .stage / .demo / .kgt (FM2K family: `2DKGT2K`/`2DKGT2G`).
Made for modders: scripts reference sounds by INDEX into the flat sounds[]
table, so replacing a sound's payload in place never touches a script block.

    soundtool.py list    Char.player
    soundtool.py extract Char.player -o out_dir/            # every sound
    soundtool.py extract Char.player --sound 7 -o hit.wav   # one sound
    soundtool.py replace Char.player --sound 7 new_hit.wav          # in-place (+ .bak)
    soundtool.py replace Char.player --sound "punch1" new.wav -o Out.player

`--sound` takes an index (from `list`) or a name; names may repeat, so the
index is the unambiguous form.

Replacement WAVs are validated against the ENGINE'S actual RIFF walker
(ParseRIFFWaveFormat @0x416043 in WonderfulWorld_ver_0946, == closercombat
iWalkWavFile): RIFF/WAVE magic, a truthful RIFF size field (the engine uses
it as its walk bound -- an overstated value makes the game read out of
bounds), a `fmt ` chunk >= 14 bytes, and a `data` chunk. The fmt chunk is
handed raw to DirectSound CreateSoundBuffer, so we additionally require
PCM, 8/16-bit, 1-2 channels (override with --force; the sound will likely
just not play). MIDI slots accept .mid files (MThd check only).

Safety: before editing, the file is round-trip verified byte-exact; after
editing, the output is re-parsed and every untouched record is compared
byte-for-byte. If either check fails, nothing is written / the exact
difference is reported.
"""
from __future__ import annotations

import argparse
import os
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kgt
import fm2nd
from kgt import Sound, _try_decode

SOUND_TYPE_NAMES = {0: "stop-all", 1: "WAV", 2: "MIDI", 3: "CD"}
SOUND_EXT = {1: "wav", 2: "mid", 3: "bin"}


# ── file container (dispatch .kgt vs .player/.stage/.demo) ─────────────

class Container:
    """Uniform view over a parsed FM2K file: .sounds list + repack()."""

    def __init__(self, path: str):
        self.path = path
        with open(path, "rb") as f:
            self.orig = f.read()
        ext = os.path.splitext(path)[1].lower().lstrip(".")
        if fm2nd._is_fm95(self.orig):
            raise SystemExit(
                f"{path}: FM95-family file (KGTGAME/2DKGT95 signature); "
                f"sound editing is only supported for FM2K-family files")
        if ext == "kgt":
            self.kind = "kgt"
            self.obj = kgt.Kgt.parse(self.orig)
            self.sounds = self.obj.sounds
        elif ext in ("player", "stage", "demo"):
            self.kind, self.obj = fm2nd.parse_file(path)
            self.sounds = self.obj.header.sounds
        else:
            raise SystemExit(
                f"{path}: unknown extension {ext!r}; "
                f"expected .kgt/.player/.stage/.demo")

    def repack(self) -> bytes:
        return self.obj.pack()


# ── WAV validation (mirror of the engine's ParseRIFFWaveFormat) ────────

def walk_wav(data: bytes):
    """Chunk-walk `data` exactly like the engine does.

    Returns (info, fatal, warnings):
      info     -- dict with fmt fields + data size (None if unreadable)
      fatal    -- problems that make the engine reject/misread the file
      warnings -- format oddities CreateSoundBuffer will likely refuse
    """
    fatal, warnings = [], []
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return None, ["not a RIFF/WAVE file"], []
    riff_size = struct.unpack_from("<I", data, 4)[0]
    if riff_size + 8 > len(data):
        fatal.append(
            f"RIFF size field ({riff_size}) overstates the file "
            f"({len(data) - 8} bytes follow the header): the engine trusts "
            f"this field as its walk bound and would read out of bounds "
            f"in-game -- re-export the WAV")
    end = min(8 + riff_size, len(data))
    pos, fmt, data_size = 12, None, None
    while pos + 8 <= end:
        cid = data[pos:pos + 4]
        csize = struct.unpack_from("<I", data, pos + 4)[0]
        body = pos + 8
        if cid == b"fmt " and fmt is None:
            if csize < 14:
                fatal.append(f"fmt chunk is {csize} bytes; engine requires >= 14")
                break
            tag, ch, rate = struct.unpack_from("<HHI", data, body)
            bits = struct.unpack_from("<H", data, body + 14)[0] if csize >= 16 else None
            fmt = {"format_tag": tag, "channels": ch,
                   "sample_rate": rate, "bits": bits}
        elif cid == b"data" and data_size is None:
            data_size = csize
            if body + csize > end:
                fatal.append(
                    f"data chunk ({csize} bytes at offset {body}) runs past "
                    f"the RIFF bound; the engine would copy garbage/OOB")
        pos = body + ((csize + 1) & ~1)
    if fmt is None:
        fatal.append("no fmt chunk found within the RIFF bound")
    if data_size is None:
        fatal.append("no data chunk found within the RIFF bound "
                     "(engine treats the WAV as unplayable)")
    info = dict(fmt or {}, data_size=data_size)
    if fmt:
        if fmt["format_tag"] != 1:
            warnings.append(
                f"format tag {fmt['format_tag']} is not PCM (1); the engine "
                f"passes fmt straight to DirectSound and the sound will "
                f"likely not play")
        if fmt["bits"] not in (8, 16):
            warnings.append(f"{fmt['bits']}-bit samples; DirectSound secondary "
                            f"buffers want 8 or 16-bit")
        if fmt["channels"] not in (1, 2):
            warnings.append(f"{fmt['channels']} channels; expected mono or stereo")
    return info, fatal, warnings


def fmt_desc(info) -> str:
    if not info or "channels" not in info:
        return "?"
    ch = {1: "mono", 2: "stereo"}.get(info["channels"], f"{info['channels']}ch")
    bits = f"{info['bits']}-bit" if info.get("bits") else "?-bit"
    tag = "PCM" if info.get("format_tag") == 1 else f"tag={info.get('format_tag')}"
    return f"{tag} {info.get('sample_rate', '?')}Hz {bits} {ch}"


# ── sound selection ─────────────────────────────────────────────────────

def sound_name(s: Sound) -> str:
    return _try_decode(s.name).strip()


def resolve_sound(sounds, key: str) -> int:
    """`key` is an index or a name. Names: exact match first, then unique
    case-insensitive substring; ambiguity is an error listing candidates."""
    try:
        idx = int(key)
        if not 0 <= idx < len(sounds):
            raise SystemExit(f"sound index {idx} out of range 0..{len(sounds) - 1}")
        return idx
    except ValueError:
        pass
    exact = [i for i, s in enumerate(sounds) if sound_name(s) == key]
    if len(exact) == 1:
        return exact[0]
    if len(exact) > 1:
        raise SystemExit(
            f"name {key!r} matches {len(exact)} sounds "
            f"(indices {exact}); use the index")
    sub = [i for i, s in enumerate(sounds) if key.lower() in sound_name(s).lower()]
    if len(sub) == 1:
        return sub[0]
    if sub:
        cands = ", ".join(f"{i}:{sound_name(sounds[i])!r}" for i in sub[:10])
        raise SystemExit(f"name {key!r} is ambiguous: {cands}; use the index")
    raise SystemExit(f"no sound named {key!r}; run `soundtool.py list` to see names")


# ── commands ────────────────────────────────────────────────────────────

def cmd_list(args):
    c = Container(args.input)
    print(f"{args.input}: {len(c.sounds)} sound slots")
    print(f"{'idx':>4}  {'type':<8} {'bytes':>9}  {'format':<28} name")
    for i, s in enumerate(c.sounds):
        if s.size == 0 and not sound_name(s):
            continue
        t = SOUND_TYPE_NAMES.get(s.sound_type & 0xF, f"?{s.sound_type & 0xF}")
        if s.sound_type & 0x10:
            t += "+loop"
        desc, mark = "", ""
        if (s.sound_type & 0xF) == 1 and s.size:
            info, fatal, warn = walk_wav(s.data)
            desc = fmt_desc(info)
            if fatal:
                mark = "  !! " + "; ".join(fatal)
            elif warn:
                mark = "  ! " + "; ".join(warn)
        print(f"{i:>4}  {t:<8} {s.size:>9}  {desc:<28} {sound_name(s)}{mark}")


def cmd_extract(args):
    c = Container(args.input)
    if args.sound is not None:
        i = resolve_sound(c.sounds, args.sound)
        s = c.sounds[i]
        if s.size == 0:
            raise SystemExit(f"sound {i} ({sound_name(s)!r}) is empty")
        out = args.output or f"{i:04d}_{sound_name(s) or 'sound'}." + \
            SOUND_EXT.get(s.sound_type & 0xF, "bin")
        with open(out, "wb") as f:
            f.write(s.data)
        print(f"wrote {s.size} bytes to {out}")
        return
    outdir = args.output or os.path.splitext(args.input)[0] + "_sounds"
    os.makedirs(outdir, exist_ok=True)
    n = 0
    for i, s in enumerate(c.sounds):
        if s.size == 0:
            continue
        ext = SOUND_EXT.get(s.sound_type & 0xF, "bin")
        name = sound_name(s).replace("/", "_").replace("\\", "_") or f"sound{i}"
        with open(os.path.join(outdir, f"{i:04d}_{name}.{ext}"), "wb") as f:
            f.write(s.data)
        n += 1
    print(f"extracted {n} sounds to {outdir}/")


def cmd_replace(args):
    c = Container(args.input)

    # gate 0: the file itself must round-trip byte-exact BEFORE we touch it
    if c.repack() != c.orig:
        raise SystemExit(
            f"{args.input} does not round-trip byte-exact through the parser; "
            f"refusing to edit (this protects you from silent corruption -- "
            f"please report this file so we can fix the parser)")

    i = resolve_sound(c.sounds, args.sound)
    s = c.sounds[i]
    slot_type = s.sound_type & 0xF
    with open(args.new, "rb") as f:
        new = f.read()

    # gate 1: validate the replacement against the slot's type
    if slot_type == 1:
        info, fatal, warnings = walk_wav(new)
        for w in warnings:
            print(f"WARN {args.new}: {w}", file=sys.stderr)
        if fatal:
            for p in fatal:
                print(f"FATAL {args.new}: {p}", file=sys.stderr)
            raise SystemExit("replacement WAV would break in-engine; aborting")
        if warnings and not args.force:
            raise SystemExit("replacement WAV has format problems (see WARN "
                             "above); use --force to write it anyway")
        old_info, _, _ = walk_wav(s.data) if s.size else (None, None, None)
        print(f"replacing sound {i} {sound_name(s)!r}: "
              f"{fmt_desc(old_info)} ({s.size} B) -> {fmt_desc(info)} ({len(new)} B)")
    elif slot_type == 2:
        if new[:4] != b"MThd":
            raise SystemExit(f"{args.new}: slot {i} is MIDI but the file has "
                             f"no MThd header")
        print(f"replacing MIDI sound {i} {sound_name(s)!r}: "
              f"{s.size} B -> {len(new)} B")
    else:
        raise SystemExit(
            f"sound {i} is type {SOUND_TYPE_NAMES.get(slot_type, slot_type)!r}; "
            f"only WAV and MIDI slots can be replaced")

    old_payloads = [snd.data for snd in c.sounds]
    s.data, s.size = new, len(new)
    packed = c.repack()

    # gate 2: re-parse the output and prove only the target sound changed
    check = Container.__new__(Container)
    check.path, check.orig = "<memory>", packed
    if c.kind == "kgt":
        check.obj = kgt.Kgt.parse(packed)
        check.sounds = check.obj.sounds
    else:
        check.obj = fm2nd._PARSERS[c.kind].parse(packed)
        check.sounds = check.obj.header.sounds
    assert len(check.sounds) == len(old_payloads)
    for j, snd in enumerate(check.sounds):
        want = new if j == i else old_payloads[j]
        if snd.data != want:
            raise SystemExit(f"self-verify FAILED at sound {j}; not writing")
    if check.obj.pack() != packed:
        raise SystemExit("self-verify FAILED (output not stable); not writing")

    out = args.output or args.input
    if out == args.input:
        bak = args.input + ".bak"
        if not os.path.exists(bak):
            shutil.copyfile(args.input, bak)
            print(f"backup saved to {bak}")
        else:
            print(f"backup {bak} already exists; keeping it (it is the "
                  f"oldest original)")
    with open(out, "wb") as f:
        f.write(packed)
    print(f"OK wrote {len(packed)} bytes to {out} "
          f"(self-verified: only sound {i} changed)")


def main():
    ap = argparse.ArgumentParser(
        description="List / extract / replace sounds in FM2K "
                    ".player/.stage/.demo/.kgt files without touching "
                    "script blocks.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("list", help="show every sound slot with format info")
    p.add_argument("input")
    p.set_defaults(fn=cmd_list)

    p = sub.add_parser("extract", help="dump sounds to WAV/MID files")
    p.add_argument("input")
    p.add_argument("--sound", help="index or name (default: all sounds)")
    p.add_argument("-o", "--output",
                   help="output file (with --sound) or directory")
    p.set_defaults(fn=cmd_extract)

    p = sub.add_parser("replace",
                       help="swap one sound's audio; validates the WAV "
                            "against the engine's real RIFF parser")
    p.add_argument("input")
    p.add_argument("--sound", required=True, help="index or name")
    p.add_argument("new", help="replacement .wav (or .mid for MIDI slots)")
    p.add_argument("-o", "--output",
                   help="output file (default: in-place, keeps a .bak)")
    p.add_argument("--force", action="store_true",
                   help="write despite non-PCM/odd-format warnings")
    p.set_defaults(fn=cmd_replace)

    args = ap.parse_args()
    sys.exit(args.fn(args) or 0)


if __name__ == "__main__":
    main()
