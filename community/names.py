"""Display names and #tags (COMMUNITY_TICKET_SPEC.md section 10).

- name_ok(): the client verifier's rule, a port of pc-core Names.cpp over the same generated table (name_table.py,
  from NameTable.inc). The service never accepts a name a client would refuse.
- normalize_name(): the service side. NFC, strip invisible / bidi / zero-width characters (everything the table bans
  that is not a control), collapse and trim spaces, refuse controls, then name_ok().
- skeleton(): the key the #tag is unique under. A simplified UTS #39 skeleton: NFKD, case-folded, combining marks
  dropped, and common cross-script confusables mapped to Latin. When COMMUNITY_CONFUSABLES points at Unicode's
  confusables.txt, its full prototype map is used instead.

Names are never private: they travel in the ticket and every peer sees Name#1234.
"""
from __future__ import annotations

import bisect
import os
import unicodedata
from functools import lru_cache

from name_table import NAME_BAD, NAME_MARK

NAME_MAX_BYTES, NAME_MAX_SCALARS = 96, 24
_BAD_LO = [lo for lo, _ in NAME_BAD]
_MARK_LO = [lo for lo, _ in NAME_MARK]


def _in(table, los, cp: int) -> bool:
    i = bisect.bisect_right(los, cp) - 1
    return i >= 0 and table[i][0] <= cp <= table[i][1]


def _decode_strict(b: bytes):
    """Strict UTF-8 scalars (no overlongs, surrogates, > U+10FFFF, truncation); None when invalid."""
    out, i = [], 0
    while i < len(b):
        c = b[i]
        if c < 0x80:
            out.append(c)
            i += 1
            continue
        if c & 0xE0 == 0xC0:
            n, cp, lo = 2, c & 0x1F, 0x80
        elif c & 0xF0 == 0xE0:
            n, cp, lo = 3, c & 0x0F, 0x800
        elif c & 0xF8 == 0xF0:
            n, cp, lo = 4, c & 0x07, 0x10000
        else:
            return None
        if i + n > len(b):
            return None
        for j in range(1, n):
            if b[i + j] & 0xC0 != 0x80:
                return None
            cp = (cp << 6) | (b[i + j] & 0x3F)
        if cp < lo or cp > 0x10FFFF or 0xD800 <= cp <= 0xDFFF:
            return None
        out.append(cp)
        i += n
    return out


def name_ok(utf8) -> bool:
    if isinstance(utf8, str):
        try:
            utf8 = utf8.encode("utf-8")
        except UnicodeEncodeError:
            return False
    if not utf8 or len(utf8) > NAME_MAX_BYTES:
        return False
    cps = _decode_strict(utf8)
    if cps is None or len(cps) > NAME_MAX_SCALARS:
        return False
    marks, prev = 0, 0
    for i, cp in enumerate(cps):
        if _in(NAME_BAD, _BAD_LO, cp):
            return False
        if cp == 0x20 and (i == 0 or prev == 0x20):
            return False
        if _in(NAME_MARK, _MARK_LO, cp):
            marks += 1
            if marks > 2 or i == 0:
                return False
        else:
            marks = 0
        prev = cp
    return prev != 0x20


class BadName(ValueError):
    pass


def normalize_name(raw: str) -> str:
    """The service's normalization. Raises BadName with a short reason the launcher can show."""
    if not isinstance(raw, str):
        raise BadName("name must be text")
    s = unicodedata.normalize("NFC", raw)
    kept = []
    for ch in s:
        cp = ord(ch)
        cat = unicodedata.category(ch)
        if cat == "Cc" and ch not in "\t\n\r":
            raise BadName("control characters are not allowed")
        if ch in "\t\n\r" or (cat == "Zs" and ch != " "):
            kept.append(" ")  # any other space becomes a plain space
            continue
        if cp != 0x20 and _in(NAME_BAD, _BAD_LO, cp) and cat in ("Cf", "Mn", "Lo", "So", "Zl", "Zp"):
            continue  # invisible, bidi, zero-width, fillers: stripped
        kept.append(ch)
    s = " ".join("".join(kept).split())
    s = unicodedata.normalize("NFC", s)
    if not s:
        raise BadName("name is empty")
    if len(s) > NAME_MAX_SCALARS:
        raise BadName(f"name is longer than {NAME_MAX_SCALARS} characters")
    if len(s.encode("utf-8")) > NAME_MAX_BYTES:
        raise BadName(f"name is longer than {NAME_MAX_BYTES} bytes")
    if not name_ok(s):
        raise BadName("name has a character that is not allowed")
    return s


# a small built-in confusable map (Cyrillic / Greek look-alikes, digits that read as letters)
_BUILTIN_CONFUSABLES = {
    "0": "o", "1": "l", "i": "l", "|": "l", "5": "s", "$": "s", "@": "a", "rn": "m", "vv": "w",
    "а": "a", "в": "b", "е": "e", "к": "k", "м": "m", "н": "h", "о": "o", "р": "p", "с": "c", "т": "t",
    "у": "y", "х": "x", "і": "l", "ј": "j", "ѕ": "s", "ԁ": "d", "ԛ": "q", "ԝ": "w", "ӏ": "l",
    "α": "a", "β": "b", "ε": "e", "η": "n", "ι": "l", "κ": "k", "ν": "v", "ο": "o", "ρ": "p", "τ": "t",
    "υ": "u", "χ": "x", "ω": "w",
}


@lru_cache(maxsize=1)
def _confusables_file() -> dict[str, str]:
    path = os.environ.get("COMMUNITY_CONFUSABLES", "")
    out: dict[str, str] = {}
    if not path or not os.path.isfile(path):
        return out
    with open(path, encoding="utf-8-sig") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = [p.strip() for p in line.split(";")]
            if len(parts) < 2:
                continue
            src = "".join(chr(int(x, 16)) for x in parts[0].split())
            dst = "".join(chr(int(x, 16)) for x in parts[1].split())
            out[src] = dst
    return out


def skeleton(name: str) -> str:
    table = _confusables_file()
    s = unicodedata.normalize("NFD", name)
    if table:  # UTS #39: NFD, map each character to its prototype, NFD again
        s = "".join(table.get(ch, ch) for ch in s)
        return unicodedata.normalize("NFD", s)
    s = unicodedata.normalize("NFKD", name).casefold()
    s = "".join(ch for ch in s if not unicodedata.combining(ch))
    s = "".join(_BUILTIN_CONFUSABLES.get(ch, ch) for ch in s)
    for multi in ("rn", "vv"):
        s = s.replace(multi, _BUILTIN_CONFUSABLES[multi])
    return s.replace(" ", "")
