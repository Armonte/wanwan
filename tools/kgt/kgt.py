#!/usr/bin/env python3
"""kgt.py

"""

from __future__ import annotations

import argparse
import base64
import dataclasses
import io
import json
import struct
import sys
from dataclasses import dataclass, field
from typing import List


# ─── Primitives ─────────────────────────────────────────────────────────

def _u8(buf: io.BytesIO) -> int:    return struct.unpack("<B", buf.read(1))[0]
def _u16(buf: io.BytesIO) -> int:   return struct.unpack("<H", buf.read(2))[0]
def _i16(buf: io.BytesIO) -> int:   return struct.unpack("<h", buf.read(2))[0]
def _u32(buf: io.BytesIO) -> int:   return struct.unpack("<I", buf.read(4))[0]
def _i32(buf: io.BytesIO) -> int:   return struct.unpack("<i", buf.read(4))[0]

def _bytes(buf: io.BytesIO, n: int) -> bytes:
    b = buf.read(n)
    if len(b) != n:
        raise EOFError(f"need {n} bytes, got {len(b)}")
    return b

def _pu8(v: int) -> bytes:   return struct.pack("<B", v)
def _pu16(v: int) -> bytes:  return struct.pack("<H", v)
def _pi16(v: int) -> bytes:  return struct.pack("<h", v)
def _pu32(v: int) -> bytes:  return struct.pack("<I", v)
def _pi32(v: int) -> bytes:  return struct.pack("<i", v)


# ─── RLE decompressor (ported from Unity DecompressUtil.cs) ────────────

def rle_decompress(src: bytes, dest_size: int) -> bytes:
    """2DFM RLE — 4 opcodes (top 2 bits): 0=zero-fill, 1=verbatim,
    2=byte-fill, 3=back-reference. Lengths 6-bit base with two-stage
    extension (0 → next byte+0x3f, 0+0 → next u16 + (byte<<16) + 0x13f)."""
    out = bytearray(dest_size)
    si = 0
    di = 0
    n = len(src)
    while si < n and di < dest_size:
        b = src[si]
        op = b >> 6
        run = b & 0x3F
        if run == 0:
            si += 1
            ext = src[si]
            if ext == 0:
                si += 1
                lo = src[si] | (src[si + 1] << 8)
                si += 2
                hi = src[si] << 16
                run = lo + hi + 0x013F
            else:
                run = ext + 0x3F
        if op == 0:
            # zero-fill: bytes already zero from bytearray(dest_size)
            di += run
        elif op == 1:
            # verbatim copy
            for i in range(run):
                out[di + i] = src[si + 1 + i]
            di += run
            si += run
        elif op == 2:
            si += 1
            nb = src[si]
            for i in range(run):
                out[di + i] = nb
            di += run
        elif op == 3:
            si += 1
            bt = src[si]
            if bt == 0:
                si += 1
                bt = (src[si] + 1) << 8
            start = di - bt
            for i in range(run):
                out[di + i] = out[start + i]
            di += run
        si += 1
    return bytes(out)


def rle_compress(px: bytes) -> bytes:
    """2DFM RLE encoder -- BYTE-EXACT port of KGT2nd_EDITOR.exe's ProcessRLEAnimationData
    (0x4344a0) + EncodeSprite{RunLength,BackReference,RawData,Command,Offset}. Greedy: at each
    position measure run-length (zero/byte-fill) vs back-reference (255-byte window, dist 2..255),
    longer wins (ties -> back-ref), threshold > 2, else literal. Verified byte-identical to original
    on 387/387 real frames. Round-trips with rle_decompress(). Enables re-RLE after a pixel-domain
    recompress (e.g. PEP) while keeping the .player byte-exact."""
    out = bytearray(); n = len(px)
    def emit_command(op, length):                      # EncodeSpriteCommand
        if length >= 64:
            out.append((op << 6) & 0xFF)
            if length >= 319:
                out.append(0); out.append((length - 63) & 0xFF)
                out.append(((length - 319) >> 8) & 0xFF); out.append(((length - 319) >> 16) & 0xFF)
            else:
                out.append((length - 63) & 0xFF)
        else:
            out.append((length + (op << 6)) & 0xFF)
    def emit_offset(dist):                             # EncodeSpriteOffset (dist 2..255 -> 1 byte)
        if dist >= 256:
            out.append(0); out.append(dist & 0xFF); out.append(((dist - 256) >> 8) & 0xFF)
        else:
            out.append(dist & 0xFF)
    def emit_rawdata(ls, pos):                         # EncodeSpriteRawData (op 1, verbatim)
        if pos != ls:
            emit_command(1, pos - ls); out.extend(px[ls:pos])
    def measure_runlength(pos):                        # EncodeSpriteRunLength (measure)
        b = px[pos]; v4 = 0
        while v4 < n - pos and px[pos + v4] == b:
            v4 += 1
        return v4
    def measure_backref(pos):                          # EncodeSpriteBackReference (measure)
        v6 = max(pos - 255, 0)
        if v6 == pos:
            return 0, pos
        best = 0; bstart = pos
        for start in range(v6, pos - 1):
            mlen = 0; k = 0
            while pos + k < n and px[start + k] == px[pos + k]:
                mlen += 1; k += 1
            if best < mlen:
                best = mlen; bstart = start
        return best, bstart
    pos = 0; ls = 0
    while pos < n:
        rl = measure_runlength(pos); bl, bs = measure_backref(pos)
        if rl <= bl:
            if bl > 2:
                emit_rawdata(ls, pos); emit_command(3, bl); emit_offset(pos - bs)
                pos += bl; ls = pos; continue
        elif rl > 2:
            emit_rawdata(ls, pos); b = px[pos]
            if b != 0:
                emit_command(2, rl); out.append(b)
            else:
                emit_command(0, rl)
            pos += rl; ls = pos; continue
        pos += 1
    emit_rawdata(ls, pos)
    return bytes(out)


# ─── BMP writer (8-bit indexed) ────────────────────────────────────────

def write_bmp_8bit(path: str, width: int, height: int,
                   palette_bgra: bytes, pixels: bytes) -> None:
    """Write a Windows BMP (BITMAPV3-ish, 8bpp indexed). Bottom-up rows
    with 4-byte stride padding. Palette is 256 × BGRA (1024 B)."""
    assert len(palette_bgra) == 1024
    assert len(pixels) >= width * height
    stride = (width + 3) & ~3   # 4-byte aligned
    row_pad = stride - width
    file_header_sz = 14
    info_header_sz = 40
    pal_sz = 1024
    bits_off = file_header_sz + info_header_sz + pal_sz
    img_sz = stride * height
    file_sz = bits_off + img_sz

    out = bytearray()
    # BITMAPFILEHEADER
    out += b"BM"
    out += struct.pack("<I", file_sz)
    out += struct.pack("<HH", 0, 0)
    out += struct.pack("<I", bits_off)
    # BITMAPINFOHEADER
    out += struct.pack("<I", info_header_sz)
    out += struct.pack("<i", width)
    out += struct.pack("<i", height)    # positive = bottom-up
    out += struct.pack("<HH", 1, 8)     # planes=1, bpp=8
    out += struct.pack("<I", 0)         # BI_RGB (uncompressed)
    out += struct.pack("<I", img_sz)
    out += struct.pack("<i", 2835)      # x ppm (72 dpi)
    out += struct.pack("<i", 2835)
    out += struct.pack("<I", 256)       # palette entries
    out += struct.pack("<I", 0)         # important colors
    # Palette: convert BGRA (file's order) to BGRX (BMP order; drop alpha)
    for i in range(256):
        b = palette_bgra[i * 4 + 0]
        g = palette_bgra[i * 4 + 1]
        r = palette_bgra[i * 4 + 2]
        out += bytes((b, g, r, 0))
    # Pixels: bottom-up, padded
    pad = b"\x00" * row_pad
    for y in range(height - 1, -1, -1):
        out += pixels[y * width : y * width + width]
        out += pad

    with open(path, "wb") as f:
        f.write(out)


def _try_decode(b: bytes) -> str:
    """Best-effort string decode for display. NUL-terminate at first 0."""
    nul = b.find(b"\x00")
    if nul >= 0:
        b = b[:nul]
    for enc in ("cp932", "gbk", "latin-1"):
        try:
            return b.decode(enc)
        except UnicodeDecodeError:
            continue
    return b.hex()


# ─── Records (SCRIPT, SCRIPT_ITEM, SPRITE_FRAME, Palette, SoundData, …) ──

@dataclass
class Script:
    """39 bytes. From common.h SCRIPT struct."""
    script_name: bytes          # 32B, NUL-terminated but preserve trailing garbage
    script_index: int           # int16 — start cmd_idx in scriptItemTable
    unknown_flag1: int          # uint8  — sometimes 0x01/0x21/0x39/0x81 per common.h
    special_flag: int           # int32  — special-script-item marker

    @classmethod
    def parse(cls, buf: io.BytesIO) -> "Script":
        return cls(_bytes(buf, 32), _i16(buf), _u8(buf), _i32(buf))

    def pack(self) -> bytes:
        assert len(self.script_name) == 32
        return (self.script_name + _pi16(self.script_index)
                + _pu8(self.unknown_flag1) + _pi32(self.special_flag))


@dataclass
class ScriptItem:
    """16 bytes — opcode dispatch struct from common.h SCRIPT_ITEM."""
    script_type: int            # uint8
    payload: bytes              # 15B

    @classmethod
    def parse(cls, buf: io.BytesIO) -> "ScriptItem":
        return cls(_u8(buf), _bytes(buf, 15))

    def pack(self) -> bytes:
        assert len(self.payload) == 15
        return _pu8(self.script_type) + self.payload


@dataclass
class SpriteFrame:
    """20-byte header + variable-size pixel data. 010 SPRITE_FRAME.

    Cross-ref: closercombat `kgtImageHeader` (5 int32 = 20 bytes, confirmed).
    Field ORDER is WIDTH(+4) then HEIGHT(+8) -- our original labeling.
    closercombat labels these iHeight(+4)/iWidth(+8), i.e. the OPPOSITE; we
    VISUALLY VERIFIED ours is correct by rendering non-square sprites both
    ways (Nogaku.player frames 298/747): row stride = +4 field gives clean,
    coherent sprites; row stride = +8 field gives diagonal shear-smear.
    So +4 = width. (Byte round-trip is unaffected either way -- only the
    labels + dump dimensions change.) SHARE-BACK to closercombat: their
    iHeight/iWidth are swapped, AND they mark our `has_private_palette`
    field only as "unk".
    """
    unknown_flag1: int          # int32 — closercombat kgtImageHeader.pAlloc:
                                #   runtime alloc pointer, 0 on disk (not an
                                #   "origin tag").
    width: int                  # int32 (+4) — row stride (visually verified;
                                #   closercombat mislabels this iHeight).
    height: int                 # int32 (+8) — closercombat mislabels iWidth.
    has_private_palette: int    # int32 — 1=appends 1KB palette after frame.
                                #   (closercombat has this as "unk".)
    size: int                   # int32 — kgtImageHeader.iSize. 0 = raw
                                #   width*height (+1KB if priv pal)
    frame_content: bytes        # variable

    @classmethod
    def parse(cls, buf: io.BytesIO) -> "SpriteFrame":
        uf1 = _i32(buf); w = _i32(buf); h = _i32(buf); hpp = _i32(buf); size = _i32(buf)
        if size == 0:
            t = w * h
            n = (t + (1024 if hpp else 0)) if t > 0 else 0
        else:
            n = size
        return cls(uf1, w, h, hpp, size, _bytes(buf, n) if n else b"")

    def pack(self) -> bytes:
        return (_pi32(self.unknown_flag1) + _pi32(self.width) + _pi32(self.height)
                + _pi32(self.has_private_palette) + _pi32(self.size)
                + self.frame_content)


@dataclass
class Palette:
    """1024 BGRA + 32 byte gap = 1056 bytes.

    Cross-ref: closercombat `kgtPallette` is a SINGLE entry (b, g, r,
    field3='always 01'); the character struct holds 8 of `kgtPallette[256]`
    back-to-back at stride 0x400 (1024B), with NO inter-palette gap. So our
    per-entry 'A' byte == their field3 (the constant 0x01). Our 32-byte `gap`
    is therefore FILE-format-specific (closercombat's is the editor's
    in-memory layout, which lacks it) -- keep it; it's real on disk.
    """
    colors: bytes               # 256 × 4 BGRA (== 256 × kgtPallette; A byte is
                                #   their field3, constant 0x01)
    gap: bytes                  # 32B file-only padding (always-zero observed;
                                #   absent from closercombat's in-memory struct)

    @classmethod
    def parse(cls, buf: io.BytesIO) -> "Palette":
        return cls(_bytes(buf, 1024), _bytes(buf, 32))

    def pack(self) -> bytes:
        assert len(self.colors) == 1024 and len(self.gap) == 32
        return self.colors + self.gap


@dataclass
class Sound:
    """42-byte header + variable data. From common.h SoundData.

    Cross-ref: closercombat `kgtSound` (docs/kgt_filetype_definitions.h) --
    byte-for-byte identical, 42 bytes (0x2A). Their field names + notes are
    folded into the per-field comments below; field names kept as-is here
    because fm2nd.py round-trips them by name into JSON.
    """
    unknown1: int               # int32 — closercombat kgtSound.pAlloc: the
                                #   RUNTIME allocation pointer, 0 on disk. (Was
                                #   guessed as an "origin tag"; it's the alloc
                                #   slot the loader fills, hence always 0 in
                                #   files.)
    name: bytes                 # 32B — kgtSound.sName[32]
    size: int                   # int32 — bytes of `data`. closercombat
                                #   kgtSound.iSizeOrWavPtr: "the size, reused as
                                #   the wav pointer after reading from file"
                                #   (matches char_data_loader overwriting it
                                #   with the sound-object ptr at +36).
    sound_type: int             # uint8 — kgtSound.cFlags. Low nibble = what to
                                #   do: 0=stop-all, 1=WAV, 2=MIDI, 3=CD (==
                                #   DispatchScriptSoundCommand @0x403430 cases).
                                #   High nibble = playing behavior. NB on 0x10:
                                #   on the SOUND's own cFlags closercombat reads
                                #   it as LOOP; on the SCRIPT command byte
                                #   0x403430 reads it as the volume-mode flag --
                                #   different bytes, both true. Observed: 1,2,3,17.
    sound_track: int            # uint8 — kgtSound.cMciFlags (MCI flags; broader
                                #   than just a CDDA track number).
    data: bytes                 # variable

    @classmethod
    def parse(cls, buf: io.BytesIO) -> "Sound":
        u1 = _i32(buf); n = _bytes(buf, 32); sz = _i32(buf)
        st = _u8(buf); tk = _u8(buf)
        return cls(u1, n, sz, st, tk, _bytes(buf, sz) if sz > 0 else b"")

    def pack(self) -> bytes:
        assert len(self.name) == 32
        if self.size != len(self.data):
            raise ValueError(
                f"sound {_try_decode(self.name).strip()!r}: size field "
                f"({self.size}) != data length ({len(self.data)}); the engine "
                f"reads exactly `size` bytes, so a mismatch corrupts every "
                f"record after this one")
        return (_pi32(self.unknown1) + self.name + _pi32(self.size)
                + _pu8(self.sound_type) + _pu8(self.sound_track) + self.data)

    @classmethod
    def from_dict(cls, s: dict, idx: int = -1) -> "Sound":
        """Build from the JSON dict shape. `size` is derived from the decoded
        data; a stale hand-edited size field is fixed with a warning instead
        of producing a corrupt file."""
        data = base64.b64decode(s["data_b64"])
        size = s["size"]
        if size != len(data):
            name = _try_decode(bytes.fromhex(s["name"])).strip()
            print(f"WARN sound[{idx}] {name!r}: JSON size field ({size}) != "
                  f"decoded data length ({len(data)}); using data length",
                  file=sys.stderr)
            size = len(data)
        return cls(unknown1=s["unknown1"], name=bytes.fromhex(s["name"]),
                   size=size, sound_type=s["sound_type"],
                   sound_track=s["sound_track"], data=data)


@dataclass
class Reaction:
    """36 bytes — 32 name + 4 flags. 010 ReactionItem."""
    name: bytes                 # 32B
    is_hurt_action: int         # int32 (0/1 per C++ KgtGame, not bitflags)

    @classmethod
    def parse(cls, buf: io.BytesIO) -> "Reaction":
        return cls(_bytes(buf, 32), _i32(buf))

    def pack(self) -> bytes:
        assert len(self.name) == 32
        return self.name + _pi32(self.is_hurt_action)


@dataclass
class CommonImage:
    """32 bytes — image-asset filename. Editor-only: the WW runtime never
    references this 6400-byte region (verified by IDA insn scan over the
    full text segment — zero immediate/lea/mov targeting the in-memory
    range 0x4438AC..0x4451AC). The editor stores 200 shared image asset
    names here so its asset-library UI can list them; the game itself
    only loads them by per-character SpriteFrame index. The 010 Editor
    template labeled this `ThrowReactionItem[200]`; that name appears to
    be a guess — Xem's `CommonImages` matches both the editor-UI labels
    and the runtime evidence."""
    name: bytes                 # 32B

    @classmethod
    def parse(cls, buf: io.BytesIO) -> "CommonImage":
        return cls(_bytes(buf, 32))

    def pack(self) -> bytes:
        assert len(self.name) == 32
        return self.name


# Backward-compat alias for callers that imported the old class name.
ThrowReaction = CommonImage


# ─── Top-level Kgt ──────────────────────────────────────────────────────

@dataclass
class Kgt:
    # ── Header ─────────────────────────────────────────────────────────
    file_signature: bytes               # 16B  "2DKGT2K\\0" + 8 trailing bytes (version flags)
    project_name: bytes                 # 256B NUL-padded, may have trailing garbage

    # ── Common resource section (variable-length) ──────────────────────
    scripts: List[Script] = field(default_factory=list)
    script_items: List[ScriptItem] = field(default_factory=list)
    sprite_frames: List[SpriteFrame] = field(default_factory=list)
    palettes: List[Palette] = field(default_factory=list)   # always 8
    sounds: List[Sound] = field(default_factory=list)

    # ── Game system block (66108 bytes in WW) ──────────────────────────
    player_name_gap: int = 0            # int32 — fseek-skip-4 in C++ reader
    player_infos: List[bytes] = field(default_factory=list)   # 50 × 256B (g_char_slot_data filenames)
    reactions: List[Reaction] = field(default_factory=list)   # 200 × 36B

    # 4 bytes the C++ reader fseek's past (no xrefs in WW build).
    # Value is consistently `0x00000002` across our 146-file corpus.
    # 010 template comment: "fixed value 02 00 00 00".
    freeze_time_marker: int = 0         # int32

    # RecoverTimeConfig — 4 bytes mapped by IDA disasm to per-frame globals:
    #   +0: no xrefs in WW (vestigial first byte; C++ calls it `gap`)
    #   +1: g_block_hitstop_frames    (frames defender freezes on block)
    #   +2: g_hit_hitstop_frames      (frames attacker+defender freeze on hit)
    #   +3: g_collision_effect_type   (effect type on attack/attack clash)
    # C++ template names (atk/dfs/ccl freeze time) are editor-perspective.
    recover_time_gap: int = 0           # uint8 RecoverTimeConfig.gap (no xrefs)
    block_hitstop_frames: int = 0       # uint8 g_block_hitstop_frames
    hit_hitstop_frames: int = 0         # uint8 g_hit_hitstop_frames
    collision_effect_type: int = 0      # uint8 g_collision_effect_type

    stage_infos: List[bytes] = field(default_factory=list)    # 50 × 256B (g_stage_file_buffer)
    demo_infos: List[bytes] = field(default_factory=list)     # 100 × 256B

    # GameDemoConfig — 8 bytes. 6 named + 2 vestigial.
    title_demo_id: int = 0
    char_sel_for_1p_demo_id: int = 0    # storyModeCharSelectDemoId in C++
    single_demo_id: int = 0             # oneVsOneModeCharSelectDemoId
    team_demo_id: int = 0               # teamModeCharSelectDemoId
    gameover_demo_id: int = 0           # continueDemoId
    start_demo_id: int = 0              # openingDemoId
    unknown_demo_id_1: int = 0          # no xrefs in WW (`unknownTag1` in C++)
    unknown_demo_id_2: int = 0          # no xrefs in WW (`unknownTag2` in C++)

    # ProjectBaseConfig — int32 bitfield. Low 7 bits encode game-mode flags.
    general_settings: int = 0           # int32

    common_images: List[CommonImage] = field(default_factory=list)   # 200 × 32B image-asset names (editor-only)

    # 132 × u16 array. IDA: `g_score_digit_sprites_array` @ 0x4451AC.
    # Read by DisplayScoreNumbers as `[digit_value % 10]` for score digits.
    # First 50 entries always identity (0..49 = digit-base slot index);
    # next 54 entries vary per game (alternate digit sprite IDs); last 28
    # always zero. The 010 template called it "位置数据" (Position data) —
    # outdated; IDA name is authoritative.
    score_digit_sprites_array: bytes = b""    # 264B (132 × u16)

    # CharSelectConfig — 28 bytes (14 × signed/unsigned int16).
    char_sel_start_pos_x: int = 0       # uint16
    char_sel_start_pos_y: int = 0
    char_img_width: int = 0
    char_img_height: int = 0
    char_sel_col_count: int = 0
    char_sel_row_count: int = 0
    char_sel_for_p1_pos_x: int = 0
    char_sel_for_p1_pos_y: int = 0
    char_sel_for_team_p1_range_x: int = 0   # int16
    char_sel_for_team_p1_range_y: int = 0
    char_sel_for_p2_pos_x: int = 0
    char_sel_for_p2_pos_y: int = 0
    char_sel_for_p2_range_x: int = 0
    char_sel_for_p2_range_y: int = 0

    # 50-byte per-slot CSS eligibility byte. Per C++ reader: `playerSelectableInfos[50]`.
    # Values observed: 3=selectable, 1=CPU-only/hidden, 0=empty slot.
    player_selectable_infos: bytes = b""

    # 946 trailing bytes. Confirmed all-zero across all 146 observed FM2K
    # samples. No IDA xrefs. Almost certainly editor-time alignment padding
    # the game's LoadGameSystemFile actually reads only the first 942 of
    # (file = 996 + sig_block, game reads 0x1023C bytes total).
    reserved_tail_946: bytes = b""

    # ── Parsing ─────────────────────────────────────────────────────────

    @classmethod
    def parse(cls, data: bytes) -> "Kgt":
        buf = io.BytesIO(data)

        sig = _bytes(buf, 16)
        sig_str = sig[:7]
        if sig_str in (b"KGTGAME", b"2DKGT95"):
            raise NotImplementedError(
                f"FM95 .kgt format detected (signature={sig_str!r}); "
                "this parser only handles FM2K (2DKGT2K / 2DKGT2G). "
                "See FM2K_KgtParser.cpp for the FM95 fixed-offset layout.")
        if sig_str not in (b"2DKGT2K", b"2DKGT2G"):
            raise ValueError(
                f"unknown .kgt signature {sig_str!r}; "
                "supported: 2DKGT2K, 2DKGT2G (FM2K family)")

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

        player_name_gap = _i32(buf)
        player_infos = [_bytes(buf, 256) for _ in range(50)]

        reactions = [Reaction.parse(buf) for _ in range(200)]

        freeze_time_marker = _i32(buf)
        recover_time_gap     = _u8(buf)
        block_hitstop_frames = _u8(buf)
        hit_hitstop_frames   = _u8(buf)
        collision_effect_type = _u8(buf)

        stage_infos = [_bytes(buf, 256) for _ in range(50)]
        demo_infos  = [_bytes(buf, 256) for _ in range(100)]

        title_demo_id           = _u8(buf)
        char_sel_for_1p_demo_id = _u8(buf)
        single_demo_id          = _u8(buf)
        team_demo_id            = _u8(buf)
        gameover_demo_id        = _u8(buf)
        start_demo_id           = _u8(buf)
        unknown_demo_id_1       = _u8(buf)
        unknown_demo_id_2       = _u8(buf)

        general_settings = _i32(buf)

        common_images = [CommonImage.parse(buf) for _ in range(200)]

        score_digit_sprites_array = _bytes(buf, 264)

        cs_sx  = _u16(buf); cs_sy  = _u16(buf)
        ci_w   = _u16(buf); ci_h   = _u16(buf)
        cs_cc  = _u16(buf); cs_rc  = _u16(buf)
        cs_p1x = _u16(buf); cs_p1y = _u16(buf)
        cs_t1x = _i16(buf); cs_t1y = _i16(buf)
        cs_p2x = _u16(buf); cs_p2y = _u16(buf)
        cs_t2x = _i16(buf); cs_t2y = _i16(buf)

        player_selectable_infos = _bytes(buf, 50)
        reserved_tail_946       = _bytes(buf, 946)

        trailing = buf.read()
        if trailing:
            sys.stderr.write(
                f"warning: {len(trailing)} trailing bytes after parse\n")

        return cls(
            file_signature=sig, project_name=proj,
            scripts=scripts, script_items=script_items,
            sprite_frames=sprite_frames, palettes=palettes, sounds=sounds,
            player_name_gap=player_name_gap, player_infos=player_infos,
            reactions=reactions,
            freeze_time_marker=freeze_time_marker,
            recover_time_gap=recover_time_gap,
            block_hitstop_frames=block_hitstop_frames,
            hit_hitstop_frames=hit_hitstop_frames,
            collision_effect_type=collision_effect_type,
            stage_infos=stage_infos, demo_infos=demo_infos,
            title_demo_id=title_demo_id,
            char_sel_for_1p_demo_id=char_sel_for_1p_demo_id,
            single_demo_id=single_demo_id, team_demo_id=team_demo_id,
            gameover_demo_id=gameover_demo_id,
            start_demo_id=start_demo_id,
            unknown_demo_id_1=unknown_demo_id_1,
            unknown_demo_id_2=unknown_demo_id_2,
            general_settings=general_settings,
            common_images=common_images,
            score_digit_sprites_array=score_digit_sprites_array,
            char_sel_start_pos_x=cs_sx, char_sel_start_pos_y=cs_sy,
            char_img_width=ci_w, char_img_height=ci_h,
            char_sel_col_count=cs_cc, char_sel_row_count=cs_rc,
            char_sel_for_p1_pos_x=cs_p1x, char_sel_for_p1_pos_y=cs_p1y,
            char_sel_for_team_p1_range_x=cs_t1x,
            char_sel_for_team_p1_range_y=cs_t1y,
            char_sel_for_p2_pos_x=cs_p2x, char_sel_for_p2_pos_y=cs_p2y,
            char_sel_for_p2_range_x=cs_t2x,
            char_sel_for_p2_range_y=cs_t2y,
            player_selectable_infos=player_selectable_infos,
            reserved_tail_946=reserved_tail_946,
        )

    # ── Packing ─────────────────────────────────────────────────────────

    def pack(self) -> bytes:
        out = io.BytesIO()
        assert len(self.file_signature) == 16
        assert len(self.project_name) == 256
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
        for sd in self.sounds: out.write(sd.pack())

        out.write(_pi32(self.player_name_gap))
        assert len(self.player_infos) == 50
        for pi in self.player_infos:
            assert len(pi) == 256
            out.write(pi)

        assert len(self.reactions) == 200
        for r in self.reactions: out.write(r.pack())

        out.write(_pi32(self.freeze_time_marker))
        out.write(_pu8(self.recover_time_gap))
        out.write(_pu8(self.block_hitstop_frames))
        out.write(_pu8(self.hit_hitstop_frames))
        out.write(_pu8(self.collision_effect_type))

        assert len(self.stage_infos) == 50
        for s in self.stage_infos:
            assert len(s) == 256
            out.write(s)
        assert len(self.demo_infos) == 100
        for d in self.demo_infos:
            assert len(d) == 256
            out.write(d)

        for v in (self.title_demo_id, self.char_sel_for_1p_demo_id,
                  self.single_demo_id, self.team_demo_id,
                  self.gameover_demo_id, self.start_demo_id,
                  self.unknown_demo_id_1, self.unknown_demo_id_2):
            out.write(_pu8(v))

        out.write(_pi32(self.general_settings))

        assert len(self.common_images) == 200
        for tr in self.common_images: out.write(tr.pack())

        assert len(self.score_digit_sprites_array) == 264
        out.write(self.score_digit_sprites_array)

        for v in (self.char_sel_start_pos_x, self.char_sel_start_pos_y,
                  self.char_img_width, self.char_img_height,
                  self.char_sel_col_count, self.char_sel_row_count,
                  self.char_sel_for_p1_pos_x, self.char_sel_for_p1_pos_y):
            out.write(_pu16(v))
        out.write(_pi16(self.char_sel_for_team_p1_range_x))
        out.write(_pi16(self.char_sel_for_team_p1_range_y))
        out.write(_pu16(self.char_sel_for_p2_pos_x))
        out.write(_pu16(self.char_sel_for_p2_pos_y))
        out.write(_pi16(self.char_sel_for_p2_range_x))
        out.write(_pi16(self.char_sel_for_p2_range_y))

        assert len(self.player_selectable_infos) == 50
        out.write(self.player_selectable_infos)
        assert len(self.reserved_tail_946) == 946
        out.write(self.reserved_tail_946)

        return out.getvalue()

    # ── JSON dump / load ────────────────────────────────────────────────

    def general_settings_bits(self) -> dict:
        """Decode general_settings (ProjectBaseConfig) into named flags."""
        gs = self.general_settings
        return {
            "encrypt_game":         bool(gs & (1 << 0)),
            "allow_clash":          bool(gs & (1 << 1)),
            "enable_story_mode":    bool(gs & (1 << 2)),
            "enable_1v1_mode":      bool(gs & (1 << 3)),
            "enable_team_mode":     bool(gs & (1 << 4)),
            "show_hp_after_bar":    bool(gs & (1 << 5)),
            "press_to_start":       bool(gs & (1 << 6)),
            "raw":                  gs,
        }

    def score_digits_array(self) -> List[int]:
        """Decode score_digit_sprites_array as 132 × u16."""
        return list(struct.unpack("<132H", self.score_digit_sprites_array))

    def player_selectable_infos_bits(self) -> List[dict]:
        """Decode the 50-byte CSS-eligibility block as 2-bit flags per slot.
        Per Xem's fm2ndparser reverse-engineering: bit 0 = enabled in
        story mode, bit 1 = enabled in vs mode. Explains why only 0/1/3
        are observed (bit 2 onward asserted unused). 2 alone is never
        seen in our corpus."""
        out = []
        for b in self.player_selectable_infos:
            out.append({
                "story_mode": bool(b & 0x1),
                "vs_mode":    bool(b & 0x2),
                "raw":        b,
            })
        return out

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
                 "frame_content_b64": base64.b64encode(sf.frame_content).decode()}
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
            "player_name_gap": self.player_name_gap,
            "player_infos": [
                {"raw": pi.hex(), "name_str": _try_decode(pi)}
                for pi in self.player_infos],
            "reactions": [
                {"name": r.name.hex(),
                 "name_str": _try_decode(r.name),
                 "is_hurt_action": r.is_hurt_action}
                for r in self.reactions],
            "freeze_time_marker": self.freeze_time_marker,
            "recover_time_gap": self.recover_time_gap,
            "block_hitstop_frames": self.block_hitstop_frames,
            "hit_hitstop_frames": self.hit_hitstop_frames,
            "collision_effect_type": self.collision_effect_type,
            "stage_infos": [
                {"raw": s.hex(), "name_str": _try_decode(s)}
                for s in self.stage_infos],
            "demo_infos": [
                {"raw": d.hex(), "name_str": _try_decode(d)}
                for d in self.demo_infos],
            "title_demo_id": self.title_demo_id,
            "char_sel_for_1p_demo_id": self.char_sel_for_1p_demo_id,
            "single_demo_id": self.single_demo_id,
            "team_demo_id": self.team_demo_id,
            "gameover_demo_id": self.gameover_demo_id,
            "start_demo_id": self.start_demo_id,
            "unknown_demo_id_1": self.unknown_demo_id_1,
            "unknown_demo_id_2": self.unknown_demo_id_2,
            "general_settings": self.general_settings,
            "general_settings_bits": self.general_settings_bits(),
            "common_images": [
                {"name": t.name.hex(),
                 "name_str": _try_decode(t.name)}
                for t in self.common_images],
            "score_digit_sprites_array": self.score_digit_sprites_array.hex(),
            "score_digit_sprites_array_u16": self.score_digits_array(),
            "char_sel_start_pos_x": self.char_sel_start_pos_x,
            "char_sel_start_pos_y": self.char_sel_start_pos_y,
            "char_img_width": self.char_img_width,
            "char_img_height": self.char_img_height,
            "char_sel_col_count": self.char_sel_col_count,
            "char_sel_row_count": self.char_sel_row_count,
            "char_sel_for_p1_pos_x": self.char_sel_for_p1_pos_x,
            "char_sel_for_p1_pos_y": self.char_sel_for_p1_pos_y,
            "char_sel_for_team_p1_range_x": self.char_sel_for_team_p1_range_x,
            "char_sel_for_team_p1_range_y": self.char_sel_for_team_p1_range_y,
            "char_sel_for_p2_pos_x": self.char_sel_for_p2_pos_x,
            "char_sel_for_p2_pos_y": self.char_sel_for_p2_pos_y,
            "char_sel_for_p2_range_x": self.char_sel_for_p2_range_x,
            "char_sel_for_p2_range_y": self.char_sel_for_p2_range_y,
            "player_selectable_infos": list(self.player_selectable_infos),
            "player_selectable_infos_bits": self.player_selectable_infos_bits(),
            "reserved_tail_946": self.reserved_tail_946.hex(),
        }

    @classmethod
    def from_json(cls, d: dict) -> "Kgt":
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
                payload=bytes.fromhex(si["payload"])) for si in d["script_items"]],
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
            player_name_gap=d["player_name_gap"],
            player_infos=[bytes.fromhex(pi["raw"]) for pi in d["player_infos"]],
            reactions=[Reaction(
                name=bytes.fromhex(r["name"]),
                is_hurt_action=r["is_hurt_action"]) for r in d["reactions"]],
            freeze_time_marker=d["freeze_time_marker"],
            recover_time_gap=d["recover_time_gap"],
            block_hitstop_frames=d["block_hitstop_frames"],
            hit_hitstop_frames=d["hit_hitstop_frames"],
            collision_effect_type=d["collision_effect_type"],
            stage_infos=[bytes.fromhex(s["raw"]) for s in d["stage_infos"]],
            demo_infos=[bytes.fromhex(s["raw"]) for s in d["demo_infos"]],
            title_demo_id=d["title_demo_id"],
            char_sel_for_1p_demo_id=d["char_sel_for_1p_demo_id"],
            single_demo_id=d["single_demo_id"],
            team_demo_id=d["team_demo_id"],
            gameover_demo_id=d["gameover_demo_id"],
            start_demo_id=d["start_demo_id"],
            unknown_demo_id_1=d["unknown_demo_id_1"],
            unknown_demo_id_2=d["unknown_demo_id_2"],
            general_settings=d["general_settings"],
            common_images=[CommonImage(name=bytes.fromhex(t["name"]))
                            for t in d.get("common_images",
                                            d.get("throw_reactions", []))],
            score_digit_sprites_array=bytes.fromhex(d["score_digit_sprites_array"]),
            char_sel_start_pos_x=d["char_sel_start_pos_x"],
            char_sel_start_pos_y=d["char_sel_start_pos_y"],
            char_img_width=d["char_img_width"],
            char_img_height=d["char_img_height"],
            char_sel_col_count=d["char_sel_col_count"],
            char_sel_row_count=d["char_sel_row_count"],
            char_sel_for_p1_pos_x=d["char_sel_for_p1_pos_x"],
            char_sel_for_p1_pos_y=d["char_sel_for_p1_pos_y"],
            char_sel_for_team_p1_range_x=d["char_sel_for_team_p1_range_x"],
            char_sel_for_team_p1_range_y=d["char_sel_for_team_p1_range_y"],
            char_sel_for_p2_pos_x=d["char_sel_for_p2_pos_x"],
            char_sel_for_p2_pos_y=d["char_sel_for_p2_pos_y"],
            char_sel_for_p2_range_x=d["char_sel_for_p2_range_x"],
            char_sel_for_p2_range_y=d["char_sel_for_p2_range_y"],
            player_selectable_infos=bytes(d["player_selectable_infos"]),
            reserved_tail_946=bytes.fromhex(d["reserved_tail_946"]),
        )


# ─── CLI ────────────────────────────────────────────────────────────────

def cmd_parse(args):
    with open(args.input, "rb") as f:
        data = f.read()
    k = Kgt.parse(data)
    j = k.to_json()
    if args.decode_blocks:
        import blocks
        # The first script_item of each skill is the "settings" sentinel;
        # subsequent items are opcode-dispatched. We walk skill ranges
        # using scripts[i].script_index as the cursor.
        ranges = []
        for i, sc in enumerate(k.scripts):
            start = sc.script_index
            end = (k.scripts[i + 1].script_index
                   if i + 1 < len(k.scripts) else len(k.script_items))
            ranges.append((start, end))
        is_settings = [False] * len(k.script_items)
        for (start, end) in ranges:
            if 0 <= start < len(k.script_items):
                is_settings[start] = True
        decoded = []
        for idx, si in enumerate(k.script_items):
            if is_settings[idx]:
                # Force opcode 0 dispatch so it picks _parse_settings.
                decoded.append(blocks.decode_block(0, si.payload))
            else:
                decoded.append(blocks.decode_block(si.script_type, si.payload))
        j["script_items_decoded"] = decoded
    out = sys.stdout if args.output == "-" else open(args.output, "w")
    json.dump(j, out, indent=2, ensure_ascii=False)
    if args.output != "-":
        out.close()


def cmd_pack(args):
    with open(args.input) as f:
        d = json.load(f)
    k = Kgt.from_json(d)
    out = k.pack()
    with open(args.output, "wb") as f:
        f.write(out)
    print(f"wrote {len(out)} bytes to {args.output}")


def cmd_verify(args):
    with open(args.input, "rb") as f:
        data = f.read()
    # FM95 fallback: opaque-blob round-trip via fm2nd.Fm95Opaque.
    if data[:7] in (b"KGTGAME", b"2DKGT95"):
        import fm2nd
        obj = fm2nd.Fm95Opaque.parse(data, label="kgt-fm95")
        if obj.pack() == data:
            print(f"OK (kgt-fm95) round-trip exact ({len(data)} bytes): {args.input}")
            return 0
        print(f"FAIL FM95 round-trip diverged: {args.input}", file=sys.stderr)
        return 1
    try:
        k = Kgt.parse(data)
    except (NotImplementedError, ValueError) as e:
        print(f"SKIP {args.input}: {e}", file=sys.stderr)
        return 0
    repacked = k.pack()
    if repacked == data:
        print(f"OK round-trip exact ({len(data)} bytes): {args.input}")
        return 0
    for i, (a, b) in enumerate(zip(data, repacked)):
        if a != b:
            print(f"FAIL first diff at offset 0x{i:x}: "
                  f"{a:#04x} vs {b:#04x}", file=sys.stderr)
            break
    print(f"FAIL sizes orig={len(data)} repack={len(repacked)}", file=sys.stderr)
    return 1


def cmd_info(args):
    with open(args.input, "rb") as f:
        data = f.read()
    try:
        k = Kgt.parse(data)
    except (NotImplementedError, ValueError) as e:
        print(f"SKIP: {e}")
        return 0
    players = [_try_decode(pi) for pi in k.player_infos if pi[0] != 0]
    stages  = [_try_decode(s) for s in k.stage_infos if s[0] != 0]
    demos   = [_try_decode(s) for s in k.demo_infos if s[0] != 0]
    bits = k.general_settings_bits()
    on_flags = [n for n, v in bits.items() if v and n != "raw"]
    print(f"file: {args.input}")
    print(f"size: {len(data)} bytes")
    print(f"signature: {k.file_signature.hex()} ({_try_decode(k.file_signature)})")
    print(f"project: {_try_decode(k.project_name)}")
    print(f"scripts: {len(k.scripts)}")
    print(f"script_items: {len(k.script_items)}")
    print(f"sprite_frames: {len(k.sprite_frames)}")
    print(f"sounds: {len(k.sounds)}")
    print(f"freeze_time_marker: {k.freeze_time_marker}  recover_time_gap: {k.recover_time_gap}")
    print(f"hitstop frames: block={k.block_hitstop_frames} hit={k.hit_hitstop_frames}  collision_effect_type={k.collision_effect_type}")
    print(f"general_settings: 0x{k.general_settings:08x} → {','.join(on_flags) if on_flags else '(none)'}")
    print(f"players ({len(players)}): {players[:5]}{'...' if len(players)>5 else ''}")
    print(f"stages ({len(stages)}): {stages[:5]}{'...' if len(stages)>5 else ''}")
    print(f"demos ({len(demos)}): {demos[:5]}{'...' if len(demos)>5 else ''}")


def cmd_extract(args):
    """Dump all assets: sprite frames → BMP, sounds → WAV/MID/CDA-stub,
    palettes → BMP swatches. Skips empty / size==0 slots."""
    import os
    with open(args.input, "rb") as f:
        data = f.read()
    try:
        k = Kgt.parse(data)
    except (NotImplementedError, ValueError) as e:
        print(f"SKIP: {e}", file=sys.stderr)
        return 1

    base = args.output
    os.makedirs(os.path.join(base, "frames"),   exist_ok=True)
    os.makedirs(os.path.join(base, "palettes"), exist_ok=True)
    os.makedirs(os.path.join(base, "sounds"),   exist_ok=True)

    # Default shared palette for frames with no private palette.
    default_pal = k.palettes[args.palette].colors

    # Frames → BMP
    frame_count = 0
    for i, sf in enumerate(k.sprite_frames):
        if sf.width == 0 or sf.height == 0:
            continue
        n_pixels = sf.width * sf.height
        # Decode raw pixel bytes + (optional) private palette
        if sf.size != 0:
            # Compressed: decompress to width*height (+ 1024 if priv pal)
            need = n_pixels + (1024 if sf.has_private_palette else 0)
            try:
                blob = rle_decompress(sf.frame_content, need)
            except Exception as e:
                print(f"  frame {i}: decompress fail: {e}", file=sys.stderr)
                continue
        else:
            blob = sf.frame_content
        if sf.has_private_palette:
            palette = blob[:1024]
            pixels  = blob[1024:1024 + n_pixels]
        else:
            palette = default_pal
            pixels  = blob[:n_pixels]
        if len(pixels) < n_pixels:
            print(f"  frame {i}: short by {n_pixels - len(pixels)} bytes", file=sys.stderr)
            continue
        outp = os.path.join(base, "frames", f"{i:04d}_{sf.width}x{sf.height}.bmp")
        write_bmp_8bit(outp, sf.width, sf.height, palette, pixels)
        frame_count += 1

    # Shared palettes → 16×16 BMP swatch (each color = 1 pixel)
    for pi, p in enumerate(k.palettes):
        # Build 16×16 paletted image where pixel[y*16+x] = (y*16+x)
        pixels = bytes(range(256))
        outp = os.path.join(base, "palettes", f"{pi}.bmp")
        write_bmp_8bit(outp, 16, 16, p.colors, pixels)

    # Sounds → WAV / MIDI / CDA stub
    sound_count = 0
    for i, s in enumerate(k.sounds):
        if s.size == 0 or not s.data:
            continue
        # sound_type is a packed byte: low nibble = type (1=WAV, 2=MIDI,
        # 3=CDDA), high bits = flags (0x10 = volume mode). Use the nibble.
        # Confirmed via wwdecomp src/sound.c DispatchScriptSoundCommand.
        ext = {1: "wav", 2: "mid", 3: "bin"}.get(s.sound_type & 0xF, "bin")
        name = _try_decode(s.name).strip().replace("/", "_") or f"sound{i}"
        # Strip existing extension if user-named the entry "foo.wav"
        for e in (".wav", ".WAV", ".mid", ".MID", ".midi", ".bin"):
            if name.endswith(e):
                name = name[:-len(e)]
                break
        outp = os.path.join(base, "sounds", f"{i:04d}_{name}.{ext}")
        with open(outp, "wb") as f:
            f.write(s.data)
        sound_count += 1

    print(f"extracted to {base}:")
    print(f"  frames:   {frame_count} / {len(k.sprite_frames)} BMPs")
    print(f"  palettes: 8 BMPs (16×16 swatches)")
    print(f"  sounds:   {sound_count} / {len(k.sounds)} WAV/MIDI files")
    return 0


def cmd_coverage(args):
    """Show byte-level coverage map — every section accounted for."""
    with open(args.input, "rb") as f:
        data = f.read()
    buf = io.BytesIO(data)
    sections = []
    def mark(name, start, end, kind="known"):
        sections.append((start, end, name, kind, end - start))
    pos = lambda: buf.tell()

    s = pos(); _bytes(buf, 16);    mark("file_signature (sig+version)", s, pos())
    s = pos(); _bytes(buf, 256);   mark("project_name", s, pos())
    s = pos(); n = _i32(buf);      mark("scripts.count", s, pos())
    s = pos()
    for _ in range(n): Script.parse(buf)
    mark(f"scripts[{n}]", s, pos())
    s = pos(); n = _i32(buf);      mark("script_items.count", s, pos())
    s = pos()
    for _ in range(n): ScriptItem.parse(buf)
    mark(f"script_items[{n}]", s, pos())
    s = pos(); n = _i32(buf);      mark("sprite_frames.count", s, pos())
    s = pos()
    for _ in range(n): SpriteFrame.parse(buf)
    mark(f"sprite_frames[{n}]", s, pos())
    s = pos()
    for _ in range(8): Palette.parse(buf)
    mark("palettes[8]", s, pos())
    s = pos(); n = _i32(buf);      mark("sounds.count", s, pos())
    s = pos()
    for _ in range(n): Sound.parse(buf)
    mark(f"sounds[{n}]", s, pos())
    s = pos(); _i32(buf);          mark("player_name_gap", s, pos())
    s = pos()
    for _ in range(50): _bytes(buf, 256)
    mark("player_infos[50] (g_char_slot_data)", s, pos())
    s = pos()
    for _ in range(200): Reaction.parse(buf)
    mark("reactions[200]", s, pos())
    s = pos(); _i32(buf);          mark("freeze_time_marker (always 2)", s, pos())
    s = pos(); _u8(buf);           mark("recover_time_gap", s, pos())
    s = pos(); _u8(buf);           mark("block_hitstop_frames", s, pos())
    s = pos(); _u8(buf);           mark("hit_hitstop_frames", s, pos())
    s = pos(); _u8(buf);           mark("collision_effect_type", s, pos())
    s = pos()
    for _ in range(50): _bytes(buf, 256)
    mark("stage_infos[50] (g_stage_file_buffer)", s, pos())
    s = pos()
    for _ in range(100): _bytes(buf, 256)
    mark("demo_infos[100]", s, pos())
    s = pos(); _bytes(buf, 6);     mark("6 demo_ids (title..opening)", s, pos())
    s = pos(); _bytes(buf, 2);     mark("unknown_demo_tag_1/2 (no xrefs)", s, pos())
    s = pos(); _i32(buf);          mark("general_settings (ProjectBaseConfig)", s, pos())
    s = pos()
    for _ in range(200): CommonImage.parse(buf)
    mark("common_images[200]", s, pos())
    s = pos(); _bytes(buf, 264);   mark("score_digit_sprites_array (132 × u16)", s, pos())
    s = pos(); _bytes(buf, 28);    mark("char_sel_config (14 × 2B)", s, pos())
    s = pos(); _bytes(buf, 50);    mark("player_selectable_infos[50]", s, pos())
    s = pos(); _bytes(buf, 946);   mark("reserved_tail_946 (all-zero pad)", s, pos())
    trailing = buf.read()
    if trailing:
        mark(f"!! trailing data ({len(trailing)}B)",
             pos() - len(trailing), pos(), "unparsed")

    total = len(data)
    accounted = sum(sz for _,_,_,_,sz in sections)
    print(f"{'OFFSET':>10}  {'END':>10}  {'BYTES':>10}  SECTION")
    for start, end, name, kind, sz in sections:
        prefix = "!! " if kind == "unparsed" else "   "
        print(f"  0x{start:08x}  0x{end:08x}  {sz:10}  {prefix}{name}")
    print()
    print(f"file total:  {total:>10} bytes  ({total/1024/1024:.2f} MiB)")
    print(f"accounted:   {accounted:>10}  ({100*accounted/total:.4f}%)")
    if total != accounted:
        print(f"MISMATCH:    {total - accounted:>10}")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("parse")
    p.add_argument("input"); p.add_argument("-o", "--output", default="-")
    p.add_argument("--decode-blocks", action="store_true",
                   help="add semantic decode of script_items (Xem's Block "
                        "catalog ported to Python)")
    p.set_defaults(fn=cmd_parse)

    p = sub.add_parser("pack")
    p.add_argument("input"); p.add_argument("output")
    p.set_defaults(fn=cmd_pack)

    p = sub.add_parser("verify")
    p.add_argument("input"); p.set_defaults(fn=cmd_verify)

    p = sub.add_parser("info")
    p.add_argument("input"); p.set_defaults(fn=cmd_info)

    p = sub.add_parser("coverage", help="byte-level coverage map")
    p.add_argument("input"); p.set_defaults(fn=cmd_coverage)

    p = sub.add_parser("extract", help="dump sprite frames as BMPs + sounds as WAV/MIDI")
    p.add_argument("input")
    p.add_argument("output", help="output directory")
    p.add_argument("--palette", type=int, default=0,
                   help="shared palette index 0-7 for frames without private palette (default 0)")
    p.set_defaults(fn=cmd_extract)

    args = ap.parse_args()
    rc = args.fn(args)
    sys.exit(rc or 0)


if __name__ == "__main__":
    main()
