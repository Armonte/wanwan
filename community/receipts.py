"""Match receipts and the receipt list (COMMUNITY_TICKET_SPEC.md v9.2 sections 7 and 7c). A port of PovertyCaster's
pc-core `Receipt.cpp`, `ReceiptRules.cpp` and `ReceiptList.cpp`: the session key, the PCCHAIN1 / PCSLOT1 input chains,
the started / progress / result layout and its verifier, the rules between receipts, and the receipt list (the report
body) with its per-game bounds. tests/test_receipts.py pins the C++ vectors.

A receipt on the wire is `fields || signature[64]`, the signature being Ed25519(client key, RESULT tag || fields). The
tag is never transmitted; it is the layout's version.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Optional

import tickets as T

STARTED, RESULT, PROGRESS = 0, 1, 2
WINNER_DRAW, WINNER_LEFT_EARLY = 0xFF, 0xFE
STARTED_FRAME = 120
PROGRESS_EVERY = 600
PROGRESS_GATE = 1200
PLAUSIBILITY_SLACK = 120
LIST_RECEIPT_MAX = 512
LIST_ENTRY_MAX = 2 + LIST_RECEIPT_MAX
LIST_PER_KEY_MAX = 2
LIST_STRUCTURAL_MAX = 0xFFFF
SIG = 64

OK = T.OK


# ---- session key and chains ----
def session_key(nonce0: bytes, nonce1: bytes, tid0: bytes, tid1: bytes) -> bytes:
    return T.sha256(nonce0 + nonce1 + tid0 + tid1)


def joint_chain_seed(key: bytes, match_index: int) -> bytes:
    return T.sha256(b"PCCHAIN1" + key + T.le(match_index, 4))


def slot_chain_seed(key: bytes, match_index: int, slot: int) -> bytes:
    return T.sha256(b"PCSLOT1" + key + T.le(match_index, 4) + bytes([slot]))


class InputChains:
    """Both chains over confirmed consumed inputs, frame 0 first and every frame after it."""

    def __init__(self, key: bytes, match_index: int):
        self.last = -1
        self.joint = joint_chain_seed(key, match_index)
        self.slot = [slot_chain_seed(key, match_index, 0), slot_chain_seed(key, match_index, 1)]

    def feed(self, frame: int, in0: int, in1: int) -> bool:
        if frame != self.last + 1:
            return False
        self.joint = T.sha256(self.joint + T.le(frame, 4) + T.le(in0, 4) + T.le(in1, 4))
        for s, v in ((0, in0), (1, in1)):
            self.slot[s] = T.sha256(self.slot[s] + T.le(frame, 4) + T.le(v, 4))
        self.last = frame
        return True


# ---- the per-game row ----
@dataclass(frozen=True)
class GameRules:
    rounds_to_win_legal: int = 0x0E  # bit v set = rounds_to_win v legal (1..30); default 1,2,3
    fps: int = 100
    min_round_frames: int = 300
    double_ko: bool = False
    input_width: int = 2
    max_match_frames: int = 180000


def legal_mask(text: str) -> int:
    """'1,2,3' -> the bit mask. ValueError on anything outside 1..30."""
    m = 0
    for part in str(text).split(","):
        v = int(part.strip())
        if not 1 <= v <= 30:
            raise ValueError(f"rounds_to_win {v} outside 1..30")
        m |= 1 << v
    return m


def rules_valid(g: GameRules) -> bool:
    return (g.fps > 0 and g.min_round_frames > 0 and g.max_match_frames > 0 and 1 <= g.input_width <= 4
            and g.rounds_to_win_legal != 0 and not g.rounds_to_win_legal & 1 and not g.rounds_to_win_legal >> 31)


def rounds_to_win_legal(rtw: int, g: GameRules) -> bool:
    return rules_valid(g) and 1 <= rtw <= 30 and bool((g.rounds_to_win_legal >> rtw) & 1)


# ---- the receipt ----
@dataclass(frozen=True)
class Receipt:
    session_key: bytes
    match_index: int
    game_id: bytes
    build_id: bytes
    kind: int
    signer_slot: int
    started_unix: int
    rounds_to_win: int
    frame: int
    state_hash: bytes
    inputs_chain: bytes
    slot_chain: tuple
    rounds_won: tuple
    winner_slot: int = 0

    @property
    def has_evidence(self) -> bool:
        return any(self.state_hash)


def _shape_ok(r: Receipt) -> bool:
    if not r.game_id or not r.build_id or len(r.game_id) > 0xFFFF or len(r.build_id) > 0xFFFF:
        return False
    if r.signer_slot > 1 or r.rounds_to_win < 1:
        return False
    w0, w1, rtw = r.rounds_won[0], r.rounds_won[1], r.rounds_to_win
    if r.kind == STARTED:
        return r.frame == STARTED_FRAME and w0 == 0 and w1 == 0
    if r.kind == PROGRESS:
        return w0 < rtw and w1 < rtw
    if r.kind == RESULT:
        return {0: w0 == rtw and w1 < rtw, 1: w1 == rtw and w0 < rtw, WINNER_DRAW: w0 == rtw and w1 == rtw,
                WINNER_LEFT_EARLY: w0 < rtw and w1 < rtw}.get(r.winner_slot, False)
    return False


def receipt_fields(r: Receipt) -> bytes:
    """The fields (without the tag). ValueError when the receipt breaks a rule parse_receipt refuses."""
    if not _shape_ok(r):
        raise ValueError("receipt shape")
    b = r.session_key + T.le(r.match_index, 4) + T.lp(r.game_id) + T.lp(r.build_id)
    b += bytes([r.kind, r.signer_slot]) + T.le(r.started_unix, 8) + bytes([r.rounds_to_win]) + T.le(r.frame, 4)
    b += r.state_hash + r.inputs_chain + r.slot_chain[0] + r.slot_chain[1] + bytes(r.rounds_won)
    if r.kind == RESULT:
        b += bytes([r.winner_slot])
    return b


def sign_receipt(r: Receipt, seed: bytes) -> bytes:
    """Tests only (the service never signs a receipt): fields || signature."""
    f = receipt_fields(r)
    return f + T.sign(seed, T.TAG_RESULT, f)


def parse_receipt(p: bytes) -> tuple[str, Optional[Receipt], int]:
    """The structural part: (reason, receipt, signature offset). unknown_format (truncated, trailing) or noncanonical."""
    rd = T._Reader(p, 0)
    key, mi = rd.take(32), rd.le(4)
    gid = rd.take(rd.le(2)) if rd.ok else b""
    bid = rd.take(rd.le(2)) if rd.ok else b""
    kind, signer, started, rtw, frame = rd.u8(), rd.u8(), rd.le(8), rd.u8(), rd.le(4)
    sh, ic, c0, c1, won = rd.take(32), rd.take(32), rd.take(32), rd.take(32), rd.take(2)
    winner = rd.u8() if kind == RESULT else 0
    if not rd.ok or len(p) - rd.at != SIG:
        return "unknown_format", None, 0
    if kind > PROGRESS:
        return "noncanonical", None, 0
    r = Receipt(key, mi, gid, bid, kind, signer, started, rtw, frame, sh, ic, (c0, c1), (won[0], won[1]), winner)
    if not _shape_ok(r):
        return "noncanonical", None, 0
    return OK, r, rd.at


def verify_receipt(p: bytes, key: bytes, slot_pubs: tuple[bytes, bytes], rules: GameRules) -> tuple[str, Optional[Receipt]]:
    """verifyReceiptInSession: the layout, the row (legal rounds_to_win, frame within max_match_frames), this session's
    key, and a signature under the key OF THE RECEIPT'S signer_slot."""
    why, r, at = parse_receipt(p)
    if why != OK:
        return why, None
    if not rounds_to_win_legal(r.rounds_to_win, rules) or r.frame > rules.max_match_frames:
        return "noncanonical", None
    if r.session_key != key:
        return "unknown_key", None
    if slot_pubs[0] == slot_pubs[1]:
        return "bad_client_key", None
    pub = slot_pubs[r.signer_slot]
    if not T.pubkey_strong(pub):
        return "bad_client_key", None
    if not T.verify(pub, T.TAG_RESULT, p[:at], p[at:]):
        return "bad_signature", None
    return OK, r


def signature_ok(p: bytes, pub: bytes) -> bool:
    """The signature alone, under one key (the service's opponent lookup)."""
    why, _, at = parse_receipt(p)
    return why == OK and T.pubkey_strong(pub) and T.verify(pub, T.TAG_RESULT, p[:at], p[at:])


# ---- the rules between receipts ----
def same_match(a: Receipt, b: Receipt) -> bool:
    return (a.session_key == b.session_key and a.match_index == b.match_index and a.game_id == b.game_id
            and a.build_id == b.build_id and a.rounds_to_win == b.rounds_to_win and a.started_unix == b.started_unix)


def score_rules_ok(r: Receipt, g: GameRules) -> bool:
    if not rounds_to_win_legal(r.rounds_to_win, g) or r.frame > g.max_match_frames:
        return False
    return not (r.kind == RESULT and r.winner_slot == WINNER_DRAW and not g.double_ko)


def score_step_ok(earlier: Receipt, later: Receipt) -> bool:
    if not same_match(earlier, later) or later.frame < earlier.frame:
        return False
    return all(0 <= later.rounds_won[s] - earlier.rounds_won[s] <= 1 for s in (0, 1))


def reachable(frm: Receipt, result: Receipt, g: GameRules) -> bool:
    if result.kind != RESULT or not same_match(frm, result) or result.frame < frm.frame or not rules_valid(g):
        return False
    grow = 1 + (result.frame - frm.frame) // g.min_round_frames
    for s in (0, 1):
        d = result.rounds_won[s] - frm.rounds_won[s]
        if d < 0 or d > grow:
            return False
    return score_rules_ok(result, g)


AGREE, DESYNC, NOT_COMPARABLE = "agree", "desync", "not_comparable"


def compare_at(a: Receipt, b: Receipt) -> str:
    if (not same_match(a, b) or a.frame != b.frame or a.signer_slot == b.signer_slot or a.kind == RESULT
            or b.kind == RESULT):
        return NOT_COMPARABLE
    if a.slot_chain[a.signer_slot] != b.slot_chain[a.signer_slot] or \
            b.slot_chain[b.signer_slot] != a.slot_chain[b.signer_slot]:
        return DESYNC
    if a.inputs_chain != b.inputs_chain or a.state_hash != b.state_hash or a.rounds_won != b.rounds_won:
        return DESYNC
    return AGREE


def same_result_desync_only(a: Receipt, b: Receipt) -> bool:
    return (a.kind == RESULT and b.kind == RESULT and a.signer_slot != b.signer_slot and same_match(a, b)
            and a.frame == b.frame and a.inputs_chain == b.inputs_chain and a.slot_chain == b.slot_chain
            and a.winner_slot == b.winner_slot and a.rounds_won == b.rounds_won and a.has_evidence
            and b.has_evidence and a.state_hash != b.state_hash)


def frame_fits_wall_time(r: Receipt, now: int, g: GameRules) -> bool:
    if not rules_valid(g) or now + PLAUSIBILITY_SLACK < r.started_unix:
        return False
    return r.frame <= (now + PLAUSIBILITY_SLACK - r.started_unix) * g.fps


# ---- the receipt list (7c) ----
@dataclass(frozen=True)
class Listed:
    bytes: bytes
    receipt: Receipt = field(compare=False)

    @property
    def key(self):
        r = self.receipt
        return (r.signer_slot, r.frame, r.kind, self.bytes)


def _check_sorted(v: list[Listed], max_receipts: int) -> str:
    if not v or len(v) > max_receipts or len(v) > LIST_STRUCTURAL_MAX:
        return "noncanonical"
    run = 1
    for i, x in enumerate(v):
        if not x.bytes or len(x.bytes) > LIST_RECEIPT_MAX:
            return "noncanonical"
        if i == 0:
            continue
        if not v[i - 1].key < x.key:
            return "noncanonical"  # unsorted, or a byte-identical duplicate
        run = run + 1 if v[i - 1].key[:3] == x.key[:3] else 1
        if run > LIST_PER_KEY_MAX:
            return "noncanonical"
    return OK


def make_list(receipts: list[bytes], max_receipts: int = LIST_STRUCTURAL_MAX) -> tuple[str, list[Listed]]:
    if not receipts or len(receipts) > max_receipts:
        return "noncanonical", []
    v = []
    for b in receipts:
        if not b or len(b) > LIST_RECEIPT_MAX:
            return "unknown_format", []
        why, r, _ = parse_receipt(b)
        if why != OK:
            return why, []
        v.append(Listed(bytes(b), r))
    v.sort(key=lambda x: x.key)
    why = _check_sorted(v, max_receipts)
    return (why, v) if why == OK else (why, [])


def encode_list(v: list[Listed], max_receipts: int = LIST_STRUCTURAL_MAX) -> bytes:
    if _check_sorted(v, max_receipts) != OK:
        raise ValueError("receipt list rules")
    return T.le(len(v), 2) + b"".join(T.le(len(x.bytes), 2) + x.bytes for x in v)


def parse_list(p: bytes, max_receipts: int) -> tuple[str, list[Listed]]:
    """Exactly one list: n in 1..max_receipts first, each entry only after its bytes are known present."""
    if len(p) < 2:
        return "unknown_format", []
    n, at = T.get_le(p, 0, 2), 2
    if n < 1 or n > max_receipts:
        return "noncanonical", []
    v = []
    for _ in range(n):
        if len(p) - at < 2:
            return "unknown_format", []
        ln = T.get_le(p, at, 2)
        if ln < 1 or ln > LIST_RECEIPT_MAX or len(p) - at < 2 + ln:
            return "unknown_format", []
        b = p[at + 2:at + 2 + ln]
        at += 2 + ln
        why, r, _ = parse_receipt(b)
        if why != OK:
            return why, []
        v.append(Listed(b, r))
    why = _check_sorted(v, max_receipts)
    if why != OK:
        return why, []
    if at != len(p):
        return "unknown_format", []
    return OK, v


def max_tape_receipts(g: GameRules) -> int:
    if not rules_valid(g):
        return 0
    f = g.max_match_frames
    m = 2 * (3 + -(-f // PROGRESS_EVERY) + -(-f // g.min_round_frames))
    return min(m, LIST_STRUCTURAL_MAX)


def tape_cap(g: GameRules) -> int:
    if not rules_valid(g):
        return 0
    return g.max_match_frames * 2 * g.input_width + 4096 + 2 + max_tape_receipts(g) * LIST_ENTRY_MAX


def report_body_cap(g: GameRules) -> int:
    return max_tape_receipts(g) * LIST_ENTRY_MAX + 2 if rules_valid(g) else 0


def parse_report_body(p: bytes, g: GameRules) -> tuple[str, list[Listed]]:
    if not rules_valid(g):
        return "noncanonical", []
    if len(p) > report_body_cap(g):
        return "unknown_format", []
    return parse_list(p, max_tape_receipts(g))

