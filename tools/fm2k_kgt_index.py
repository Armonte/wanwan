#!/usr/bin/env python3
"""Index every 2DFM game we hold, keyed on the .kgt -- the dedupe oracle.

Spans all four byte stores: the playable library, and the archive stores for
atwiki / freem / IA. Emits one row per PHYSICAL OCCURRENCE of a .kgt, so the
same game found in three places produces three rows sharing a project_key.

CONTAINER-AWARE, because the bytes are not always a loose .kgt:
  loose        <dir>/Game.kgt
  zip          read the member's first 272 bytes from the central directory;
               ~0.03s per archive, no extraction
  lzh/rar/7z   `7z l` to find .kgt members, extract only those to a tempdir
  sfx-exe      a lot of FM2K distributions ship as a single self-extracting
               .exe (25-700 MB rather than the ~1.1 MB stub). `7z l` lists
               real game files -> treat exactly like the matching archive.
  packed-exe   Enigma / UPX / MoleBox / ASPack / Themida / PECompact. The .kgt
               is embedded and usually unreadable. These are KEEP-BUT-FLAG:
               recorded as engine-from-exe-strings if the markers survive
               packing, else verdict PACKED with no identity. They must never
               be reported as "not 2DFM", which is what would get them deleted.

Nothing is ever deleted or moved by this tool. Read-only over the byte stores.

Usage:
    python3 tools/fm2k_kgt_index.py                    # full rebuild
    python3 tools/fm2k_kgt_index.py --report-dupes     # duplicate groups
    python3 tools/fm2k_kgt_index.py --root DIR --archive DIR --out PATH
"""
from __future__ import annotations

import argparse
import json
import sys
import tempfile
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fm2k_ident as I  # noqa: E402

REPO = Path(__file__).resolve().parents[1]
DEFAULT_ROOTS = ["/mnt/d/Games/fm2k", "/mnt/d/Games/fm95"]
DEFAULT_ARCHIVES = ["/mnt/d/fm2k-archive"]
DEFAULT_OUT = REPO / "data" / "identity" / "kgt_index.json"


def row(ident: I.KgtIdent, *, container: str, location: str,
        archive_path: str = "", source: str = "", packer: str = "") -> dict:
    d = ident.to_dict()
    d.update(container=container, location=location, archive_path=archive_path,
             source=source, packer=packer)
    return d


def index_loose(roots: list[Path]) -> list[dict]:
    out = []
    for root in roots:
        if not root.exists():
            print(f"  skip (missing): {root}")
            continue
        n = 0
        for kgt in root.rglob("*"):
            if kgt.suffix.lower() != ".kgt" or not kgt.is_file():
                continue
            ident = I.ident_from_file(kgt)
            if ident is None:
                continue
            out.append(row(ident, container="local",
                           location=str(kgt).replace("\\", "/"), source="local"))
            n += 1
        print(f"  {root}: {n} .kgt")
    return out


def index_zip(path: Path, source: str) -> tuple[list[dict], str]:
    rows = []
    try:
        with zipfile.ZipFile(path) as z:
            members = [m for m in z.namelist() if m.lower().endswith(".kgt")]
            for m in members:
                try:
                    with z.open(m) as fh:
                        head = fh.read(I.KGT_HEADER_LEN)
                except (KeyError, OSError, zipfile.BadZipFile):
                    continue
                if I.parse_kgt_header(head) is None:
                    continue
                try:
                    with z.open(m) as fh:
                        data = fh.read()
                except (OSError, zipfile.BadZipFile):
                    continue
                ident = I.ident_from_bytes(data, inner_path=m)
                if ident:
                    rows.append(row(ident, container="archive-zip",
                                    location=f"{path}!{m}",
                                    archive_path=str(path), source=source))
    except (zipfile.BadZipFile, OSError):
        return [], "unreadable-zip"
    return rows, "" if rows else "no-kgt"


def index_via_7z(path: Path, source: str, container: str) -> tuple[list[dict], str]:
    names = I.list_members(path)
    if not names:
        return [], "not-an-archive"
    kgts = [n for n in names if n.lower().endswith(".kgt")]
    if not kgts:
        return [], "no-kgt"
    rows, failed = [], 0
    with tempfile.TemporaryDirectory(prefix="kgtidx_") as td:
        for m in kgts:
            data = I.extract_member(path, m, Path(td))
            if not data:
                failed += 1
                continue
            ident = I.ident_from_bytes(data, inner_path=m)
            if ident:
                rows.append(row(ident, container=container,
                                location=f"{path}!{m}",
                                archive_path=str(path), source=source))
    if rows:
        return rows, ""
    # LISTED a .kgt but could not read it out. That is NOT "not a 2DFM game" --
    # it is an extractor gap, and calling it not-2DFM is how real games get
    # thrown away. Name it so a human can fix the toolchain instead.
    if failed:
        return [], ("unreadable-archive-install-unar" if not I.have_rar_tool()
                    else "unreadable-archive")
    return [], "no-kgt"


def index_archives(archives: list[Path]) -> tuple[list[dict], list[dict]]:
    rows, problems = [], []
    for base in archives:
        if not base.exists():
            print(f"  skip (missing): {base}")
            continue
        files = [p for p in base.rglob("*") if p.is_file()]
        print(f"  {base}: {len(files)} files")
        for p in files:
            suf = p.suffix.lower()
            source = f"{base.name}:{p.parent.name}"
            if suf == ".zip":
                r, why = index_zip(p, source)
            elif suf in I.ARCHIVE_SUFFIXES:
                r, why = index_via_7z(p, source, "archive-7z")
            elif suf == ".exe":
                r, why = index_via_7z(p, source, "archive-sfx-exe")
                # Only fall through to the packed/plain-exe checks when this
                # genuinely is not an archive. An SFX we LISTED but could not
                # extract keeps its own verdict -- overwriting it with
                # "not-2dfm-exe" is exactly how a real game gets discarded.
                if not r and why.startswith("unreadable-archive"):
                    problems.append({"path": str(p), "verdict": why,
                                     "source": source})
                    continue
                if not r:
                    # Not an SFX. Could still be a packed single-exe build --
                    # flag it, never call it not-2DFM.
                    try:
                        data = p.read_bytes()
                    except OSError:
                        data = b""
                    packer = I.detect_packer(data)
                    engine = I.sniff_exe_engine(data)
                    if engine or packer:
                        problems.append({"path": str(p), "verdict":
                                         "PACKED" if packer else "EXE_ONLY",
                                         "packer": packer, "engine": engine,
                                         "source": source})
                        continue
                    # SIZE IS THE TELL. A stock FM2K runtime stub is
                    # ~1,204,224 bytes. Anything far larger is a distribution
                    # in a container we could not open (measured: four 25-67 MB
                    # PE32 files that `7z l` cannot list at all and that carry
                    # no packer section). Saying "not 2DFM" about those is a
                    # guess that loses games, so say what is actually true --
                    # we could not identify it -- and let a human or a better
                    # extractor decide.
                    try:
                        big = p.stat().st_size > 4 * 1024 * 1024
                    except OSError:
                        big = False
                    why = ("oversized-exe-needs-extractor" if big
                           else "not-2dfm-exe")
            else:
                continue
            rows.extend(r)
            if why:
                problems.append({"path": str(p), "verdict": why, "source": source})
    return rows, problems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", action="append", default=None)
    ap.add_argument("--archive", action="append", default=None)
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--report-dupes", action="store_true")
    args = ap.parse_args()

    roots = [Path(r) for r in (args.root or DEFAULT_ROOTS)]
    archives = [Path(a) for a in (args.archive or DEFAULT_ARCHIVES)]

    t0 = time.time()
    print("scanning local roots...")
    rows = index_loose(roots)
    print("scanning archive stores...")
    arch_rows, problems = index_archives(archives)
    rows.extend(arch_rows)

    projects: dict[str, dict] = {}
    for r in rows:
        p = projects.setdefault(r["project_key"], {
            "project_name": r["project_name"], "engine": r["engine"],
            "builds": {}, "locations": []})
        p["builds"].setdefault(r["build_key"], r["kgt_size"])
        p["locations"].append(r["location"])

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(".tmp")
    tmp.write_text(json.dumps({
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "roots": [str(r) for r in roots], "archives": [str(a) for a in archives],
        "n_rows": len(rows), "n_projects": len(projects),
        "rows": rows, "problems": problems,
    }, ensure_ascii=False, indent=2), encoding="utf-8")
    tmp.replace(out)

    proj_out = out.parent / "projects.json"
    tmp2 = proj_out.with_suffix(".tmp")
    tmp2.write_text(json.dumps(projects, ensure_ascii=False, indent=2),
                    encoding="utf-8")
    tmp2.replace(proj_out)

    print(f"\n{len(rows)} .kgt occurrences, {len(projects)} distinct projects "
          f"({time.time()-t0:.1f}s)")
    print(f"  problems/flags: {len(problems)}")
    print(f"  -> {out}\n  -> {proj_out}")

    if args.report_dupes:
        print("\nduplicate groups (same project, >1 location):")
        for k, p in sorted(projects.items(), key=lambda kv: -len(kv[1]["locations"])):
            if len(p["locations"]) > 1:
                print(f"  {p['project_name'][:36]:36s} {len(p['locations'])} copies, "
                      f"{len(p['builds'])} build(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
