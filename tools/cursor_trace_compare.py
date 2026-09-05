#!/usr/bin/env python3
"""cursor_trace_compare.py -- join two peers' cursortrace CSVs and report every
(match, frame) where a peer checksum differs, split by which field differed and
by WHICH CANDIDATE CHECKSUM.

THE CLAIM UNDER TEST (docs/dev/desync_false_positive_2026-09-03.md, settled by
artifacts/fingerprint_re_2026-09-03.md). The pre-a3c1173 wire value folded in
two fields the two machines were never required to agree on:

  * 0x470060 -- NOT a round timer. Its only write in the binary (0x40703B)
    copies that peer's own game.ini `Editor.TestPlay.VSMode` in at match init
    and nothing reads it. Per-peer settings state.
  * the two input words, indexed by 0x447EE0 -- a free-running per-process
    engine tick counter, sole writer 0x4146F4, never reset, advanced 0..9 times
    per iteration of a timeGetTime catch-up loop. Not a shared quantity.

THREE ARMS, measured on the SAME rows so the schedule is identical:

  OLD  gameplay_fingerprint -- rng, both hp, 0x470060, 0x470044, both inputs
  NEW  wire_fingerprint     -- rng, both hp, 0x470044                (a3c1173)
  NEW+ NEW plus 0x470048 (g_round_limit) and 0x470050 (round countdown),
       SYNTHESISED HERE from the raw columns. Nothing ships it; this arm exists
       to answer "would adding them be safe" with a measurement instead of an
       argument.

WHAT "THE" CHECKSUM FOR A FRAME MEANS. A frame is saved more than once: the
forward save, then one per resim generation. GekkoNet's SendSessionHealthCheck
(game_session.cpp) sends whatever checksum sits in its state store when the
frame falls out of the prediction window, and `_last_sent_healthcheck` means it
is never re-sent after a later resim overwrites it. So this tool reports three
joins rather than guessing: FIRST (each peer's forward save), LAST (each peer's
final generation), and SETS (the set of distinct values each peer produced).
SETS is the decisive one -- the same set on both sides plus a FIRST/LAST
disagreement means no generation of the simulation diverged.

PHASE-OFFSET TEST. For every cursor-implicated mismatch the tool asks whether
peer A's ring word at its cursor equals peer B's word one slot back or forward.
That separates "the peers hold different input CONTENT" from "the same content
at a different cursor PHASE", from the CSV rather than by inference.

Usage:
  python3 tools/cursor_trace_compare.py P1.csv P2.csv
  python3 tools/cursor_trace_compare.py P1.csv P2.csv --max-report 40
  python3 tools/cursor_trace_compare.py ONE.csv --self   # intra-process control
"""

from __future__ import annotations

import argparse
import struct
import sys
from collections import defaultdict
from pathlib import Path

# Determinism-critical: fields both peers MUST agree on at the same frame.
DET_FIELDS = ["rng", "p1_hp", "p2_hp", "round_number"]
# Per-peer / unsynchronised: the two terms a3c1173 removed from the wire.
BAD_FIELDS = ["vsmode_dead", "p1_word", "p2_word"]
# The proposed additions, measured but not shipped.
CAND_FIELDS = ["round_limit", "round_count"]

INT_COLS = {
    "match", "frame", "seq", "replay", "rb", "netplay_frame", "game_mode",
    "fingerprint", "wire_fp", "rng", "p1_hp", "p2_hp", "vsmode_dead",
    "round_number", "round_limit", "round_count",
    "buf_idx", "idx", "p1_word", "p2_word", "p1_prev", "p2_prev",
    "p1_next", "p2_next",
    # accepted for pre-rename CSVs
    "round_timer", "game_timer",
}
# Older CSVs (before the RE settlement renamed the columns) carry these names.
ALIASES = {"round_timer": "vsmode_dead", "game_timer": "round_number"}


def fletcher32(data: bytes) -> int:
    """Only used for the SYNTHESISED NEW+ arm. It does not need to match the
    hook's Fletcher32 (which is an XXH3 wrapper) -- both peers are hashed with
    the SAME function here, so equality of the synthesised value is what is
    being tested, never its numeric identity with anything in the hook."""
    s1 = 0xFFFF
    s2 = 0xFFFF
    for i in range(0, len(data), 2):
        w = data[i] | (data[i + 1] << 8)
        s1 = (s1 + w) % 0xFFFF
        s2 = (s2 + s1) % 0xFFFF
    return (s2 << 16) | s1


def synth_new_plus(r: dict) -> int:
    return fletcher32(struct.pack("<6I", r["rng"], r["p1_hp"], r["p2_hp"],
                                  r["round_number"], r["round_limit"],
                                  r["round_count"]))


def load(path: Path) -> tuple[list[dict], dict]:
    rows: list[dict] = []
    meta: dict = {}
    cols: list[str] | None = None
    with path.open(encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n").rstrip("\r")
            if not line:
                continue
            if line.startswith("#"):
                for tok in line[1:].split():
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        meta.setdefault(k, v)
                continue
            parts = line.split(",")
            if cols is None:
                cols = [ALIASES.get(c, c) for c in parts]
                continue
            if len(parts) != len(cols):
                continue
            r: dict = {}
            for k, v in zip(cols, parts):
                if k in INT_COLS:
                    try:
                        r[k] = int(v, 0)
                    except ValueError:
                        r[k] = 0
                else:
                    r[k] = v
            r.setdefault("wire_fp", 0)
            r.setdefault("round_limit", 0)
            r.setdefault("round_count", 0)
            r["newplus_fp"] = synth_new_plus(r)
            rows.append(r)
    if cols is None:
        raise SystemExit(f"{path}: no CSV header -- is this a cursortrace file?")
    if "wire_fp" not in cols:
        print(f"  !! {path}: no wire_fp column -- this CSV predates a3c1173, "
              f"so only the OLD arm can be read from it.")
    return rows, meta


def index_by_frame(rows: list[dict]) -> dict[tuple[int, int], list[dict]]:
    idx: dict[tuple[int, int], list[dict]] = defaultdict(list)
    for r in rows:
        idx[(r["match"], r["frame"])].append(r)
    for v in idx.values():
        v.sort(key=lambda r: r["seq"])
    return idx


def classify(a: dict, b: dict, key: str) -> tuple[str, list[str]]:
    """Bucket one (peerA, peerB) pair under one candidate checksum."""
    det = [f for f in DET_FIELDS if a[f] != b[f]]
    bad = [f for f in BAD_FIELDS
           if (a[f] & 0xFFFF if f.endswith("_word") else a[f])
           != (b[f] & 0xFFFF if f.endswith("_word") else b[f])]
    cand = [f for f in CAND_FIELDS if a[f] != b[f]]
    cur = ["buf_idx"] if a["buf_idx"] != b["buf_idx"] else []
    fields = det + bad + cand + cur
    if a[key] == b[key]:
        return ("MATCH", fields)
    if det:
        return ("REAL", fields)
    if key == "newplus_fp" and cand:
        return ("CANDIDATE-ONLY", fields)
    if bad or cur:
        return ("CURSOR-ONLY", fields)
    return ("UNEXPLAINED", fields)


def phase_note(a: dict, b: dict) -> str:
    notes = []
    for p in ("p1", "p2"):
        aw = a[f"{p}_word"] & 0xFFFF
        # "identical" first: a neutral frame stores 0x0000 in every slot, so a
        # prev/next match on a zero word says nothing. Refuse a phase claim
        # about a zero word or every idle frame reads as a one-slot offset.
        if aw == (b[f"{p}_word"] & 0xFFFF):
            notes.append(f"{p}: identical word")
        elif aw == 0 or (b[f"{p}_prev"] & 0xFFFF) == 0 == (b[f"{p}_next"] & 0xFFFF):
            notes.append(f"{p}: differs (zero word, phase undecidable)")
        elif aw == (b[f"{p}_prev"] & 0xFFFF):
            notes.append(f"{p}: A@idx == B@idx-1")
        elif aw == (b[f"{p}_next"] & 0xFFFF):
            notes.append(f"{p}: A@idx == B@idx+1")
        else:
            notes.append(f"{p}: content differs")
    return f"cursor dA-B={a['buf_idx'] - b['buf_idx']:+d} | " + ", ".join(notes)


def fmt(r: dict, key: str) -> str:
    return (f"chk=0x{r[key]:08X} buf={r['buf_idx']} idx={r['idx']} "
            f"p1w=0x{r['p1_word']:08X} p2w=0x{r['p2_word']:08X} "
            f"rng=0x{r['rng']:08X} hp={r['p1_hp']}/{r['p2_hp']} "
            f"vsmode={r['vsmode_dead']} rnum={r['round_number']} "
            f"rlim={r['round_limit']} rcnt={r['round_count']} "
            f"seq={r['seq']} replay={r['replay']} mode={r['game_mode']}")


def ring_census(keys, ia, ib, pick, label: str) -> None:
    """Is the ring difference a cursor PHASE offset or different CONTENT?

    The two have different fixes and the CSV can tell them apart, so it is
    reported rather than assumed. A symmetric A=0/B!=0 vs B=0/A!=0 split is
    speculative-vs-confirmed input, not one peer failing to write a ring."""
    c: dict[str, int] = defaultdict(int)
    for k in keys:
        a, b = pick(ia[k]), pick(ib[k])
        for p in ("p1", "p2"):
            aw, bw = a[f"{p}_word"] & 0xFFFF, b[f"{p}_word"] & 0xFFFF
            if aw == bw:
                c[f"{p} equal"] += 1
            else:
                c[f"{p} DIFFER"] += 1
                c[f"{p} " + ("A=0" if aw == 0 else "B=0" if bw == 0
                             else "both nonzero")] += 1
    print(f"\n=== RING CONTENT at the cursor, {label} ({len(keys)} keys) ===")
    for kk in sorted(c):
        print(f"    {kk:<18} {c[kk]}")


def old_fp(r: dict, vsmode: int) -> int:
    """SaveState_CalculateFingerprint's packed struct, recomputed offline:
    rng, p1_hp, p2_hp, 0x470060, 0x470044, p1_input:u16, p2_input:u16."""
    return fletcher32(struct.pack("<5IHH", r["rng"], r["p1_hp"], r["p2_hp"],
                                  vsmode, r["round_number"],
                                  r["p1_word"] & 0xFFFF, r["p2_word"] & 0xFFFF))


def new_fp(r: dict) -> int:
    return fletcher32(struct.pack("<4I", r["rng"], r["p1_hp"], r["p2_hp"],
                                  r["round_number"]))


def vsmode_counterfactual(keys, ia, ib) -> None:
    """The 0x470060 half, which the loopback harness CANNOT stage.

    Both peers run out of ONE game directory, so they read one game.ini and
    0x470060 is equal on every recorded row. Two peers with different local ini
    files is exactly what this harness cannot build. So hold every measured
    field and change ONLY peer B's 0x470060 -- which the disassembly proves is
    a write-only copy of that peer's own Editor.TestPlay.VSMode (sole write
    0x40703B, zero readers) -- and recount.

    Labelled a COUNTERFACTUAL on purpose: it does not claim the class occurred
    in this run, it quantifies what the old checksum does when the field
    differs, which is the only thing in dispute."""
    print("\n=== 0x470060 COUNTERFACTUAL (not observed in this run: the "
          "harness gives both peers one game.ini) ===")
    for label, pick in (("FIRST", lambda g: g[0]), ("LAST", lambda g: g[-1])):
        od = sum(1 for k in keys
                 if old_fp(pick(ia[k]), pick(ia[k])["vsmode_dead"])
                 != old_fp(pick(ib[k]), pick(ib[k])["vsmode_dead"] + 1))
        nd = sum(1 for k in keys if new_fp(pick(ia[k])) != new_fp(pick(ib[k])))
        print(f"    {label}: B's 0x470060 off by one -> OLD differs on "
              f"{od}/{len(keys)} keys, NEW on {nd}/{len(keys)}")


def report_join(arm: str, key: str, name: str,
                pairs: list[tuple[tuple[int, int], dict, dict]],
                max_report: int) -> dict[str, int]:
    buckets: dict[str, int] = defaultdict(int)
    field_census: dict[str, int] = defaultdict(int)
    shown, lines = 0, []
    for k, a, b in pairs:
        kind, fields = classify(a, b, key)
        buckets[kind] += 1
        if kind == "MATCH":
            continue
        for f in fields:
            field_census[f] += 1
        if shown < max_report:
            shown += 1
            m, fr = k
            lines.append(f"  m{m} f{fr} {kind} fields={','.join(fields) or '(none)'}\n"
                         f"      A {fmt(a, key)}\n      B {fmt(b, key)}\n"
                         f"      {phase_note(a, b)}")
    total = sum(buckets.values())
    bad = total - buckets.get("MATCH", 0)
    print(f"\n--- {arm} / {name}: {total} keys, {bad} mismatched ---")
    for kd in ("CURSOR-ONLY", "CANDIDATE-ONLY", "REAL", "UNEXPLAINED"):
        if buckets.get(kd):
            print(f"    {kd:<15} {buckets[kd]}")
    if field_census:
        print("    fields differing on mismatched keys: " +
              ", ".join(f"{f}={n}" for f, n in
                        sorted(field_census.items(), key=lambda kv: -kv[1])))
    for ln in lines:
        print(ln)
    if bad > shown:
        print(f"    ... {bad - shown} more not shown (--max-report)")
    return buckets


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a", type=Path, help="peer A cursortrace CSV (usually P1)")
    ap.add_argument("b", type=Path, nargs="?",
                    help="peer B cursortrace CSV (usually P2); omit with --self")
    ap.add_argument("--max-report", type=int, default=8)
    ap.add_argument("--self", action="store_true",
                    help="INTRA-process control: for every frame saved more "
                         "than once, every generation must produce the same "
                         "value. A failure here is a determinism bug, not the "
                         "cross-peer class under study.")
    args = ap.parse_args()

    rows_a, meta_a = load(args.a)
    print(f"A {args.a}: {len(rows_a)} rows  header={meta_a}")
    if int(meta_a.get("dropped", "0") or 0) > 0:
        print(f"  !! A DROPPED {meta_a['dropped']} rows -- TRUNCATED trace; an "
              f"absence of mismatches proves nothing.")

    arms = [("OLD  gameplay_fingerprint", "fingerprint"),
            ("NEW  wire_fingerprint", "wire_fp"),
            ("NEW+ wire + 0x470048 + 0x470050 (synthesised)", "newplus_fp")]

    if args.self or args.b is None:
        for path, rows in [(args.a, rows_a)] + (
                [(args.b, load(args.b)[0])] if args.b else []):
            idx = index_by_frame(rows)
            multi = {k: v for k, v in idx.items() if len(v) > 1}
            print(f"\n=== SELF ({path.name}) === frames saved >1x: {len(multi)}")
            for arm, key in arms:
                bad = [k for k, v in multi.items()
                       if len({g[key] for g in v}) > 1]
                cur = [k for k, v in multi.items()
                       if len({g["buf_idx"] for g in v}) > 1]
                print(f"    {arm:<46} generations disagreeing: {len(bad)}"
                      + (f"   e.g. {bad[:5]}" if bad else ""))
            print(f"    cursor differs across generations of a frame: {len(cur)}"
                  + (f"   e.g. {cur[:5]}" if cur else ""))
        if args.b is None:
            return 0

    rows_b, meta_b = load(args.b)
    print(f"B {args.b}: {len(rows_b)} rows  header={meta_b}")
    if int(meta_b.get("dropped", "0") or 0) > 0:
        print(f"  !! B DROPPED {meta_b['dropped']} rows -- TRUNCATED trace.")
    if not rows_a or not rows_b:
        print("\nVERDICT: NO DATA on at least one peer -- either "
              "FM2K_CURSOR_TRACE never reached that process, or it never "
              "reached a dump path.")
        return 2

    ia, ib = index_by_frame(rows_a), index_by_frame(rows_b)
    keys = sorted(set(ia) & set(ib))
    print(f"\ncommon (match,frame) keys: {len(keys)}  "
          f"(A-only {len(set(ia) - set(ib))}, B-only {len(set(ib) - set(ia))})")
    if not keys:
        print("\nVERDICT: NO OVERLAP -- nothing can be compared.")
        return 2

    # The cursor skew itself, independent of any checksum. This is the
    # mechanism's own signature: a per-process tick counter that is never reset
    # should show a LARGE, roughly CONSTANT offset between peers, not drift.
    deltas = defaultdict(int)
    for k in keys:
        deltas[ia[k][0]["buf_idx"] - ib[k][0]["buf_idx"]] += 1
    print("\n=== CURSOR SKEW (A.buf_idx - B.buf_idx, forward saves) ===")
    for d, n in sorted(deltas.items(), key=lambda kv: -kv[1])[:10]:
        print(f"    {d:+6d}  x{n}")

    ring_census(keys, ia, ib, lambda g: g[0], "FIRST (forward save)")
    ring_census(keys, ia, ib, lambda g: g[-1], "LAST (final generation)")
    vsmode_counterfactual(keys, ia, ib)

    first = [(k, ia[k][0], ib[k][0]) for k in keys]
    last = [(k, ia[k][-1], ib[k][-1]) for k in keys]

    results = {}
    for arm, key in arms:
        print(f"\n================ {arm} ================")
        bf = report_join(arm, key, "FIRST (forward save)", first, args.max_report)
        bl = report_join(arm, key, "LAST (final generation)", last, args.max_report)
        same = sum(1 for k in keys
                   if {r[key] for r in ia[k]} == {r[key] for r in ib[k]})
        print(f"\n--- {arm} / SETS: identical set {same}, differing set "
              f"{len(keys) - same} ---")
        results[arm] = {
            "cursor_only": bf.get("CURSOR-ONLY", 0) + bl.get("CURSOR-ONLY", 0),
            "candidate_only": bf.get("CANDIDATE-ONLY", 0) + bl.get("CANDIDATE-ONLY", 0),
            "real": bf.get("REAL", 0) + bl.get("REAL", 0),
            "unexplained": bf.get("UNEXPLAINED", 0) + bl.get("UNEXPLAINED", 0),
            "set_diff": len(keys) - same,
        }

    print("\n================ VERDICT ================")
    for arm, r in results.items():
        tot = r["cursor_only"] + r["candidate_only"] + r["real"] + r["unexplained"]
        print(f"{arm:<46} mismatched keys {tot:<6} "
              f"(cursor-only {r['cursor_only']}, candidate-only "
              f"{r['candidate_only']}, determinism-critical {r['real']}, "
              f"unexplained {r['unexplained']})")
    old = results["OLD  gameplay_fingerprint"]
    new = results["NEW  wire_fingerprint"]
    plus = results["NEW+ wire + 0x470048 + 0x470050 (synthesised)"]

    print()
    if old["cursor_only"] > 0 and old["real"] == 0:
        print("(1) REPRODUCED. The OLD checksum differs at equal frames while "
              "rng, both hp and the round number agree -- the false-positive "
              "class, with only the unsynchronised terms differing.")
    elif old["cursor_only"] > 0:
        print("(1) REPRODUCED, but the run ALSO carries determinism-critical "
              "mismatches; read the two classes apart.")
    else:
        print("(1) NOT REPRODUCED on the OLD checksum in this run.")

    if new["cursor_only"] == 0 and new["unexplained"] == 0:
        print(f"(2) FIX HOLDS. The NEW checksum has 0 cursor-only and 0 "
              f"unexplained mismatches over the same {len(keys)} keys"
              + (f" ({new['real']} determinism-critical remain, which are real "
                 f"divergences and not this class)." if new["real"] else "."))
    else:
        print(f"(2) FIX DOES NOT HOLD: NEW still shows "
              f"{new['cursor_only']} cursor-only / {new['unexplained']} "
              f"unexplained mismatches.")

    extra = plus["candidate_only"] + plus["unexplained"] - new["unexplained"]
    if extra <= 0:
        print("(3) 0x470048 and 0x470050 LOOK SAFE: adding them introduces no "
              "mismatch the NEW checksum did not already have.")
    else:
        print(f"(3) 0x470048 / 0x470050 NOT SAFE as measured: they add {extra} "
              f"mismatched keys that NEW does not have.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
