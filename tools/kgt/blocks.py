"""Decode script_item 16-byte opcode records into typed blocks.

Ported from Xem85's fm2ndparser (C#). Each block is 16 bytes:
  byte 0     : opcode (0..37 with gaps; 0 is the "settings" sentinel)
  bytes 1-15 : payload (15 bytes)

Decoded blocks are dicts with `op` (int), `type` (string mnemonic), and
the per-opcode fields. The original 15-byte payload is NOT included
here — kgt.py keeps it via `script_items[*].payload` for byte-exact
round-trip. This module is a purely additive semantic overlay.

Coverage: 25 typed opcodes (M, DS, S, O, E, RC, SF, SG, SC, I, EB, GS,
GL, RP, GC, R, FA, FD, PS, C, V, Rnd, Color, Com, AI) + a "settings"
sentinel (opcode 0). 26 total dispatch entries. Unknown opcodes return
{"type": "unknown", ...}.

References:
- Xem85 fm2ndparser, Blocks/*.cs + BaseParser.cs parse* methods
- common.h SCRIPT_ITEM (16B opaque)
- WW character_state_machine dispatch (per CSM-dispatch-map memory)
"""
from __future__ import annotations

import struct
from typing import Any, Dict


def _b(p: bytes, i: int) -> int:
    return p[i]


def _u16(p: bytes, i: int) -> int:
    return struct.unpack_from("<H", p, i)[0]


def _i16(p: bytes, i: int) -> int:
    return struct.unpack_from("<h", p, i)[0]


def _flag(b: int, n: int) -> bool:
    return bool(b & (1 << n))


_VAR_NAMES = {}
for _i in range(17):
    _VAR_NAMES[_i] = f"Task Var {chr(ord('A') + _i)}"
for _i in range(16):
    _VAR_NAMES[64 + _i] = f"Char Var {chr(ord('A') + _i)}"
for _i in range(16):
    _VAR_NAMES[128 + _i] = f"System Var {chr(ord('A') + _i)}"
_VAR_NAMES.update({
    192: "Data: X coor", 193: "Data: Y coor",
    194: "Data: Map X coor", 195: "Data: Map Y coor",
    196: "Data: Parent X", 197: "Data: Parent Y",
    198: "Data: Time", 199: "Data: No. Rounds",
})


def var_name(v: int) -> str:
    return _VAR_NAMES.get(v, "Unknown")


# Each parser receives the 15B payload (NOT the opcode byte) and returns
# the field dict. Field order follows Xem's parse* methods in
# BaseParser.cs to ease cross-referencing.

def _parse_settings(p: bytes) -> Dict[str, Any]:
    # First block of every skill. NOTE (editor-IDB verified 2026-07-15,
    # docs/dev/2dfm_studio_re_notes.md): Xem's "settings_type" catalog
    # (0,1,3,9,33,57,...,515) is NOT in this payload -- it is the SCRIPT
    # entry's special_flag (editor: KgtScript.roleFlags), which selects
    # the payload layout. For character skills: p[1] = "Sk Level" u8.
    # Timed .kgt scripts (roleFlags & 0x10): u16@0 = display time
    # (100 = 1 sec). Pos rows (& 0x02): i16 X @0, i16 Y @2 (+-999) etc.
    # Full per-roleFlags table in the RE notes.
    return {
        "settings_type_note": "see script.special_flag (roleFlags)",
        "time_or_pos_word": _u16(p, 0),
        "level": p[1],
    }


def _parse_m(p: bytes) -> Dict[str, Any]:
    # 1 — Motion
    f = p[8]
    return {
        "gravity_x": _i16(p, 0), "move_x": _i16(p, 2),
        "move_y": _i16(p, 4), "gravity_y": _i16(p, 6),
        "add": _flag(f, 0),
        "stop_move_x": _flag(f, 1), "stop_move_y": _flag(f, 2),
        "stop_gravity_x": _flag(f, 3), "stop_gravity_y": _flag(f, 4),
    }


def _parse_ds(p: bytes) -> Dict[str, Any]:
    # 2 — Dispatch Skill (DSSkill enum + skill block ref)
    return {
        "when": p[0],
        "skill_number": _u16(p, 1),
        "skill_block": p[3],
    }


def _parse_s(p: bytes) -> Dict[str, Any]:
    # 3 — Sound: just u16 sound index @1. p[0] is NOT a command byte --
    # the editor never writes it (0 in 6886 S blocks across 38 files);
    # loop/type flags live in the sound TABLE entry's sound_type byte
    # (0x10 = loop -> DSBPLAY_LOOPING). Editor-IDB verified 2026-07-15,
    # docs/dev/2dfm_studio_re_notes.md.
    return {
        "always_zero": p[0],
        "sound_number": _u16(p, 1),
    }


def _parse_o(p: bytes) -> Dict[str, Any]:
    # 4 — Object spawn
    f = p[0]
    return {
        "out": _flag(f, 0), "point": _flag(f, 1),
        "uncond": _flag(f, 2), "shadow": _flag(f, 3),
        "parent": _flag(f, 5), "pic_xy": _flag(f, 6),
        "skill_number": _u16(p, 1), "skill_block": p[3],
        "out_skill_number": _u16(p, 4), "out_skill_block": p[6],
        "x": _i16(p, 7), "y": _i16(p, 9),
        "number": p[11], "depth": p[12],
    }


def _parse_e(p: bytes) -> Dict[str, Any]:
    # 5 — Effect (empty body in Xem; placeholder)
    return {}


def _parse_rc(p: bytes) -> Dict[str, Any]:
    # 7 — Render Common (image overlay)
    f = p[0]
    return {
        "in": _flag(f, 0), "turn_x": _flag(f, 2),
        "turn_y": _flag(f, 3), "same": _flag(f, 4),
        "common_image_number": _u16(p, 1),
        "x": _i16(p, 3), "y": _i16(p, 5),
    }


def _parse_sf(p: bytes) -> Dict[str, Any]:
    # 9 — Skill Fork (loop counter + skill block)
    return {
        "loop": p[0],
        "skill_number": _u16(p, 1),
        "skill_block": p[3],
    }


def _parse_sg(p: bytes) -> Dict[str, Any]:
    # 10 — Skill Goto
    return {
        "skill_number": _u16(p, 0),
        "skill_block": p[2],
    }


def _parse_sc(p: bytes) -> Dict[str, Any]:
    # 11 — Skill Call
    return {
        "skill_number": _u16(p, 0),
        "skill_block": p[2],
    }


def _parse_i(p: bytes) -> Dict[str, Any]:
    # 12 — Image draw (Wait u16, then 6-bit-encoded I + flags)
    # Xem's getSplittedData masks low 5 bits of word[1] into the value,
    # upper bits into flags. word = (p[2], p[3]). value = (p[2] | (p[3] & 0x1F) << 8).
    word2 = p[2] | ((p[3] & 0x1F) << 8)
    flags = p[3] & 0xE0  # bits 5,6,7 — Xem only uses 6,7
    flags2 = p[8]
    return {
        "wait": _u16(p, 0),
        "i": word2,
        "turn_x": _flag(flags, 6),
        "turn_y": _flag(flags, 7),
        "x": _i16(p, 4), "y": _i16(p, 6),
        "ignore_direction": _flag(flags2, 0),
    }


def _parse_eb(p: bytes) -> Dict[str, Any]:
    # 14 — Effect Background (fade/shake)
    f = p[7]
    return {
        "fading_type": p[0],
        "rgba": [p[1], p[2], p[3], p[4]],
        "duration": _u16(p, 5),
        "player": _flag(f, 0), "enemy": _flag(f, 1),
        "bg": _flag(f, 2), "system": _flag(f, 3),
        "shake_bg_x": {"type": p[8], "shake": p[9], "duration": p[10]},
        "shake_bg_y": {"type": p[11], "shake": p[12], "duration": p[13]},
    }


def _parse_gs(p: bytes) -> Dict[str, Any]:
    # 16 — Gauge Special (skip 1, then ref + flags + add)
    return {
        "_pad": p[0],
        "skill_number": _u16(p, 1), "skill_block": p[3],
        "is_more": p[4] == 1,
        "level": p[5],
        "add": _i16(p, 6),
    }


def _parse_gl(p: bytes) -> Dict[str, Any]:
    # 17 — Gauge Life (skip 1, then ref + flag + add)
    return {
        "_pad": p[0],
        "skill_number": _u16(p, 1), "skill_block": p[3],
        "is_more": p[4] == 1,
        "add": _i16(p, 5),
    }


def _parse_rp(p: bytes) -> Dict[str, Any]:
    # 20 — Render Particle / hit-junction
    f = p[0]
    return {
        "in": _flag(f, 0), "turn_x": _flag(f, 2),
        "hit_junction_number": _u16(p, 1),
        "x": _i16(p, 3), "y": _i16(p, 5),
    }


def _parse_gc(p: bytes) -> Dict[str, Any]:
    # 21 — Gauge Compare (skip 1, then 4 gauges)
    return {
        "_pad": p[0],
        "player_life_gauge": _i16(p, 1),
        "player_special_gauge": _i16(p, 3),
        "enemy_life_gauge": _i16(p, 5),
        "enemy_special_gauge": _i16(p, 7),
    }


def _parse_r(p: bytes) -> Dict[str, Any]:
    # 23 — Reaction table (6 hit-junction refs)
    return {
        "hits_stand": _u16(p, 0), "hits_crouched": _u16(p, 2),
        "hits_air": _u16(p, 4), "guard_stand": _u16(p, 6),
        "guard_crouched": _u16(p, 8), "guard_air": _u16(p, 10),
    }


def _parse_fa(p: bytes) -> Dict[str, Any]:
    # 24 — Frame Attack (hitbox)
    f = p[9]
    return {
        "x": _i16(p, 0), "y": _i16(p, 2),
        "width": _i16(p, 4), "height": _i16(p, 6),
        "number": p[8],
        "cancel": _flag(f, 0), "combo": _flag(f, 1),
        "halfed": _flag(f, 2), "no_sky_detection": _flag(f, 3),
        "no_detection": _flag(f, 4), "during_guard": _flag(f, 5),
        "guard_fail": _flag(f, 6), "during_receipt": _flag(f, 7),
        "_pad": p[10],
        "power": p[11],
    }


def _parse_fd(p: bytes) -> Dict[str, Any]:
    # 25 — Frame Defence (hurtbox / vulnerability)
    f = p[9]
    return {
        "x": _i16(p, 0), "y": _i16(p, 2),
        "width": _i16(p, 4), "height": _i16(p, 6),
        "number": p[8],
        "collide": _flag(f, 0), "damaged": _flag(f, 1),
        "throw": _flag(f, 2),
        "damage_rate": p[10],
    }


def _parse_ps(p: bytes) -> Dict[str, Any]:
    # 26 — Pause/Stop
    return {"player_time": p[0], "enemy_time": p[1]}


def _parse_c(p: bytes) -> Dict[str, Any]:
    # 30 — Cancel
    f = p[0]
    return {
        "hits": _flag(f, 0), "uncond": _flag(f, 1),
        "skill_cancel_condition": _flag(f, 3),
        "from": p[1],
        "skill_number": _u16(p, 2),  # getSkill (only u16, no block byte)
        "to": p[4],
    }


def _parse_v(p: bytes) -> Dict[str, Any]:
    # 31 — Variable assign / compare
    skill_number = _u16(p, 0)
    skill_block = p[2]
    var = p[3]
    f = p[4]
    its_same = _flag(f, 2)
    its_above = _flag(f, 3)
    return {
        "multi_cond_skill_number": skill_number,
        "multi_cond_skill_block": skill_block,
        "var": var, "var_name": var_name(var),
        "replace": _flag(f, 0), "add": _flag(f, 1),
        "its_the_same": its_same and not its_above,
        "its_above":   its_above and not its_same,
        "its_below":   its_above and its_same,
        "use_even":    _flag(f, 7),
        "use_even_var": p[5], "use_even_var_name": var_name(p[5]),
        "value": _i16(p, 6),
        "multi_cond_value": _i16(p, 8),
    }


def _parse_rnd(p: bytes) -> Dict[str, Any]:
    # 32 — Random branch
    return {
        "random_num": _u16(p, 0),
        "when_its_above": _u16(p, 2),
        "_pad": p[4],
        "skill_number": _u16(p, 5), "skill_block": p[7],
    }


def _parse_color(p: bytes) -> Dict[str, Any]:
    # 35 — Color override
    return {
        "option": p[0],
        "rgba": [p[1], p[2], p[3], p[4]],
    }


def _parse_com(p: bytes) -> Dict[str, Any]:
    # 36 — Command (skill + 5 input steps)
    # Xem reads 5 steps × (getSplittedData2 + 1B flags) = 5 × 2 = 10 bytes
    # of step data, starting at offset 4 (after skill_ref u16+u8 and time).
    steps = []
    for i in range(5):
        # getSplittedData2: 1B where low nibble = value, high nibble = flags
        b = p[4 + i * 2]
        flags1 = b >> 4
        direction = b & 0x0F
        flags2 = p[4 + i * 2 + 1]
        steps.append({
            "direction": direction,
            "a": _flag(flags1, 0), "b": _flag(flags1, 1),
            "c": _flag(flags1, 2), "d": _flag(flags1, 3),
            "e": _flag(flags2, 0), "f": _flag(flags2, 1),
            "continue": _flag(flags2, 4), "active": _flag(flags2, 5),
        })
    return {
        "skill_number": _u16(p, 0), "skill_block": p[2],
        "time": p[3],
        "steps": steps,
    }


def _parse_ai(p: bytes) -> Dict[str, Any]:
    # 37 — Afterimage / AI marker (skip 2, then Num, Time, Option, Fading, Rgba)
    return {
        "_pad0": p[0], "_pad1": p[1],
        "num": p[2], "time": p[3],
        "option": p[4],
        "fading_type": p[5],
        "rgba": [p[6], p[7], p[8], p[9]],
    }


_DISPATCH = {
    0:  ("settings", _parse_settings),
    1:  ("M",       _parse_m),
    2:  ("DS",      _parse_ds),
    3:  ("S",       _parse_s),
    4:  ("O",       _parse_o),
    5:  ("E",       _parse_e),
    7:  ("RC",      _parse_rc),
    9:  ("SF",      _parse_sf),
    10: ("SG",      _parse_sg),
    11: ("SC",      _parse_sc),
    12: ("I",       _parse_i),
    14: ("EB",      _parse_eb),
    16: ("GS",      _parse_gs),
    17: ("GL",      _parse_gl),
    20: ("RP",      _parse_rp),
    21: ("GC",      _parse_gc),
    23: ("R",       _parse_r),
    24: ("FA",      _parse_fa),
    25: ("FD",      _parse_fd),
    26: ("PS",      _parse_ps),
    30: ("C",       _parse_c),
    31: ("V",       _parse_v),
    32: ("Rnd",     _parse_rnd),
    35: ("Color",   _parse_color),
    36: ("Com",     _parse_com),
    37: ("AI",      _parse_ai),
}


def decode_block(opcode: int, payload: bytes) -> Dict[str, Any]:
    """Decode a 16-byte block into a typed dict.

    payload must be exactly 15 bytes (the bytes AFTER the opcode byte).
    Returns {"op": int, "type": str, "fields": dict}. For unrecognized
    opcodes, type is "Unknown" and fields is empty."""
    if len(payload) != 15:
        raise ValueError(f"payload must be 15B, got {len(payload)}")
    entry = _DISPATCH.get(opcode)
    if entry is None:
        return {"op": opcode, "type": "Unknown", "fields": {}}
    type_str, parser = entry
    try:
        fields = parser(payload)
    except Exception as e:
        fields = {"_decode_error": str(e)}
    return {"op": opcode, "type": type_str, "fields": fields}


def known_opcodes() -> Dict[int, str]:
    """Return {opcode_byte: type_mnemonic} for documentation."""
    return {op: name for op, (name, _) in _DISPATCH.items()}
