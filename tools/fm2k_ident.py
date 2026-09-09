#!/usr/bin/env python3
"""Shared 2DFM game identity + engine classification.

THE POINT, and why every earlier attempt was broken: **the .exe is not the
game.** It is a shared FM2K runtime stub, ~1,204,224 bytes, byte-identical
across titles. Measured 2026-09-09 over the local library: 157 installed games
share only 11 distinct exe xxh64 values, and ONE of them (0x506FF9AB93D15134)
covers 86 games. Any dedupe, known-build table or "which game is this" lookup
keyed on the exe hash is broken by construction.

Identity lives in the .kgt. Its first 272 bytes are a 16-byte signature
followed by a NUL-padded CP932 project name the author typed in the editor.
Everything from 272 on is the game-system block (scripts, sprites, palettes,
player/stage slots), so it moves on any content edit.

Two keys, deliberately:

  project_key = xxh64(kgt[0:272])   -- WHICH GAME. Collapses versions of the
                                       same project, because the header is just
                                       (signature, project name). NOTE: it is
                                       exactly that and nothing more, so it does
                                       NOT survive an author renaming a project.
  build_key   = sha256(whole file)  -- WHICH BUILD. Distinguishes releases.

Measured over the 150 local .kgt: 138 distinct project_key, 140 distinct
build_key. Every project_key collision was a genuine duplicate install
(colors x3, AOB x3, ReSHUFFLE x2 ...); the header-equal/body-different pairs
were version variants of one project (HHRTFGRelease01 vs 02Hotfix). Zero
cross-project collisions.

Signatures are MEASURED over all 150 local .kgt, not guessed:
    2DKGT2K 91   2DKGT2G 34   KGTGAME 32
The docstrings this replaces claimed "2DKGT95" for FM95; that string does not
occur in the wild, and "2DKGT2G" was missing entirely.
"""
from __future__ import annotations

import hashlib
import subprocess
import zipfile
from dataclasses import dataclass, asdict, field
from pathlib import Path

import xxhash

KGT_HEADER_LEN = 272
SIG_LEN = 16

SIG_TO_ENGINE = {
    b"2DKGT2K": "FM2K",
    b"2DKGT2G": "FM2K",
    b"KGTGAME": "FM95",
    b"2DKGT95": "FM95",   # listed by FM2K_KgtParser.cpp; never observed
}

# Engine markers inside the .exe, for distributions whose .kgt is not a loose
# file (packed / SFX / single-exe builds). Kept byte-identical to
# launcher/discovery/game_discovery_sniff.cpp::SniffEngineFromStrings so the
# C++ and Python sides cannot disagree.
EXE_MARKERS = [(b"KGT95GAME", "FM95"), (b"KGT2KGAME", "FM2K"),
               (b"2DKGT2K", "FM2K"), (b"kgt2k.INI", "FM2K")]
EXE_SNIFF_BYTES = 256 * 1024

# PE section names -> packer. Same table as DetectPackerFromPE in the launcher.
# A packed game is KEEP-BUT-FLAG: its .kgt may be embedded and unreadable, so
# it must never be silently classified as "not 2DFM" and deleted.
PACKER_SECTIONS = [(b".enigma", "Enigma Protector"), (b"UPX0", "UPX"),
                   (b"UPX1", "UPX"), (b".molebox", "MoleBox"),
                   (b".boom", "MoleBox"), (b"MoleBox", "MoleBox"),
                   (b".aspack", "ASPack"), (b".adata", "ASPack"),
                   (b".Themida", "Themida"), (b".winlice", "Themida"),
                   (b"PEC2", "PECompact"), (b".pec", "PECompact")]

ARCHIVE_SUFFIXES = {".zip", ".lzh", ".lha", ".rar", ".7z", ".tar", ".gz", ".tgz"}


@dataclass
class KgtIdent:
    kgt_sig: str
    engine: str
    project_name: str
    project_key: str
    build_key: str
    kgt_size: int
    inner_path: str = ""

    def to_dict(self) -> dict:
        return asdict(self)


def parse_kgt_header(head: bytes) -> tuple[bytes, str] | None:
    """(signature, project_name) from the first 272 bytes, or None."""
    if len(head) < KGT_HEADER_LEN:
        return None
    sig = head[:SIG_LEN]
    if sig[:7] not in SIG_TO_ENGINE:
        return None
    raw = head[SIG_LEN:KGT_HEADER_LEN].split(b"\x00", 1)[0]
    try:
        name = raw.decode("cp932").strip()
    except UnicodeDecodeError:
        name = raw.decode("cp932", errors="replace").strip()
    return sig, name


def engine_for_sig(sig: bytes) -> str:
    return SIG_TO_ENGINE.get(sig[:7], "FM2K")


def ident_from_bytes(data: bytes, inner_path: str = "") -> KgtIdent | None:
    parsed = parse_kgt_header(data[:KGT_HEADER_LEN])
    if parsed is None:
        return None
    sig, name = parsed
    return KgtIdent(
        kgt_sig=sig[:7].decode("ascii", "replace"),
        engine=engine_for_sig(sig),
        project_name=name,
        project_key="0x%016X" % xxhash.xxh64(data[:KGT_HEADER_LEN], seed=0).intdigest(),
        build_key=hashlib.sha256(data).hexdigest(),
        kgt_size=len(data),
        inner_path=inner_path,
    )


def ident_from_file(path: Path) -> KgtIdent | None:
    try:
        data = path.read_bytes()
    except OSError:
        return None
    return ident_from_bytes(data, inner_path=path.name)


def sniff_exe_engine(data: bytes) -> str | None:
    window = data[:EXE_SNIFF_BYTES]
    for marker, engine in EXE_MARKERS:
        if marker in window:
            return engine
    return None


def detect_packer(data: bytes) -> str:
    window = data[:EXE_SNIFF_BYTES]
    for marker, name in PACKER_SECTIONS:
        if marker in window:
            return name
    return ""


def _seven_zip_list(path: Path) -> list[str]:
    """Member names via `7z l`, or [] when it is not an archive."""
    try:
        r = subprocess.run(["7z", "l", "-ba", "-slt", str(path)],
                           capture_output=True, timeout=180)
    except (OSError, subprocess.SubprocessError):
        return []
    if r.returncode != 0:
        return []
    return [ln[7:].strip() for ln in r.stdout.decode("utf-8", "replace").splitlines()
            if ln.startswith("Path = ")]


def have_rar_tool() -> str:
    """Name of an available RAR extractor, or "" if none."""
    import shutil
    for exe in ("unar", "unrar", "rar"):
        if shutil.which(exe):
            return exe
    return ""


def _rar_extract_one(path: Path, member: str, dest: Path) -> bytes | None:
    """RAR fallback. p7zip cannot decode every RAR method.

    MEASURED 2026-09-09 on atwiki 234 (battle_calling.exe, a RAR SFX): `7z l`
    lists all 52 members happily, but `7z x` fails the .kgt with
    "ERROR: Unsupported Method" AND STILL CREATES A 0-BYTE FILE. Trusting the
    file's existence, or 7z's exit code alone, silently yields empty bytes --
    which is why the caller below validates length as well.
    """
    tool = have_rar_tool()
    if not tool:
        return None
    cmd = ([tool, "-o", str(dest), "-f", str(path), member] if tool == "unar"
           else [tool, "x", "-y", str(path), member, str(dest) + "/"])
    try:
        subprocess.run(cmd, capture_output=True, timeout=600)
    except (OSError, subprocess.SubprocessError):
        return None
    cand = list(dest.rglob(Path(member).name))
    if not cand:
        return None
    try:
        data = cand[0].read_bytes()
    except OSError:
        return None
    return data or None


def _lsar_list(path: Path) -> list[str]:
    """Member names via `lsar`. PREFERRED over 7z for anything non-zip.

    ENCODING IS THE REASON. These archives are Japanese freeware and store
    CP932 filenames. p7zip hands them back as mojibake -- measured on atwiki
    315 (mb.exe, an embedded RAR), 7z reports the member as
        mb/\ufffd\ufffd\ufffd^\ufffd\ufffd\ufffdu\ufffd\ufffd\ufffd[\ufffdh.kgt
    where lsar correctly reports  mb/メタルブレード.kgt . Feeding 7z's mangled
    name back to an extractor matches nothing, so the member silently fails to
    extract and the game looks unreadable.
    """
    import shutil
    if not shutil.which("lsar"):
        return []
    try:
        r = subprocess.run(["lsar", str(path)], capture_output=True, timeout=180)
    except (OSError, subprocess.SubprocessError):
        return []
    if r.returncode != 0:
        return []
    lines = r.stdout.decode("utf-8", "replace").splitlines()
    # first line is "<path>: <format>"; the rest are members
    return [ln.strip() for ln in lines[1:] if ln.strip()]


def list_members(path: Path) -> list[str]:
    """Archive members, lsar first (correct CP932), 7z as fallback."""
    return _lsar_list(path) or _seven_zip_list(path)


def extract_member(path: Path, member: str, dest: Path) -> bytes | None:
    """Extract one member, unar first, then 7z. Never returns empty bytes."""
    data = _rar_extract_one(path, member, dest)
    if data:
        return data
    return _seven_zip_extract_one(path, member, dest)


def _seven_zip_extract_one(path: Path, member: str, dest: Path) -> bytes | None:
    """Extract one member. Returns None unless real, non-empty bytes came out.

    Falls back to a RAR tool when 7z cannot decode the method. Never returns
    b"" -- an empty extraction is a failure, not an empty file.
    """
    try:
        r = subprocess.run(["7z", "x", "-y", f"-o{dest}", str(path), member],
                           capture_output=True, timeout=600)
    except (OSError, subprocess.SubprocessError):
        r = None
    data = None
    if r is not None:
        got = dest / member
        if not got.exists():
            cand = list(dest.rglob(Path(member).name))
            got = cand[0] if cand else None
        if got is not None:
            try:
                data = got.read_bytes()
            except OSError:
                data = None
    if data:
        return data
    return _rar_extract_one(path, member, dest)
