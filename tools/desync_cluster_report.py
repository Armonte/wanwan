#!/usr/bin/env python3
"""desync_cluster_report -- per-match forensics over pulled fm2k_logs bundles.

For every desync match under tools/.fm2k_logs (or --root), pair the two
peers' bundles and print, per match:

  * game, version, desync frame reported by each peer
  * the Gameplay Fingerprint each peer dumped at detection (hp/rng/timers)
  * the FIRST frame where the two rngtrace.csv streams diverge (rng or
    fingerprint), and how many frames were bit-identical before it
  * whether that first divergence is at/below the confirmed frame from the
    last [BEAT] line (real desync) or in the predicted tail (detector
    false-positive), following the method in project_relay_desync_cluster.

Single-peer bundles are listed with what can be read from one side.
Bundles are read in place; nothing is modified.
"""
import argparse
import json
import re
import sys
import zipfile
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_ROOT = HERE / ".fm2k_logs"

FP_KEYS = ("rng", "buf_idx", "p1_input", "p2_input", "p1_hp", "p2_hp",
           "game_timer", "round_timer")



# A bundle on disk is a meta.json next to ONE ZIP; the logs live inside the
# zip, not beside it. Globbing the directory therefore matched nothing and
# every field in this report came out None -- which read as "both sides
# identical" and hid a whole class of false-positive desyncs on 2026-09-03.
# Read through the archive, and still accept loose files so an already
# extracted bundle works too.
def bundle_texts(bundle: Path) -> dict:
    """{member name: text} for every log/csv in the bundle."""
    cached = _TEXT_CACHE.get(bundle)
    if cached is not None:
        return cached
    out = {}
    for f in bundle.iterdir():
        if f.is_file() and f.suffix.lower() in (".log", ".csv"):
            out[f.name] = f.read_text(errors="replace")
    for z in bundle.glob("*.zip"):
        try:
            with zipfile.ZipFile(z) as zf:
                for name in zf.namelist():
                    if name.lower().endswith((".log", ".csv")):
                        out.setdefault(Path(name).name,
                                       zf.read(name).decode("utf-8", "replace"))
        except (zipfile.BadZipFile, OSError) as e:
            print(f"  WARN  unreadable bundle {z}: {e}", file=sys.stderr)
    _TEXT_CACHE[bundle] = out
    return out


_TEXT_CACHE: dict = {}


def bundle_match(bundle: Path, pattern: str):
    """Texts whose member name matches a glob, in name order.

    NAME BY NUMBER, NOT INDEX: the hook writes FM2K_P1_* for player_index 0
    and FM2K_P2_* for index 1, so never try to select a side by index here.
    The caller already knows which side a bundle is -- meta.json says so.
    """
    import fnmatch
    return [t for n, t in sorted(bundle_texts(bundle).items())
            if fnmatch.fnmatch(n, pattern)]


def load_meta(bundle: Path):
    try:
        return json.loads((bundle / "meta.json").read_text())
    except Exception:
        return None


def parse_fingerprint(bundle: Path):
    out = {}
    for text in bundle_match(bundle, "FM2K_P*_desync_f*.log"):
        m = re.search(r"Local CRC:\s*(0x[0-9A-Fa-f]+)", text)
        if m:
            out["local_crc"] = m.group(1)
        m = re.search(r"Remote CRC:\s*(0x[0-9A-Fa-f]+)", text)
        if m:
            out["remote_crc"] = m.group(1)
        for k in FP_KEYS:
            m = re.search(r"^\s*%s\s*=\s*(\S+)" % re.escape(k), text, re.M)
            if m:
                out[k] = m.group(1)
        break
    return out


def parse_rngtrace(bundle: Path):
    """Return {frame: (rng, fingerprint)} keeping the LAST row per frame.

    rngtrace rows repeat a frame when it was re-simulated; the last row is
    the state that survived. Frames in ascending order only.
    """
    rows = {}
    for text in bundle_match(bundle, "FM2K_P*_rngtrace.csv"):
        for line in text.splitlines():
            if not line or line[0] in "#f":
                continue
            parts = line.split(",")
            if len(parts) < 3:
                continue
            try:
                fr = int(parts[0])
            except ValueError:
                continue
            rows[fr] = (parts[1].strip(), parts[2].strip())
        break
    return rows


REGION_RE = re.compile(r"^\s+(\w[\w()]*)\s+\(0x[0-9A-Fa-f]+ \+\s*\d+\): crc=(0x[0-9A-Fa-f]+)\s*(\[EXCLUDED\])?", re.M)


def parse_regions(bundle: Path):
    """Per-Region CRC (live state at detection time) -> {name: crc}."""
    out = {}
    for text in bundle_match(bundle, "FM2K_P*_desync_f*.log"):
        sect = text.split("=== Per-Region CRC", 1)
        if len(sect) < 2:
            break
        for name, crc, excluded in REGION_RE.findall(sect[1]):
            if not excluded:
                out[name] = crc
        break
    return out


def divergence_persistence(a, b, start):
    """After the first divergent frame: how many later common frames differ
    in rng, and how many differ in fingerprint only."""
    rng_diff = fp_only = same = 0
    for fr in sorted(set(a) & set(b)):
        if fr <= start:
            continue
        if a[fr][0] != b[fr][0]:
            rng_diff += 1
        elif a[fr][1] != b[fr][1]:
            fp_only += 1
        else:
            same += 1
    return rng_diff, fp_only, same


def last_beat(bundle: Path):
    beat = None
    for text in bundle_match(bundle, "FM2K_P*_Debug.log"):
        for line in text.splitlines():
            if "[BEAT]" in line:
                beat = line
    if not beat:
        return {}
    d = dict(re.findall(r"(\w+)=([-+]?[\w.]+)", beat.split("[BEAT]", 1)[1]))
    return d


def first_divergence(a, b):
    common = sorted(set(a) & set(b))
    if not common:
        return None, 0, 0
    identical = 0
    for fr in common:
        if a[fr] == b[fr]:
            identical += 1
            continue
        return fr, identical, len(common)
    return None, identical, len(common)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    ap.add_argument("--version", default=None)
    args = ap.parse_args()

    matches = defaultdict(list)
    for meta_path in args.root.rglob("meta.json"):
        bundle = meta_path.parent
        meta = load_meta(bundle)
        if not meta or meta.get("kind") != "desync":
            continue
        if args.version and meta.get("client_version") != args.version:
            continue
        key = meta.get("match_id") or ("session:" + str(meta.get("session_id")))
        matches[key].append((meta, bundle))

    for key, items in sorted(matches.items(),
                             key=lambda kv: kv[1][0][0].get("timestamp", "")):
        items.sort(key=lambda mb: mb[0].get("player_index", 0))
        m0 = items[0][0]
        print("=" * 78)
        print("match %s  game=%s  version=%s  ts=%s  peers=%d" % (
            key[:12], m0.get("game_id"), m0.get("client_version"),
            m0.get("timestamp"), len(items)))
        fps = []
        traces = []
        regions = []
        for meta, bundle in items:
            fp = parse_fingerprint(bundle)
            beat = last_beat(bundle)
            fps.append(fp)
            traces.append(parse_rngtrace(bundle))
            regions.append(parse_regions(bundle))
            print("  P%d frame=%s  hp=%s/%s rng=%s timer=%s/%s buf=%s in=%s/%s" % (
                meta.get("player_index"), meta.get("frame"),
                fp.get("p1_hp"), fp.get("p2_hp"), fp.get("rng"),
                fp.get("game_timer"), fp.get("round_timer"), fp.get("buf_idx"),
                fp.get("p1_input"), fp.get("p2_input")))
            if beat:
                print("      last BEAT: bf=%s conf=%s ping=%s FA=%s rb_total=%s rb_max=%s delay=%s" % (
                    beat.get("bf"), beat.get("conf"), beat.get("ping"), beat.get("FA"),
                    beat.get("rb_total"), beat.get("rb_max"), beat.get("delay")))
            print("      trace frames=%d  (%s .. %s)" % (
                len(traces[-1]),
                min(traces[-1]) if traces[-1] else "-",
                max(traces[-1]) if traces[-1] else "-"))
        if len(items) == 2:
            a, b = fps
            diffs = [k for k in FP_KEYS if a.get(k) != b.get(k)]
            print("  fingerprint fields differing at detection: %s" % (
                ", ".join(diffs) if diffs else "NONE (both sides identical)"))
            fr, ident, total = first_divergence(traces[0], traces[1])
            if fr is None:
                print("  rngtrace: %d/%d common frames IDENTICAL, no divergence in trace" % (
                    ident, total))
            else:
                ta, tb = traces[0][fr], traces[1][fr]
                print("  rngtrace: first divergence at frame %d after %d identical frames"
                      " (P0 rng=%s fp=%s | P1 rng=%s fp=%s)" % (
                          fr, ident, ta[0], ta[1], tb[0], tb[1]))
                # show the two frames before it for context
                for f2 in (fr - 2, fr - 1):
                    if f2 in traces[0] and f2 in traces[1]:
                        print("      f%d: P0 %s/%s  P1 %s/%s" % (
                            f2, traces[0][f2][0], traces[0][f2][1],
                            traces[1][f2][0], traces[1][f2][1]))
                rd, fo, sm = divergence_persistence(traces[0], traces[1], fr)
                print("      after it: %d frames rng differs, %d fingerprint-only, %d identical" % (
                    rd, fo, sm))
            ra, rb = regions
            if ra and rb:
                names = [n for n in ra if n in rb]
                diff = [n for n in names if ra[n] != rb[n]]
                same = [n for n in names if ra[n] == rb[n]]
                print("  regions DIFFER at detection: %s" % (", ".join(diff) if diff else "none"))
                print("  regions identical: %s" % ", ".join(same))
    return 0


if __name__ == "__main__":
    sys.exit(main())
