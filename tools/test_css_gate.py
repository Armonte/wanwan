#!/usr/bin/env python3
# Negative/positive unit test for the CSS-phase parity gate (#66 Phase 1).
#
# The gate (_css_parity_gate in spec_selftest.py) is the safety net that must
# catch a CSS determinism / spectate / replay break BEFORE CSS gains rollback.
# A gate that can't fail is worthless, so this proves it both ways against the
# REAL function (not a copy):
#   * GREEN on the real captured [CSS-FP] logs from a lockstep run.
#   * RED on three faithful desync injections -- each rewrites the [CSS-FP]
#     payloads exactly as a diverged sim would emit them:
#       1. spectator locks a different character (CSS-SPEC lock-fail)
#       2. spectator navigates a different sel-path      (CSS-SPEC nav-fail)
#       3. guest diverges from host mid-CSS              (CSS-DET fail)
#
# Runs offline against logs left by `spec_selftest.py --keep`; no game launch.
# Usage: python3 tools/test_css_gate.py [--logs DIR]
import argparse, re, shutil, sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from spec_selftest import _css_parity_gate  # the real gate under test

_FP = re.compile(r'(\[CSS-FP\] fr=\d+ in=\S+ cur=\S+ sel=)(\S+)( act=)(\d+)/(\d+)')


def _specs(d):
    return [{"tag": t, "live": d / f"live_FM2K_{t}_Debug.log"}
            for t in ("S1", "S2") if (d / f"live_FM2K_{t}_Debug.log").exists()]


def _run(d, label):
    fail, lines = _css_parity_gate(d, _specs(d))
    print(f"--- {label}: fail={fail} ---")
    for l in lines:
        print("   ", l)
    return fail, lines


def _tamper(src, dst, kind):
    """Copy every live_FM2K_*.log from src->dst, rewriting one role's [CSS-FP]
    stream to simulate `kind` of divergence."""
    dst.mkdir(parents=True, exist_ok=True)
    for f in src.glob("live_FM2K_*_Debug.log"):
        role = f.name.split("_")[2]              # P1 / P2 / S1 ...
        out = []
        touched = 0
        seen_lock = False

        def shift_sel(m):
            # map sel A/B -> A+90/B+90: preserves per-cell distinctness (so the
            # collapsed nav keeps its transition count) while guaranteeing no
            # value collides with a real host cell (0..~50) -> LCS collapses.
            a, b = m.group(2).split("/")
            return f"{m.group(1)}{int(a)+90}/{int(b)+90}{m.group(3)}{m.group(4)}/{m.group(5)}"

        for ln in open(f, errors="ignore"):
            m = _FP.search(ln)
            if m:
                p1, p2 = int(m.group(4)), int(m.group(5))
                if (p1, p2) == (1, 1):
                    seen_lock = True
                if kind == "lock" and role == "S1" and (p1, p2) == (1, 1):
                    # both-confirmed frames -> force a bogus locked char.
                    ln = _FP.sub(r'\g<1>77/77\g<3>\g<4>/\g<5>', ln)
                elif kind == "nav" and role == "S1" and not seen_lock \
                        and (p1, p2) != (1, 1):
                    # session-0 pre-confirm navigation -> shift every sel cell so
                    # the spectator's sel-path can no longer embed in the host's.
                    if 10 <= touched:
                        ln = _FP.sub(shift_sel, ln)
                    touched += 1
                elif kind == "det" and role == "P2":
                    # guest diverges from host on a mid-CSS chunk.
                    if 60 <= touched < 90:
                        ln = _FP.sub(r'\g<1>55/55\g<3>\g<4>/\g<5>', ln)
                    touched += 1
            out.append(ln)
        with open(dst / f.name, "w") as w:
            w.write("".join(out))
    return dst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--logs", default=str(Path(__file__).resolve().parent / ".spec_selftest"))
    args = ap.parse_args()
    src = Path(args.logs)
    host = src / "live_FM2K_P1_Debug.log"
    if not host.exists():
        print(f"[test] no CSS logs at {src} -- run "
              f"`spec_selftest.py --keep --spectators css,battle1` first")
        return 2

    ok = True

    # 1) GREEN on the real lockstep capture.
    fail, lines = _run(src, "REAL lockstep logs (expect PASS)")
    if fail or not any("CSS-DET" in l for l in lines):
        print("[test] FAIL: gate did not pass clean on real lockstep logs"); ok = False

    # 2..4) RED on each injected divergence.
    tmp = Path(tempfile.mkdtemp(prefix="css_gate_neg_"))
    try:
        for kind, want in (("lock", "LOCKED CHAR"),
                           ("nav",  "sel-path diverged"),
                           ("det",  "NONDETERMINISM")):
            d = _tamper(src, tmp / kind, kind)
            fail, lines = _run(d, f"injected {kind} desync (expect FAIL)")
            if not fail:
                print(f"[test] FAIL: gate MISSED injected {kind} desync"); ok = False
            elif not any(want in l for l in lines):
                print(f"[test] FAIL: {kind} flagged but wrong reason "
                      f"(no '{want}')"); ok = False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("\n[test] CSS gate self-test:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
