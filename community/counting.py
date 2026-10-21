"""The counting decision for one match key from every verified receipt the service holds (COMMUNITY_TICKET_SPEC.md
section 7 "Reporting and counting", "Contradictions", "Leaving and dropping"; STATS_DESIGN.md 4.5 with the owner's
2026-10-21 rulings). Pure: no database, no clock. results.py applies the timing (pending window, late reports, the 7-day
close, the pair cap) on top.

Owner ruling (overrides spec "never a W or L"): a left-early match is a rated LOSS for the leaver and a WIN for the
stayer, plus the leaver's `left_early` mark.

Decided because the spec is silent (STATS_DESIGN.md / README record these):
- a signer with two different receipts at one (frame, kind), or whose own scores step back or jump by 2, contradicts
  itself: a strike for that signer and the match is void (disputed on a game with a replay worker);
- a stale or unreachable lone result is struck and dropped; what remains is judged as if it was never sent;
- a stayer's "left early" result counts only when the leaver signed a progress receipt and the stayer's claim is at most
  PROGRESS_GATE + 120 frames past the leaver's latest progress (the gate: a stayer never simulates further), with scores
  not below that progress receipt's.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional

import receipts as R

FLAG_DESYNC, FLAG_LATE, FLAG_PAIR_CAP = 1, 4, 8
COUNTED = ("pending", "final", "left_early")
LEFT_EARLY_SLACK = R.PROGRESS_GATE + R.PLAUSIBILITY_SLACK


@dataclass
class Outcome:
    status: str                      # open pending disputed void left_early dropped
    winner_slot: Optional[int] = None
    rounds: Optional[tuple] = None
    frame: Optional[int] = None
    flags: int = 0
    left_slot: Optional[int] = None
    marks: list = field(default_factory=list)  # (slot, kind)

    def key(self):
        """What a late report may not change (pending and final are one outcome)."""
        status = "pending" if self.status == "final" else self.status
        return (status, self.winner_slot, tuple(self.rounds) if self.rounds else None, self.left_slot)


def _order(r: R.Receipt):
    return (r.frame, r.kind == R.PROGRESS)  # at one frame a result before a progress (the list order)


def _self_consistent(seq: list) -> bool:
    for a, b in zip(seq, seq[1:]):
        if a.frame == b.frame and a.kind == b.kind:
            return False  # two different receipts at one (frame, kind): equivocation
        if not R.score_step_ok(a, b):
            return False
    return True


def evaluate(items: list, rules: R.GameRules, worker: bool) -> Outcome:
    """`items`: verified Receipts of one match (any order, deduplicated by bytes)."""
    by = {0: sorted((r for r in items if r.signer_slot == 0), key=_order),
          1: sorted((r for r in items if r.signer_slot == 1), key=_order)}
    if not all(any(r.kind == R.STARTED for r in by[s]) for s in (0, 1)):
        return Outcome("open")
    marks: list = []
    bad = [s for s in (0, 1) if not _self_consistent(by[s])]
    if bad:
        return Outcome("disputed" if worker else "void", marks=[(s, "strike") for s in bad] +
                       ([(0, "disputed"), (1, "disputed")] if worker else []))
    for a in by[0]:
        for b in by[1]:
            if R.compare_at(a, b) == R.DESYNC:
                if worker:
                    return Outcome("disputed", marks=[(0, "disputed"), (1, "disputed")])
                return Outcome("void")

    def latest_upto(s, frame, results=False):
        best = None
        for r in by[s]:
            if r.frame <= frame and (results or r.kind != R.RESULT):
                best = r
        return best

    def later(s, frame):
        return any(r.frame > frame for r in by[s])

    def reachable_claim(r):
        frm = latest_upto(1 - r.signer_slot, r.frame)
        return frm is not None and R.reachable(frm, r, rules)

    normal = {s: next((r for r in reversed(by[s]) if r.kind == R.RESULT and r.winner_slot != R.WINNER_LEFT_EARLY),
                      None) for s in (0, 1)}
    left = {s: next((r for r in reversed(by[s]) if r.kind == R.RESULT and r.winner_slot == R.WINNER_LEFT_EARLY),
                    None) for s in (0, 1)}

    def counted(r, flags=0):
        return Outcome("pending", r.winner_slot, r.rounds_won, r.frame, flags, marks=marks)

    a, b = normal[0], normal[1]
    if a and b:
        if a.winner_slot == b.winner_slot and a.rounds_won == b.rounds_won:
            same = a.frame == b.frame and a.inputs_chain == b.inputs_chain and a.slot_chain == b.slot_chain \
                and a.state_hash == b.state_hash
            return counted(a if a.frame <= b.frame else b, 0 if same else FLAG_DESYNC)
        stale = {0: later(1, a.frame), 1: later(0, b.frame)}
        if worker and (stale[0] or stale[1]):
            return Outcome("disputed", marks=[(0, "disputed"), (1, "disputed")])
        if stale[0] != stale[1]:
            loser, keep = (0, b) if stale[0] else (1, a)
            marks.append((loser, "strike"))
            return counted(keep) if reachable_claim(keep) else Outcome("void", marks=marks)
        reach = {0: reachable_claim(a), 1: reachable_claim(b)}
        if reach[0] and reach[1]:
            return Outcome("disputed", marks=[(0, "disputed"), (1, "disputed")])
        if reach[0] or reach[1]:
            s = 0 if reach[0] else 1
            marks.append((1 - s, "strike"))
            return counted((a, b)[s])
        return Outcome("void", marks=[(0, "strike"), (1, "strike")])
    for s in (0, 1):
        r = normal[s]
        if r is None:
            continue
        if later(1 - s, r.frame) or not reachable_claim(r):
            if worker and later(1 - s, r.frame):
                return Outcome("disputed", marks=[(0, "disputed"), (1, "disputed")])
            marks.append((s, "strike"))  # stale or impossible: struck, judged as never sent
            break
        return counted(r)  # a lone valid result counts (quitting after the decision changes nothing)
    if left[0] and left[1]:
        return Outcome("dropped", marks=marks)
    for s in (0, 1):
        e = left[s]
        if e is None:
            continue
        o = 1 - s
        prog = [r for r in by[o] if r.kind == R.PROGRESS]
        if not prog:
            return Outcome("open", marks=marks)  # recorded only with the leaver's progress
        last = prog[-1]
        if not (last.frame <= e.frame <= last.frame + LEFT_EARLY_SLACK and R.same_match(last, e)
                and all(e.rounds_won[x] >= last.rounds_won[x] for x in (0, 1))):
            return Outcome("open", marks=marks)
        marks.append((o, "left_early"))
        if last.rounds_won[o] < last.rounds_won[s]:
            marks.append((o, "left_early_behind"))
        return Outcome("left_early", s, e.rounds_won, e.frame, 0, o, marks)
    return Outcome("open", marks=marks)
