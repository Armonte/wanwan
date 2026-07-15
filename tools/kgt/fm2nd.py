"""Byte-for-byte round-trip parser for FM2K .player / .stage / .demo files.

Companion to kgt.py — reuses the common-resource record dataclasses
(Script, ScriptItem, SpriteFrame, Palette, Sound) and adds the
per-file-type tail logic from the 010 Editor templates in
Unity2dfmRuntime/Docs/2dfm-{player,stage,demo}.bt.

All three formats share the same 16B `2DKGT2K\\0` signature as .kgt
(no format-specific magic). Detection is by file extension.

CLI mirrors kgt.py: `parse / pack / verify / info`. Block-decode is
shared with kgt.py (use --decode-blocks for the typed Block view).
"""
from __future__ import annotations

import argparse
import base64
import io
import json
import os
import struct
import sys
from dataclasses import dataclass, field
from typing import List

# Reuse common-resource dataclasses and helpers from kgt.py
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kgt import (
    Script, ScriptItem, SpriteFrame, Palette, Sound,
    _u8, _u16, _i16, _u32, _i32, _bytes, _pu8, _pu16, _pi16, _pu32, _pi32,
    _try_decode,
)


# ─── Common-resource header ──────────────────────────────────────────────

# All FM2K project-resource files (.kgt/.player/.stage/.demo) share an
# identical 16B + 256B + variable-length common section. Factor it out
# so the three new formats stay one-screen each.

@dataclass
class CommonHeader:
    file_signature: bytes       # 16B
    project_name: bytes         # 256B
    scripts: List[Script]
    script_items: List[ScriptItem]
    sprite_frames: List[SpriteFrame]
    palettes: List[Palette]     # always 8
    sounds: List[Sound]

    @classmethod
    def parse(cls, buf: io.BytesIO) -> "CommonHeader":
        sig = _bytes(buf, 16)
        sig_str = sig[:7]
        if sig_str in (b"KGTGAME", b"2DKGT95"):
            raise NotImplementedError(
                f"FM95 format detected (signature={sig_str!r}); "
                "this parser only handles FM2K (2DKGT2K / 2DKGT2G).")
        if sig_str not in (b"2DKGT2K", b"2DKGT2G"):
            raise ValueError(
                f"unknown FM2K signature {sig_str!r}; "
                "supported: 2DKGT2K, 2DKGT2G")
        proj = _bytes(buf, 256)
        sc_n = _i32(buf)
        scripts = [Script.parse(buf) for _ in range(sc_n)]
        si_n = _i32(buf)
        script_items = [ScriptItem.parse(buf) for _ in range(si_n)]
        sf_n = _i32(buf)
        sprite_frames = [SpriteFrame.parse(buf) for _ in range(sf_n)]
        palettes = [Palette.parse(buf) for _ in range(8)]
        snd_n = _i32(buf)
        sounds = [Sound.parse(buf) for _ in range(snd_n)]
        return cls(sig, proj, scripts, script_items,
                   sprite_frames, palettes, sounds)

    def pack(self) -> bytes:
        out = io.BytesIO()
        out.write(self.file_signature)
        out.write(self.project_name)
        out.write(_pi32(len(self.scripts)))
        for s in self.scripts: out.write(s.pack())
        out.write(_pi32(len(self.script_items)))
        for si in self.script_items: out.write(si.pack())
        out.write(_pi32(len(self.sprite_frames)))
        for sf in self.sprite_frames: out.write(sf.pack())
        assert len(self.palettes) == 8
        for p in self.palettes: out.write(p.pack())
        out.write(_pi32(len(self.sounds)))
        for s in self.sounds: out.write(s.pack())
        return out.getvalue()

    def to_json(self) -> dict:
        return {
            "file_signature": self.file_signature.hex(),
            "project_name": self.project_name.hex(),
            "project_name_str": _try_decode(self.project_name),
            "scripts": [
                {"name": s.script_name.hex(),
                 "name_str": _try_decode(s.script_name),
                 "script_index": s.script_index,
                 "unknown_flag1": s.unknown_flag1,
                 "special_flag": s.special_flag}
                for s in self.scripts],
            "script_items": [
                {"type": si.script_type, "payload": si.payload.hex()}
                for si in self.script_items],
            "sprite_frames": [
                {"unknown_flag1": sf.unknown_flag1,
                 "width": sf.width, "height": sf.height,
                 "has_private_palette": sf.has_private_palette,
                 "size": sf.size,
                 "frame_content_b64":
                     base64.b64encode(sf.frame_content).decode()}
                for sf in self.sprite_frames],
            "palettes": [
                {"colors": p.colors.hex(), "gap": p.gap.hex()}
                for p in self.palettes],
            "sounds": [
                {"unknown1": s.unknown1, "name": s.name.hex(),
                 "name_str": _try_decode(s.name),
                 "size": s.size, "sound_type": s.sound_type,
                 "sound_track": s.sound_track,
                 "data_b64": base64.b64encode(s.data).decode()}
                for s in self.sounds],
        }

    @classmethod
    def from_json(cls, d: dict) -> "CommonHeader":
        return cls(
            file_signature=bytes.fromhex(d["file_signature"]),
            project_name=bytes.fromhex(d["project_name"]),
            scripts=[Script(
                script_name=bytes.fromhex(s["name"]),
                script_index=s["script_index"],
                unknown_flag1=s["unknown_flag1"],
                special_flag=s["special_flag"]) for s in d["scripts"]],
            script_items=[ScriptItem(
                script_type=si["type"],
                payload=bytes.fromhex(si["payload"]))
                for si in d["script_items"]],
            sprite_frames=[SpriteFrame(
                unknown_flag1=sf["unknown_flag1"], width=sf["width"],
                height=sf["height"],
                has_private_palette=sf["has_private_palette"],
                size=sf["size"],
                frame_content=base64.b64decode(sf["frame_content_b64"]))
                for sf in d["sprite_frames"]],
            palettes=[Palette(
                colors=bytes.fromhex(p["colors"]),
                gap=bytes.fromhex(p["gap"])) for p in d["palettes"]],
            sounds=[Sound.from_dict(s, i) for i, s in enumerate(d["sounds"])],
        )


# ─── FM95 opaque-blob carrier ────────────────────────────────────────────

# FM95 (signatures `KGTGAME\0`, `2DKGT95\0`) is the predecessor format.
# Its .kgt is a fixed 0x78D48-byte block per FM2K_KgtParser.cpp lines
# 200-228; its .player/.demo are variable-size with an undocumented
# layout (no 010 template, no Unity reader). We don't have a public
# byte-level spec for FM95, so we round-trip everything past the 272-
# byte header (16B signature + 256B project_name) as opaque bytes.
# This gives full corpus round-trip coverage at the cost of zero
# semantic decode for FM95.

@dataclass
class Fm95Opaque:
    """Generic FM95 carrier: 16B signature + 256B name + opaque body.
    Used for FM95 .kgt / .player / .demo when full decoding isn't
    needed (or hasn't been done yet)."""
    file_signature: bytes      # 16B
    project_name: bytes        # 256B
    body: bytes = b""          # everything else
    _format_label: str = "fm95"

    @classmethod
    def parse(cls, data: bytes, label: str = "fm95") -> "Fm95Opaque":
        if len(data) < 272:
            raise ValueError(f"FM95 file too short: {len(data)} bytes")
        sig = data[:16]
        sig_str = sig[:7]
        if sig_str not in (b"KGTGAME", b"2DKGT95"):
            raise ValueError(
                f"not an FM95 signature: {sig_str!r}")
        return cls(file_signature=sig,
                   project_name=data[16:272],
                   body=data[272:],
                   _format_label=label)

    def pack(self) -> bytes:
        return self.file_signature + self.project_name + self.body

    def to_json(self) -> dict:
        return {
            "_format": self._format_label,
            "file_signature": self.file_signature.hex(),
            "project_name": self.project_name.hex(),
            "project_name_str": _try_decode(self.project_name),
            "body_b64": base64.b64encode(self.body).decode(),
            "body_size": len(self.body),
        }

    @classmethod
    def from_json(cls, d: dict) -> "Fm95Opaque":
        return cls(
            file_signature=bytes.fromhex(d["file_signature"]),
            project_name=bytes.fromhex(d["project_name"]),
            body=base64.b64decode(d["body_b64"]),
            _format_label=d.get("_format", "fm95"))


# ─── .player ─────────────────────────────────────────────────────────────

@dataclass
class Player:
    """FM2K character file. 2dfm-player.bt incorrectly documents the
    tail as a single byte; actual .player tails are tens of thousands
    of bytes of variable-size character-specific data (animations,
    hitbox layouts, AI settings — none of which is currently decoded).

    We round-trip the tail as opaque bytes. The shipped Unity reader
    (PlayerFileReader.cs) only reads the common-resource section
    too; nobody has documented the player-specific tail yet."""
    header: CommonHeader
    tail: bytes = b""

    @classmethod
    def parse(cls, data: bytes) -> "Player":
        buf = io.BytesIO(data)
        h = CommonHeader.parse(buf)
        tail = buf.read()
        return cls(header=h, tail=tail)

    def pack(self) -> bytes:
        return self.header.pack() + self.tail

    def to_json(self) -> dict:
        d = self.header.to_json()
        d["tail_b64"] = base64.b64encode(self.tail).decode()
        d["tail_size"] = len(self.tail)
        d["_format"] = "player"
        return d

    @classmethod
    def from_json(cls, d: dict) -> "Player":
        return cls(header=CommonHeader.from_json(d),
                   tail=base64.b64decode(d["tail_b64"]))


# ─── .stage ──────────────────────────────────────────────────────────────

@dataclass
class Stage:
    """FM2K stage file. Per 2dfm-stage.bt the tail is `int unknwnGap`
    + `int bgmSoundId`, followed by 1029 bytes of zero padding to
    reach a fixed 1037-byte trailing region. bgmSoundId indexes the
    sounds[] block above and selects which sound to use for stage BGM.
    Confirmed across our corpus that bytes 8..1037 of the trailing
    region are always zero."""
    header: CommonHeader
    unknown_gap: int = 0        # int32, always 0 in observed corpus
    bgm_sound_id: int = 0       # int32 — index into sounds[]
    reserved_tail: bytes = b""  # 1029B of zero padding

    @classmethod
    def parse(cls, data: bytes) -> "Stage":
        buf = io.BytesIO(data)
        h = CommonHeader.parse(buf)
        ug = _i32(buf)
        bgm = _i32(buf)
        tail = buf.read()
        return cls(header=h, unknown_gap=ug, bgm_sound_id=bgm,
                   reserved_tail=tail)

    def pack(self) -> bytes:
        return (self.header.pack()
                + _pi32(self.unknown_gap)
                + _pi32(self.bgm_sound_id)
                + self.reserved_tail)

    def to_json(self) -> dict:
        d = self.header.to_json()
        d["unknown_gap"] = self.unknown_gap
        d["bgm_sound_id"] = self.bgm_sound_id
        d["reserved_tail"] = self.reserved_tail.hex()
        d["_format"] = "stage"
        return d

    @classmethod
    def from_json(cls, d: dict) -> "Stage":
        return cls(
            header=CommonHeader.from_json(d),
            unknown_gap=d["unknown_gap"],
            bgm_sound_id=d["bgm_sound_id"],
            reserved_tail=bytes.fromhex(d["reserved_tail"]))


# ─── .demo ───────────────────────────────────────────────────────────────

@dataclass
class Demo:
    """FM2K demo (cutscene/intro/CSS layout) file. Per 2dfm-demo.bt the
    tail is: int unknownGap1, short bgmSoundId, byte isSkipByAnyKey,
    short unknownGap2, uint totalTime — 13 bytes — followed by 1024
    bytes of zero padding to reach a fixed 1037-byte trailing region."""
    header: CommonHeader
    unknown_gap_1: int = 0          # int32
    bgm_sound_id: int = 0           # int16
    is_skip_by_any_key: int = 0     # uint8 (boolean)
    unknown_gap_2: int = 0          # int16
    total_time: int = 0             # uint32 (frames or ms — unverified)
    reserved_tail: bytes = b""      # 1024B of zero padding

    @classmethod
    def parse(cls, data: bytes) -> "Demo":
        buf = io.BytesIO(data)
        h = CommonHeader.parse(buf)
        ug1 = _i32(buf)
        bgm = _i16(buf)
        skip = _u8(buf)
        ug2 = _i16(buf)
        total = _u32(buf)
        tail = buf.read()
        return cls(header=h,
                   unknown_gap_1=ug1, bgm_sound_id=bgm,
                   is_skip_by_any_key=skip,
                   unknown_gap_2=ug2, total_time=total,
                   reserved_tail=tail)

    def pack(self) -> bytes:
        return (self.header.pack()
                + _pi32(self.unknown_gap_1)
                + _pi16(self.bgm_sound_id)
                + _pu8(self.is_skip_by_any_key)
                + _pi16(self.unknown_gap_2)
                + _pu32(self.total_time)
                + self.reserved_tail)

    def to_json(self) -> dict:
        d = self.header.to_json()
        d["unknown_gap_1"] = self.unknown_gap_1
        d["bgm_sound_id"] = self.bgm_sound_id
        d["is_skip_by_any_key"] = self.is_skip_by_any_key
        d["unknown_gap_2"] = self.unknown_gap_2
        d["total_time"] = self.total_time
        d["reserved_tail"] = self.reserved_tail.hex()
        d["_format"] = "demo"
        return d

    @classmethod
    def from_json(cls, d: dict) -> "Demo":
        return cls(
            header=CommonHeader.from_json(d),
            unknown_gap_1=d["unknown_gap_1"],
            bgm_sound_id=d["bgm_sound_id"],
            is_skip_by_any_key=d["is_skip_by_any_key"],
            unknown_gap_2=d["unknown_gap_2"],
            total_time=d["total_time"],
            reserved_tail=bytes.fromhex(d["reserved_tail"]))


# ─── Dispatch ────────────────────────────────────────────────────────────

_PARSERS = {
    "player": Player,
    "stage":  Stage,
    "demo":   Demo,
}


def detect_type(path: str) -> str:
    ext = os.path.splitext(path)[1].lower().lstrip(".")
    if ext not in _PARSERS:
        raise ValueError(
            f"unknown extension {ext!r}; expected .player/.stage/.demo")
    return ext


def _is_fm95(data: bytes) -> bool:
    return len(data) >= 7 and data[:7] in (b"KGTGAME", b"2DKGT95")


def parse_file(path: str):
    t = detect_type(path)
    with open(path, "rb") as f:
        data = f.read()
    if _is_fm95(data):
        return f"{t}-fm95", Fm95Opaque.parse(data, label=f"{t}-fm95")
    return t, _PARSERS[t].parse(data)


# ─── CLI ─────────────────────────────────────────────────────────────────

def cmd_parse(args):
    t, obj = parse_file(args.input)
    j = obj.to_json()
    if args.decode_blocks:
        import blocks
        ranges = []
        for i, sc in enumerate(obj.header.scripts):
            start = sc.script_index
            end = (obj.header.scripts[i + 1].script_index
                   if i + 1 < len(obj.header.scripts)
                   else len(obj.header.script_items))
            ranges.append((start, end))
        is_settings = [False] * len(obj.header.script_items)
        for (start, end) in ranges:
            if 0 <= start < len(obj.header.script_items):
                is_settings[start] = True
        decoded = []
        for idx, si in enumerate(obj.header.script_items):
            op = 0 if is_settings[idx] else si.script_type
            decoded.append(blocks.decode_block(op, si.payload))
        j["script_items_decoded"] = decoded
    out = sys.stdout if args.output == "-" else open(args.output, "w")
    json.dump(j, out, indent=2, ensure_ascii=False)
    if args.output != "-": out.close()


def cmd_pack(args):
    with open(args.input) as f:
        d = json.load(f)
    t = d.get("_format") or os.path.splitext(args.output)[1].lstrip(".").lower()
    if t.endswith("-fm95"):
        obj = Fm95Opaque.from_json(d)
    elif t in _PARSERS:
        obj = _PARSERS[t].from_json(d)
    else:
        sys.stderr.write(f"can't determine output format ({t!r})\n")
        return 1
    out = obj.pack()
    with open(args.output, "wb") as f:
        f.write(out)
    print(f"wrote {len(out)} bytes to {args.output}")


def cmd_verify(args):
    try:
        t = detect_type(args.input)
    except ValueError as e:
        print(f"SKIP {args.input}: {e}", file=sys.stderr)
        return 0
    with open(args.input, "rb") as f:
        data = f.read()
    try:
        if _is_fm95(data):
            obj = Fm95Opaque.parse(data, label=f"{t}-fm95")
            t = f"{t}-fm95"
        else:
            obj = _PARSERS[t].parse(data)
    except (NotImplementedError, ValueError) as e:
        print(f"SKIP {args.input}: {e}", file=sys.stderr)
        return 0
    repacked = obj.pack()
    if repacked == data:
        print(f"OK ({t}) round-trip exact ({len(data)} bytes): {args.input}")
        return 0
    for i, (a, b) in enumerate(zip(data, repacked)):
        if a != b:
            print(f"FAIL first diff at offset 0x{i:x}: "
                  f"{a:#04x} vs {b:#04x}", file=sys.stderr)
            break
    print(f"FAIL sizes orig={len(data)} repack={len(repacked)}",
          file=sys.stderr)
    return 1


def cmd_info(args):
    t, obj = parse_file(args.input)
    h = obj.header
    print(f"file:       {args.input}")
    print(f"format:     {t}")
    print(f"signature:  {h.file_signature[:8].decode('ascii', 'replace')}")
    print(f"name:       {_try_decode(h.project_name)}")
    print(f"scripts:        {len(h.scripts)}")
    print(f"script items:   {len(h.script_items)}")
    print(f"sprite frames:  {len(h.sprite_frames)}")
    print(f"palettes:       {len(h.palettes)}")
    print(f"sounds:         {len(h.sounds)}")
    if t == "stage":
        print(f"bgm_sound_id:   {obj.bgm_sound_id}")
    elif t == "demo":
        print(f"bgm_sound_id:   {obj.bgm_sound_id}")
        print(f"skip_by_any:    {bool(obj.is_skip_by_any_key)}")
        print(f"total_time:     {obj.total_time}")
    elif t == "player":
        print(f"tail byte:      {obj.tail}")


def main():
    ap = argparse.ArgumentParser(
        description="FM2K .player / .stage / .demo round-trip parser. "
                    "Auto-dispatches by file extension.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("parse")
    p.add_argument("input"); p.add_argument("-o", "--output", default="-")
    p.add_argument("--decode-blocks", action="store_true",
                   help="add semantic decode of script_items (Xem's "
                        "Block catalog; same as kgt.py)")
    p.set_defaults(fn=cmd_parse)

    p = sub.add_parser("pack")
    p.add_argument("input"); p.add_argument("output")
    p.set_defaults(fn=cmd_pack)

    p = sub.add_parser("verify")
    p.add_argument("input"); p.set_defaults(fn=cmd_verify)

    p = sub.add_parser("info")
    p.add_argument("input"); p.set_defaults(fn=cmd_info)

    args = ap.parse_args()
    rc = args.fn(args)
    sys.exit(rc or 0)


if __name__ == "__main__":
    main()
