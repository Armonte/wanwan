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


def _css_session_line_indices(lines):
    """Indices of the [CSS-FP] lines of each CSS session, in file order.

    Mirrors _css_parity_gate's own session walk (a session ends at the first
    act==(1,1) latch; a reset to (0,0) after a latch opens the next), so an
    injection built on it lands exactly where the gate looks."""
    segs, cur, in_tail, prev = [], [], False, None
    for i, ln in enumerate(lines):
        m = _FP.search(ln)
        if not m:
            continue
        sel, act = m.group(2), (int(m.group(4)), int(m.group(5)))
        if (sel, act) == prev:
            continue
        prev = (sel, act)
        if act == (1, 1):
            in_tail = True
        elif act == (0, 0) and in_tail:
            segs.append(cur); cur, in_tail = [i], False
        elif not in_tail:
            cur.append(i)
    segs.append(cur)
    return [s for s in segs if s]


_TS = re.compile(r'^\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\]')


def _shift_ts(line, secs):
    """Push one log line's [HH:MM:SS.mmm] stamp forward by `secs`."""
    m = _TS.match(line)
    if not m:
        return line
    h, mi, s, ms = (int(x) for x in m.groups())
    t = h * 3600 + mi * 60 + s + ms / 1000.0 + secs
    h2 = int(t // 3600) % 24
    m2 = int(t // 60) % 60
    s2 = t % 60
    return f"[{h2:02d}:{m2:02d}:{s2:06.3f}]" + line[m.end():]


def _tamper(src, dst, kind):
    """Copy every live_FM2K_*.log from src->dst, rewriting one role's [CSS-FP]
    stream to simulate `kind` of divergence."""
    dst.mkdir(parents=True, exist_ok=True)
    for f in src.glob("live_FM2K_*_Debug.log"):
        role = f.name.split("_")[2]              # P1 / P2 / S1 ...
        # ---- LATE TRUNCATION (review H4, 2026-08-18): the GREEN boundary -----
        # The stream dies WELL INSIDE a character-select window rather than 75ms
        # after it opened. Keyed on the session's OPEN timestamp -- the rule G1
        # shipped -- this classifies DEGENERATE and fails FATALLY, which is a
        # false red of exactly the class the classification exists to remove.
        # Keyed on the session's LAST [CSS-FP] it is a TRUNCATION and gets NO
        # VERDICT, while every earlier session still scores.
        #
        # The archived cycle-4 corpus cannot exercise this: its stream died 75ms
        # after the open, so it satisfies BOTH rules. That is why the boundary
        # needed an injection of its own -- the same "proving a detector on the
        # corpus that happens to suit it" trap section 7.1 caught once already.
        if kind == "latetrunc" and role == "S1":
            raw = open(f, errors="ignore").readlines()
            segs = _css_session_line_indices(raw)
            if len(segs) < 2 or len(segs[-1]) < 6:
                return None      # corpus cannot carry this injection
            seg = segs[-1]
            cut = seg[len(seg) // 2]             # deep inside the window
            raw = raw[:cut + 1]
            # ...and make the window demonstrably OLDER than the 2s boundary:
            # the last surviving frame is 3s after the one that opened it, so
            # the open-keyed rule sees 3s (DEGENERATE) and the last-frame-keyed
            # rule sees 0s (TRUNCATED). One injection, both rules addressed.
            raw[-1] = _shift_ts(raw[-1], 3.0)
            with open(dst / f.name, "w") as w:
                w.write("".join(raw))
            continue
        # ---- G1 (2026-08-18) injections: whole-file rewrites -----------------
        if kind in ("zero", "swap") and role == "S1":
            raw = open(f, errors="ignore").readlines()
            if kind == "zero":
                # ZERO SCORABLE SESSIONS. Cut the viewer's log a few frames into
                # its FIRST character-select window: one session, never latched,
                # and it is the last -> classified TRUNCATED, scored == 0. The
                # coverage floor must FAIL rather than report a vacuous pass.
                segs = _css_session_line_indices(raw)
                # Cut CLOSE to the window opening so the session classifies
                # TRUNCATED, not DEGENERATE: this red must be produced by the
                # coverage floor alone (scored == 0), not by the degenerate-
                # session term, or it would not test the floor at all.
                cut = segs[0][min(4, len(segs[0]) - 1)] + 1 if segs else len(raw)
                raw = raw[:cut]
            else:
                # PAIRING DRIFT, in whichever form the corpus can express.
                #
                # >= 3 viewer sessions: exchange the [CSS-FP] payloads of
                # sessions 1 and 2, so each one's best LCS match is the OTHER's
                # host session while the ORDINALS are unchanged.
                #
                # exactly 2: a swap cannot express drift (the pin moves with it
                # and the second session's expectation falls off the end of the
                # host list, which is a coverage fact, not a drift). Repeat
                # session 0's payload into session 1 instead: the ordinals still
                # say "next host session", the evidence still says "the previous
                # one", and that disagreement is precisely what the pinned
                # offset exists to catch.
                #
                # A gate that re-shops the pairing per session comes back GREEN
                # on either form; a gate that pins the offset once must name it.
                segs = _css_session_line_indices(raw)
                if len(segs) >= 3:
                    a, b = segs[1], segs[2]
                    src_a = [raw[i] for i in a]
                    src_b = [raw[i] for i in b]
                    for j, i in enumerate(a):
                        raw[i] = src_b[j % len(src_b)]
                    for j, i in enumerate(b):
                        raw[i] = src_a[j % len(src_a)]
                elif len(segs) == 2:
                    a, b = segs[0], segs[1]
                    src_a = [raw[i] for i in a]
                    for j, i in enumerate(b):
                        raw[i] = src_a[j % len(src_a)]
                else:
                    return None      # corpus cannot carry this injection
            with open(dst / f.name, "w") as w:
                w.write("".join(raw))
            continue
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
                           ("det",  "NONDETERMINISM"),
                           # G1 (2026-08-18): the two ways the term could
                           # previously pass by luck.
                           ("zero", "NO VERDICT COMPUTED"),
                           ("swap", "PAIRING DRIFT")):
            d = _tamper(src, tmp / kind, kind)
            if d is None:
                # NOT a pass. The corpus this self-test was pointed at cannot
                # express this injection (a pairing-drift red needs at least two
                # viewer character-select sessions), so the term is UNTESTED
                # here and says so, loudly, instead of scoring a skip as green.
                print(f"[test] SKIPPED: injected {kind} -- this corpus has too "
                      f"few viewer CSS sessions to express it. Point --logs at "
                      f"a multi-session capture (e.g. the vanpri 16000f leg) to "
                      f"exercise it.")
                continue
            fail, lines = _run(d, f"injected {kind} desync (expect FAIL)")
            if not fail:
                print(f"[test] FAIL: gate MISSED injected {kind} desync"); ok = False
            elif not any(want in l for l in lines):
                print(f"[test] FAIL: {kind} flagged but wrong reason "
                      f"(no '{want}')"); ok = False

        # 6) GREEN BOUNDARY (review H4): a stream that dies 3s INTO a
        # character-select window is a truncation, not a divergence, and must
        # NOT fail. This is the only arm here that asserts the absence of a red,
        # and it is the arm the shipped open-keyed rule fails.
        d = _tamper(src, tmp / "latetrunc", "latetrunc")
        if d is None:
            print("[test] SKIPPED: late-truncation boundary -- this corpus has "
                  "too few / too short viewer CSS sessions to express it. Point "
                  "--logs at a multi-session capture (e.g. the vanpri 16000f "
                  "leg) to exercise it.")
        else:
            fail, lines = _run(d, "late truncation 3s into the window "
                                  "(expect PASS + NO VERDICT)")
            if fail:
                print("[test] FAIL: a stream that died 3s into a CSS window was "
                      "scored as a product failure (the TRUNCATED test is keyed "
                      "on the wrong end of the session)"); ok = False
            elif not any("TRUNCATED" in l and "LAST character-select frame" in l
                         for l in lines):
                print("[test] FAIL: late truncation did not print the "
                      "last-frame-keyed TRUNCATED line"); ok = False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("\n[test] CSS gate self-test:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
