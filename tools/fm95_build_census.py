#!/usr/bin/env python3
"""FM95 engine build census (workplan 0a, docs/dev/fm95_parity_workplan.md).

Every FM95/CPW-engine address constant we use was validated against ONE
binary (the reference CPW.exe). This tool learns whether all FM95 games
ship the same engine build (like FM2K games do) or different builds
(per-build address profiles needed).

Per candidate exe (recognized by the b"KGT95GAME" marker string):
  1. size + md5 (stdlib hashlib)
  2. hand-rolled PE parse -> .text section bytes -> md5 vs reference
  3. 16-byte prologue signature at each hook-target VA vs reference
  4. verdict: IDENTICAL_TEXT / SIGS_OK / SIGS_PARTIAL / DIFFERENT_BUILD

Output: table to stdout + tools/.fm95_census/census_report.txt.

  python3 tools/fm95_build_census.py
  python3 tools/fm95_build_census.py --ref /path/to/CPW.exe --scan /extra/dir

stdlib only. Handles fullwidth/Japanese filenames (UTF-8 str paths on WSL).
"""

import argparse
import hashlib
import os
import struct
import sys

KGT_MARKER = b"KGT95GAME"
SIG_LEN = 16
# Engine exes are roughly 100KB-700KB; the size band is only a guard against
# reading huge files -- the KGT95GAME marker is the real filter. KGT-marker
# exes outside the band are still analyzed, tagged size-odd.
SIZE_MIN = 100 * 1024
SIZE_MAX = 700 * 1024
SIZE_READ_CAP = 16 * 1024 * 1024  # never read files bigger than this

# Hook-target VAs validated against the reference CPW.exe (image base 0x400000).
HOOK_SIGS = [
    (0x408AE0, "get_player_input_p1"),
    (0x408D60, "get_player_input_p2"),
    (0x408FF0, "process_game_inputs"),
    (0x40A060, "update_game_state"),
    (0x40A910, "render_game"),
    (0x40AB60, "WinMain"),
    (0x401FF0, "DispatchScriptSoundCommand"),
    (0x41A864, "rand"),
    (0x4054B0, "LoadStageFile_alt"),
    (0x40B930, "main_window_proc"),
]

REFERENCE_CANDIDATES = [
    "/mnt/c/games/2dfm/fm95/CPW/ＣＰＷ.exe",  # fullwidth CPW
]
REFERENCE_SEARCH_DIRS = [
    "/mnt/c/games/2dfm/fm95",
    "/mnt/c/dev/fm95/CPW",
]
SCAN_DIRS = [
    "/mnt/d/games/fm95",
    "/mnt/c/games/2dfm/fm95",
]

REPORT_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           ".fm95_census", "census_report.txt")


# ---------------------------------------------------------------- PE parsing

class PEImage:
    """Minimal hand-rolled PE view: image base + section table + raw data."""

    def __init__(self, data):
        self.data = data
        self.image_base = None
        self.linker = None  # "major.minor" -- distinguishes build toolchains
        self.sections = []  # (name, vaddr, vsize, rawptr, rawsize, chars)
        self.error = None
        self._parse()

    def _parse(self):
        d = self.data
        if len(d) < 0x40 or d[:2] != b"MZ":
            self.error = "not MZ"
            return
        e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
        if e_lfanew + 24 > len(d) or d[e_lfanew:e_lfanew + 4] != b"PE\0\0":
            self.error = "no PE header"
            return
        coff = e_lfanew + 4
        (_machine, nsections, _ts, _symptr, _nsyms,
         opt_size, _chars) = struct.unpack_from("<HHIIIHH", d, coff)
        opt = coff + 20
        if opt + 2 > len(d):
            self.error = "truncated optional header"
            return
        magic = struct.unpack_from("<H", d, opt)[0]
        self.linker = "%d.%d" % (d[opt + 2], d[opt + 3])
        if magic == 0x10B:      # PE32
            self.image_base = struct.unpack_from("<I", d, opt + 28)[0]
        elif magic == 0x20B:    # PE32+ (never expected for FM95, but be safe)
            self.image_base = struct.unpack_from("<Q", d, opt + 24)[0]
        else:
            self.error = "bad optional-header magic 0x%X" % magic
            return
        sec = opt + opt_size
        for i in range(nsections):
            off = sec + i * 40
            if off + 40 > len(d):
                self.error = "truncated section table"
                return
            name = d[off:off + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", d, off + 8)
            chars = struct.unpack_from("<I", d, off + 36)[0]
            self.sections.append((name, vaddr, vsize, rawptr, rawsize, chars))

    def text_section(self):
        """Raw bytes of .text (name match, else first executable section)."""
        for name, vaddr, vsize, rawptr, rawsize, chars in self.sections:
            if name == ".text":
                return self.data[rawptr:rawptr + rawsize]
        for name, vaddr, vsize, rawptr, rawsize, chars in self.sections:
            if chars & 0x20000000:  # IMAGE_SCN_MEM_EXECUTE
                return self.data[rawptr:rawptr + rawsize]
        return None

    def read_va(self, va, length):
        """Bytes at a virtual address, or None if unmapped/virtual-only."""
        if self.image_base is None:
            return None
        rva = va - self.image_base
        for name, vaddr, vsize, rawptr, rawsize, chars in self.sections:
            span = max(vsize, rawsize)
            if vaddr <= rva < vaddr + span:
                foff = rva - vaddr + rawptr
                if foff + length > rawptr + rawsize:
                    return None  # lands in virtual-only tail
                return self.data[foff:foff + length]
        return None


# ------------------------------------------------------------------- helpers

def md5_hex(data):
    return hashlib.md5(data).hexdigest()


def fingerprint(path):
    """(data, size, md5, pe, text_md5, sigs{name: bytes|None}) for one exe."""
    with open(path, "rb") as f:
        data = f.read()
    pe = PEImage(data)
    text = pe.text_section() if pe.error is None else None
    text_md5 = md5_hex(text) if text is not None else None
    sigs = {}
    for va, name in HOOK_SIGS:
        sigs[name] = pe.read_va(va, SIG_LEN) if pe.error is None else None
    return {
        "path": path,
        "size": len(data),
        "md5": md5_hex(data),
        "pe_error": pe.error,
        "linker": pe.linker,
        "text_md5": text_md5,
        "sigs": sigs,
    }


def collect_candidates(scan_dirs, ref_path):
    """Walk scan dirs for *.exe with the KGT95GAME marker (skip the reference).

    Returns (candidates, skipped_size, skipped_nomarker, searched_dirs).
    candidates = list of (path, size, size_odd_flag).
    """
    candidates = []
    skipped_size = []
    skipped_nomarker = 0
    searched = []
    ref_real = os.path.realpath(ref_path)
    seen = set()
    for root_dir in scan_dirs:
        if not os.path.isdir(root_dir):
            continue
        searched.append(root_dir)
        for dirpath, _dirnames, filenames in os.walk(root_dir):
            for fn in filenames:
                if not fn.lower().endswith(".exe"):
                    continue
                path = os.path.join(dirpath, fn)
                real = os.path.realpath(path)
                if real == ref_real or real in seen:
                    continue
                seen.add(real)
                try:
                    size = os.stat(path).st_size
                except OSError as e:
                    skipped_size.append((path, "stat failed: %s" % e))
                    continue
                if size > SIZE_READ_CAP:
                    skipped_size.append((path, "%d bytes > read cap" % size))
                    continue
                try:
                    with open(path, "rb") as f:
                        data = f.read()
                except OSError as e:
                    skipped_size.append((path, "read failed: %s" % e))
                    continue
                if KGT_MARKER not in data:
                    skipped_nomarker += 1
                    continue
                size_odd = not (SIZE_MIN <= size <= SIZE_MAX)
                candidates.append((path, size, size_odd))
    candidates.sort(key=lambda t: t[0])
    return candidates, skipped_size, skipped_nomarker, searched


def find_reference(explicit):
    if explicit:
        if os.path.isfile(explicit):
            return explicit, [explicit]
        print("error: --ref %s not found" % explicit, file=sys.stderr)
        sys.exit(2)
    tried = list(REFERENCE_CANDIDATES)
    for cand in REFERENCE_CANDIDATES:
        if os.path.isfile(cand):
            return cand, tried
    for d in REFERENCE_SEARCH_DIRS:
        if not os.path.isdir(d):
            tried.append(d + " (missing)")
            continue
        tried.append(d)
        for dirpath, _dirnames, filenames in os.walk(d):
            for fn in filenames:
                if not fn.lower().endswith(".exe"):
                    continue
                path = os.path.join(dirpath, fn)
                try:
                    with open(path, "rb") as f:
                        if KGT_MARKER in f.read():
                            return path, tried
                except OSError:
                    continue
    return None, tried


def verdict_for(fp, ref):
    """(verdict, mismatched-sig-name-list) for one fingerprint vs reference."""
    if fp["pe_error"] is not None:
        return "PARSE_FAIL(%s)" % fp["pe_error"], []
    if fp["text_md5"] is not None and fp["text_md5"] == ref["text_md5"]:
        return "IDENTICAL_TEXT", []
    mismatched = []
    for _va, name in HOOK_SIGS:
        got = fp["sigs"][name]
        want = ref["sigs"][name]
        if got is None or want is None or got != want:
            mismatched.append(name)
    if not mismatched:
        return "SIGS_OK", []
    if len(mismatched) <= len(HOOK_SIGS) // 2:
        return "SIGS_PARTIAL", mismatched
    return "DIFFERENT_BUILD", mismatched


# ---------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description="FM95 engine build census")
    ap.add_argument("--ref", help="reference CPW exe (default: known locations)")
    ap.add_argument("--scan", action="append", default=[],
                    help="extra scan dir (repeatable)")
    args = ap.parse_args()

    ref_path, ref_tried = find_reference(args.ref)
    if ref_path is None:
        print("error: reference CPW exe not found; tried:", file=sys.stderr)
        for t in ref_tried:
            print("  " + t, file=sys.stderr)
        sys.exit(2)

    ref = fingerprint(ref_path)
    if ref["pe_error"] is not None:
        print("error: reference %s failed PE parse: %s"
              % (ref_path, ref["pe_error"]), file=sys.stderr)
        sys.exit(2)
    missing_ref_sigs = [n for n, b in ref["sigs"].items() if b is None]
    if missing_ref_sigs:
        print("error: reference has unmapped hook VAs: %s"
              % ", ".join(missing_ref_sigs), file=sys.stderr)
        sys.exit(2)

    scan_dirs = SCAN_DIRS + args.scan
    candidates, skipped_size, skipped_nomarker, searched = \
        collect_candidates(scan_dirs, ref_path)

    lines = []

    def emit(s=""):
        lines.append(s)

    emit("FM95 engine build census")
    emit("reference: %s" % ref_path)
    emit("  size=%d  md5=%s  .text md5=%s  linker=%s"
         % (ref["size"], ref["md5"], ref["text_md5"], ref["linker"]))
    emit("hook signature VAs (%d bytes each): %s"
         % (SIG_LEN, ", ".join("0x%X %s" % (va, n) for va, n in HOOK_SIGS)))
    emit("scanned dirs: %s" % (", ".join(searched) if searched else "(none exist)"))
    for d in scan_dirs:
        if d not in searched:
            emit("  (missing, not scanned: %s)" % d)
    emit("exe files without %s marker (skipped): %d"
         % (KGT_MARKER.decode(), skipped_nomarker))
    for path, why in skipped_size:
        emit("  skipped %s: %s" % (path, why))
    emit()

    if not candidates:
        emit("NO candidate FM95 exes found beyond the reference.")
        emit("Searched: %s" % ", ".join(scan_dirs))
        report = "\n".join(lines) + "\n"
        os.makedirs(os.path.dirname(REPORT_PATH), exist_ok=True)
        with open(REPORT_PATH, "w", encoding="utf-8") as f:
            f.write(report)
        print(report, end="")
        print("report written: %s" % REPORT_PATH)
        return

    rows = []
    counts = {}
    families = {}  # text_md5 -> [path, ...]  (build family = identical code)
    for path, size, size_odd in candidates:
        try:
            fp = fingerprint(path)
        except OSError as e:
            rows.append((path, size, "????????", "READ_FAIL(%s)" % e, []))
            counts["READ_FAIL"] = counts.get("READ_FAIL", 0) + 1
            continue
        verdict, mismatched = verdict_for(fp, ref)
        if size_odd:
            verdict += " [size-odd]"
        rows.append((path, size, fp["md5"][:8], verdict, mismatched))
        key = verdict.split("(")[0].split(" ")[0]
        counts[key] = counts.get(key, 0) + 1
        if fp["text_md5"] is not None:
            fam = families.setdefault(fp["text_md5"],
                                      {"linker": fp["linker"], "size": size,
                                       "members": []})
            fam["members"].append(path)

    wpath = max(len(r[0]) for r in rows)
    wpath = min(max(wpath, len("path")), 78)
    header = "%-*s  %8s  %-8s  %-16s  %s" % (wpath, "path", "size",
                                             "md5-8", "verdict", "mismatched sigs")
    emit(header)
    emit("-" * len(header))
    for path, size, md5_8, verdict, mismatched in rows:
        emit("%-*s  %8d  %-8s  %-16s  %s"
             % (wpath, path, size, md5_8, verdict,
                ",".join(mismatched) if mismatched else "-"))
    emit()
    emit("verdict distribution: " +
         ", ".join("%s=%d" % (k, v) for k, v in sorted(counts.items())))
    emit()
    emit("build families (distinct .text md5 = distinct engine code;")
    emit("linker = PE optional-header toolchain version -- differing linker")
    emit("means a different-compiler engine release, not a repack/molebox):")
    fam_order = sorted(families.items(),
                       key=lambda kv: (kv[0] != ref["text_md5"],
                                       -len(kv[1]["members"])))
    for text_md5, fam in fam_order:
        tag = " <-- REFERENCE build" if text_md5 == ref["text_md5"] else ""
        emit("  .text %s  size=%d  linker=%s  (%d exes)%s"
             % (text_md5, fam["size"], fam["linker"],
                len(fam["members"]), tag))
        for m in fam["members"]:
            emit("    %s" % m)

    n = len(rows)
    n_same = counts.get("IDENTICAL_TEXT", 0)
    n_sigok = counts.get("SIGS_OK", 0)
    if n_same + n_sigok == n:
        conclusion = ("SINGLE BUILD: all %d candidate exes match the reference "
                      "at every hook VA (%d identical .text, %d data-only "
                      "differences). One address profile suffices." % (n, n_same, n_sigok))
    elif n_same + n_sigok == 0:
        conclusion = ("PER-BUILD PROFILES REQUIRED: none of the %d candidates "
                      "match the reference hook signatures." % n)
    else:
        conclusion = ("MIXED: %d/%d candidates match all hook VAs; the rest "
                      "diverge. Per-build address profiles (or signature "
                      "scanning) needed for the divergent set." % (n_same + n_sigok, n))
    emit("conclusion: " + conclusion)

    report = "\n".join(lines) + "\n"
    os.makedirs(os.path.dirname(REPORT_PATH), exist_ok=True)
    with open(REPORT_PATH, "w", encoding="utf-8") as f:
        f.write(report)
    print(report, end="")
    print("report written: %s" % REPORT_PATH)


if __name__ == "__main__":
    main()
