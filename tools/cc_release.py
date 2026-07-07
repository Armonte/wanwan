#!/usr/bin/env python3
"""
cc_release.py -- Pokemon: Close Combat release/update tooling.

Two sides of the CC update system live here:

  AUTHORING (Altilt runs this): `build` turns a new game-build folder into a full
  zip + a delta zip (from the previous build) + a manifest fragment, with all the
  integrity metadata baked in. One command per release.

  APPLYING (reference impl): `apply` is the exact algorithm the launcher must use
  to apply a full or delta zip onto an install, with pre/post hash verification and
  protected-file handling. The C++/C# launcher reimplements this logic 1:1; this CLI
  is the spec + a working repair/verify tool.

Pure stdlib (zipfile/hashlib/json) -- runs anywhere, no deps.

FORMAT
  full zip   : the entire build tree + `version.json` { version, files:{path:sha256} }
  delta zip  : only added/modified files (at their real paths) + `_delta.json`:
               { from_version, to_version, changes:[ {path, op, from_sha256?, to_sha256?} ] }
               op = add | modify | delete
  manifest   : (on the VPS) cc_manifest.json -- latest + full{url,size,sha256}
               + deltas[ {from,to,url,size,sha256} ]. URLs are MediaFire file_premium links.

PROTECTED PATHS (never shipped/overwritten/deleted by an update -- user/launcher-owned):
  LilithPort.ini, game.ini, Replays/**, pkmnccgv.txt, pkmncclv.txt, *.log
  (game.ini is rewritten by the launcher from settings; LilithPort.ini holds the kid's
   name + lobby; Replays/ is their data. The launcher manages those separately.)
"""
import argparse, hashlib, json, os, sys, zipfile, fnmatch, tempfile, shutil

SCHEMA = 1
VERSION_JSON = "version.json"
DELTA_JSON = "_delta.json"
DEFAULT_PROTECT = ["LilithPort.ini", "game.ini", "pkmnccgv.txt", "pkmncclv.txt",
                   "Replays/*", "Replays/**", "*.log"]


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def is_protected(relpath, protect):
    rp = relpath.replace("\\", "/")
    for pat in protect:
        if fnmatch.fnmatch(rp, pat) or fnmatch.fnmatch(rp, pat + "/*"):
            return True
    return False


def walk_tree(root):
    """relpath(forward-slash) -> sha256 for every file under root."""
    out = {}
    for dirpath, _dirs, files in os.walk(root):
        for name in files:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace("\\", "/")
            out[rel] = sha256_file(full)
    return out


def zip_size_sha(path):
    return os.path.getsize(path), sha256_file(path)


# ---------------------------------------------------------------- byte-level patch (zstd --patch-from)
# A `modify` change can ship as a `patch` op: a zstd binary diff of old->new, applied against the
# on-disk old file. libzstd reconstructs it on the launcher side (we already vendor zstd). Only used
# when worthwhile (file big enough AND patch meaningfully smaller than shipping the whole file).
PATCH_MIN_BYTES = 256 * 1024   # don't bother patching tiny files (overhead not worth it)
PATCH_RATIO     = 0.80         # use a patch only if it's < this fraction of the whole new file
ZSTD_LONG       = "--long=27"  # long-distance matching window (~128MB); needed on BOTH ends

_ZSTD = shutil.which("zstd")


def zstd_ok():
    return _ZSTD is not None


def make_zstd_patch(old_path, new_path, out_path):
    """Write a zstd patch (old->new) to out_path. Returns its size, or None on failure."""
    import subprocess
    try:
        subprocess.run([_ZSTD, "-q", "-19", ZSTD_LONG, "--patch-from=" + old_path,
                        new_path, "-o", out_path, "-f"], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return os.path.getsize(out_path)
    except Exception:
        return None


def apply_zstd_patch(old_path, patch_path, out_path):
    """Reconstruct new (out_path) from old + patch. Raises on failure."""
    import subprocess
    subprocess.run([_ZSTD, "-q", "-d", ZSTD_LONG, "--patch-from=" + old_path,
                    patch_path, "-o", out_path, "-f"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# ---------------------------------------------------------------- build

def cmd_build(args):
    protect = args.protect or DEFAULT_PROTECT
    new_root = args.tree
    if not os.path.isdir(new_root):
        sys.exit(f"--tree not a dir: {new_root}")
    os.makedirs(args.out, exist_ok=True)

    print(f"[*] hashing new build '{args.version}' ...")
    new_files = walk_tree(new_root)
    print(f"    {len(new_files)} files")

    # ---- full zip (entire tree + version.json) ----
    full_name = f"pkmncc_{args.version}_full.zip"
    full_path = os.path.join(args.out, full_name)
    version_doc = {"schema": SCHEMA, "version": args.version, "files": new_files}
    with zipfile.ZipFile(full_path, "w", zipfile.ZIP_DEFLATED) as z:
        for rel in sorted(new_files):
            z.write(os.path.join(new_root, rel), rel)
        z.writestr(VERSION_JSON, json.dumps(version_doc, indent=2, sort_keys=True))
    fsize, fsha = zip_size_sha(full_path)
    print(f"[+] full  -> {full_name}  ({fsize:,} B, sha256 {fsha[:16]}...)")

    manifest = {"schema": SCHEMA, "latest": args.version,
                "full": {"version": args.version, "url": "<PASTE_MEDIAFIRE_FULL_URL>",
                         "size": fsize, "sha256": fsha},
                "deltas": []}

    # ---- delta zip (vs previous build) ----
    prev_root = args.prev_tree
    tmp_extract = None
    if not prev_root and args.prev_zip:
        tmp_extract = tempfile.mkdtemp(prefix="cc_prev_")
        with zipfile.ZipFile(args.prev_zip) as z:
            z.extractall(tmp_extract)
        # the prev full zip carries version.json at root -- ignore it as a game file
        if os.path.exists(os.path.join(tmp_extract, VERSION_JSON)):
            os.remove(os.path.join(tmp_extract, VERSION_JSON))
        prev_root = tmp_extract

    if prev_root:
        if not args.prev_version:
            sys.exit("--prev-version required when building a delta")
        print(f"[*] diffing vs previous '{args.prev_version}' ...")
        prev_files = walk_tree(prev_root)
        changes = []
        patch_tmp = {}   # relpath -> temp .zpatch path (for `patch` ops)
        use_patch = zstd_ok() and not args.no_patch
        for rel, sha in sorted(new_files.items()):
            if is_protected(rel, protect):
                continue
            if rel not in prev_files:
                changes.append({"path": rel, "op": "add", "to_sha256": sha})
            elif prev_files[rel] != sha:
                new_path = os.path.join(new_root, rel)
                new_size = os.path.getsize(new_path)
                made_patch = False
                if use_patch and new_size >= PATCH_MIN_BYTES:
                    tmp = tempfile.mktemp(suffix=".zpatch")
                    psize = make_zstd_patch(os.path.join(prev_root, rel), new_path, tmp)
                    if psize is not None and psize < new_size * PATCH_RATIO:
                        changes.append({"path": rel, "op": "patch", "codec": "zstd",
                                        "from_sha256": prev_files[rel], "to_sha256": sha,
                                        "patch_size": psize})
                        patch_tmp[rel] = tmp
                        made_patch = True
                    elif os.path.exists(tmp):
                        os.remove(tmp)
                if not made_patch:
                    changes.append({"path": rel, "op": "modify",
                                    "from_sha256": prev_files[rel], "to_sha256": sha})
        for rel, sha in sorted(prev_files.items()):
            if is_protected(rel, protect):
                continue
            if rel not in new_files:
                changes.append({"path": rel, "op": "delete", "from_sha256": sha})

        adds = [c for c in changes if c["op"] == "add"]
        mods = [c for c in changes if c["op"] == "modify"]
        pats = [c for c in changes if c["op"] == "patch"]
        dels = [c for c in changes if c["op"] == "delete"]
        delta_name = f"pkmncc_{args.prev_version}_to_{args.version}_delta.zip"
        delta_path = os.path.join(args.out, delta_name)
        delta_doc = {"schema": SCHEMA, "from_version": args.prev_version,
                     "to_version": args.version, "changes": changes}
        with zipfile.ZipFile(delta_path, "w", zipfile.ZIP_DEFLATED) as z:
            for c in adds + mods:
                z.write(os.path.join(new_root, c["path"]), c["path"])
            for c in pats:
                z.write(patch_tmp[c["path"]], c["path"] + ".zpatch")
            z.writestr(DELTA_JSON, json.dumps(delta_doc, indent=2, sort_keys=True))
        for t in patch_tmp.values():
            if os.path.exists(t):
                os.remove(t)
        dsize, dsha = zip_size_sha(delta_path)
        print(f"[+] delta -> {delta_name}  ({dsize:,} B, sha256 {dsha[:16]}...)")
        print(f"    +{len(adds)} added  ~{len(mods)} modified  Δ{len(pats)} patched  "
              f"-{len(dels)} deleted  (delta is {100*dsize//max(fsize,1)}% of full)")
        manifest["deltas"].append({"from": args.prev_version, "to": args.version,
                                   "url": "<PASTE_MEDIAFIRE_DELTA_URL>",
                                   "size": dsize, "sha256": dsha})
        if tmp_extract:
            shutil.rmtree(tmp_extract, ignore_errors=True)

    frag = os.path.join(args.out, "cc_manifest.fragment.json")
    with open(frag, "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"\n[=] manifest fragment -> {frag}")
    print("    NEXT: upload the zip(s) to MediaFire, copy each file_premium link,")
    print("          paste them over the <PASTE_*> placeholders, push to the VPS.")
    return 0


# ---------------------------------------------------------------- apply

class ApplyError(Exception):
    pass


def _read_json_from_zip(z, name):
    try:
        return json.loads(z.read(name))
    except KeyError:
        return None


def cmd_apply(args):
    protect = args.protect or DEFAULT_PROTECT
    install = args.install
    os.makedirs(install, exist_ok=True)
    with zipfile.ZipFile(args.zip) as z:
        delta = _read_json_from_zip(z, DELTA_JSON)
        full = _read_json_from_zip(z, VERSION_JSON)
        if delta:
            _apply_delta(z, delta, install, protect, args.dry_run)
            applied = delta["to_version"]
        elif full:
            _apply_full(z, full, install, protect, args.dry_run)
            applied = full["version"]
        else:
            raise ApplyError("zip has neither _delta.json nor version.json")
    print(f"[+] install now at version: {applied}")
    return 0


def _apply_delta(z, delta, install, protect, dry):
    changes = delta["changes"]
    if any(c["op"] == "patch" for c in changes) and not zstd_ok():
        raise ApplyError("delta has zstd `patch` ops but zstd is unavailable -- fall back to FULL.")
    # ---- PRE-CHECK: every modify/patch target must currently match its from_sha256 ----
    bad = []
    for c in changes:
        if c["op"] not in ("modify", "patch"):
            continue
        p = os.path.join(install, c["path"])
        if not os.path.exists(p) or sha256_file(p) != c["from_sha256"]:
            bad.append(c["path"])
    if bad:
        raise ApplyError(
            f"install is not a clean '{delta['from_version']}' -- {len(bad)} file(s) "
            f"differ from expected (e.g. {bad[:3]}). Fall back to FULL download.")
    if dry:
        print(f"[dry] delta {delta['from_version']}->{delta['to_version']}: "
              f"{sum(c['op']=='add' for c in changes)} add, "
              f"{sum(c['op']=='modify' for c in changes)} modify, "
              f"{sum(c['op']=='patch' for c in changes)} patch, "
              f"{sum(c['op']=='delete' for c in changes)} delete")
        return
    # ---- APPLY: extract adds/whole-modifies, reconstruct patches, then deletes ----
    for c in changes:
        if c["op"] in ("add", "modify"):
            if is_protected(c["path"], protect) and os.path.exists(os.path.join(install, c["path"])):
                continue  # never clobber existing protected/user files
            _extract_member(z, c["path"], install)
        elif c["op"] == "patch":
            _apply_patch_member(z, c, install)
    for c in changes:
        if c["op"] == "delete":
            p = os.path.join(install, c["path"])
            if is_protected(c["path"], protect):
                continue
            if os.path.exists(p):
                os.remove(p)
    # ---- POST-CHECK: every written file must now match to_sha256 ----
    bad = []
    for c in changes:
        if c["op"] in ("add", "modify", "patch"):
            p = os.path.join(install, c["path"])
            if is_protected(c["path"], protect):
                continue
            if not os.path.exists(p) or sha256_file(p) != c["to_sha256"]:
                bad.append(c["path"])
    if bad:
        raise ApplyError(f"post-apply verify FAILED for {len(bad)} file(s) ({bad[:3]}). "
                         f"Install is inconsistent -- do a FULL download to self-heal.")


def _apply_patch_member(z, change, install):
    """Reconstruct a `patch` op: old install file + zstd patch -> new, written same-dir (atomic)."""
    rel = change["path"]
    old = os.path.join(install, rel)
    tmp_patch = old + ".zpatch.tmp"
    tmp_new = old + ".new.tmp"
    try:
        with open(tmp_patch, "wb") as f:
            f.write(z.read(rel + ".zpatch"))
        apply_zstd_patch(old, tmp_patch, tmp_new)
        os.replace(tmp_new, old)   # same filesystem -> atomic swap
    finally:
        for t in (tmp_patch, tmp_new):
            if os.path.exists(t):
                os.remove(t)


def _apply_full(z, full, install, protect, dry):
    want = full["files"]  # path -> sha256
    if dry:
        print(f"[dry] full {full['version']}: {len(want)} files")
        return
    # extract everything (seed protected only if absent)
    for name in z.namelist():
        if name in (VERSION_JSON, DELTA_JSON) or name.endswith("/"):
            continue
        if is_protected(name, protect) and os.path.exists(os.path.join(install, name)):
            continue
        _extract_member(z, name, install)
    # remove stale managed files not in this version (preserve protected + user data)
    for dirpath, _dirs, files in os.walk(install):
        for nm in files:
            rel = os.path.relpath(os.path.join(dirpath, nm), install).replace("\\", "/")
            if rel in want or is_protected(rel, protect):
                continue
            os.remove(os.path.join(dirpath, nm))
    # POST-CHECK managed files
    bad = [rel for rel, sha in want.items()
           if not is_protected(rel, protect)
           and (not os.path.exists(os.path.join(install, rel))
                or sha256_file(os.path.join(install, rel)) != sha)]
    if bad:
        raise ApplyError(f"post-apply verify FAILED for {len(bad)} file(s) ({bad[:3]}).")


def _extract_member(z, name, install):
    dest = os.path.join(install, name)
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    with z.open(name) as src, open(dest, "wb") as out:
        shutil.copyfileobj(src, out)


# ---------------------------------------------------------------- verify

def cmd_verify(args):
    with open(args.version_json) as f:
        doc = json.load(f)
    want = doc["files"]
    protect = args.protect or DEFAULT_PROTECT
    missing, wrong = [], []
    for rel, sha in want.items():
        if is_protected(rel, protect):
            continue
        p = os.path.join(args.install, rel)
        if not os.path.exists(p):
            missing.append(rel)
        elif sha256_file(p) != sha:
            wrong.append(rel)
    if missing or wrong:
        print(f"[!] install does NOT match {doc['version']}: "
              f"{len(missing)} missing, {len(wrong)} wrong")
        for r in (missing + wrong)[:20]:
            print("    -", r)
        return 1
    print(f"[+] install verified == {doc['version']} ({len(want)} files)")
    return 0


def main():
    ap = argparse.ArgumentParser(description="CC release/update tooling")
    sub = ap.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("build", help="produce full + delta zips + manifest fragment")
    b.add_argument("--tree", required=True, help="new build folder")
    b.add_argument("--version", required=True, help="new version id, e.g. v17a")
    b.add_argument("--prev-tree", help="previous build folder (for delta)")
    b.add_argument("--prev-zip", help="previous full zip (alt to --prev-tree)")
    b.add_argument("--prev-version", help="previous version id")
    b.add_argument("--out", default="cc_out", help="output dir")
    b.add_argument("--no-patch", action="store_true", help="disable zstd byte-level patches (whole files only)")
    b.add_argument("--protect", nargs="*", help="glob(s) of user/launcher-owned paths")
    b.set_defaults(func=cmd_build)

    a = sub.add_parser("apply", help="apply a full or delta zip onto an install (reference impl)")
    a.add_argument("--install", required=True)
    a.add_argument("--zip", required=True)
    a.add_argument("--dry-run", action="store_true")
    a.add_argument("--protect", nargs="*")
    a.set_defaults(func=cmd_apply)

    v = sub.add_parser("verify", help="check an install against a version.json")
    v.add_argument("--install", required=True)
    v.add_argument("--version-json", required=True)
    v.add_argument("--protect", nargs="*")
    v.set_defaults(func=cmd_verify)

    args = ap.parse_args()
    try:
        sys.exit(args.func(args))
    except ApplyError as e:
        print(f"[FAIL] {e}", file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
