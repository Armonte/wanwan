"""receipts.py against the pc-core C++ vectors (tests/community_receipts/receipts_test.cpp and
tests/community_queue/queue_test.cpp on PovertyCaster origin/main): the session key, the chain seeds and frame 119, the
three signed receipts (byte-exact SHA-256 pins), the per-receipt shape, the rules between receipts and the receipt list
bounds (vectors 22, 30-32, 36, 40, 50, 65)."""
import hashlib
from dataclasses import replace

import receipts as R
import tickets as T

SESSION_KEY = "150545847eca83e68f4a9685e19dcbcd9e6279168d8792e31b93b0e16b1d831e"
CHAIN_SEED = "0ee0d31e53d6b95a71d22a0c943c8606e145b2c57c9991fa783b18ffbea27c86"
SLOT_SEED0 = "88b82bbdd14d14c231e2c4b8c5a6fa58095080689ed76d071e860d5bc70ccb93"
CHAIN_119 = "6e4d7c06293177c9eebbe2f4dbc95b851da2860a4ddfa78b4be86fa8bf74d7bd"
SLOT1_119 = "2c61ed6aa4e58939bc8317ba0168a643bca3a34195c05e65a7012e6c451ce488"
PIN_STARTED = "84b477a49387047c003a96c57519c60280028c64f706c2f4b5bad4ce662d0b1d"
PIN_PROGRESS = "664088caf634f54cb934be61e2742829c85f86ed0def20d87283c86b1693a3ae"
PIN_RESULT = "eae678a51442c28f4d50bbb804cabd28289eee15950ad409370eaf01dc0bce0b"

C0, C1 = bytes([2]) * 32, bytes([4]) * 32
PUBS = (T.seed_public(C0), T.seed_public(C1))
STARTED = 1800000000
RULES = R.GameRules()


def key():
    return R.session_key(b"\x11" * 16, b"\x22" * 16, b"\xa0" * 32, b"\xb0" * 32)


def inp(slot, f):
    return (f * 13 + 5) & 0x7FF if slot else (f * 7) & 0x7FF


def sim(f):
    return hashlib.sha256(b"SIM" + T.le(f, 4)).digest()


def chains(frame, tamper=None):
    c = R.InputChains(key(), 3)
    for f in range(frame + 1):
        a, b = inp(0, f), inp(1, f)
        if tamper and f == tamper[0]:
            a, b = (a ^ 1, b) if tamper[1] == 0 else (a, b ^ 1)
        c.feed(f, a, b)
    return c


def mk(kind, signer, frame, w0=0, w1=0, winner=0):
    c = chains(frame)
    return R.Receipt(key(), 3, b"wonderfulworld", b"943e826d7b675e52", kind, signer, STARTED, 2, frame, sim(frame),
                     c.joint, (c.slot[0], c.slot[1]), (w0, w1), winner)


def sig(r):
    return R.sign_receipt(r, (C0, C1)[r.signer_slot])


def check(b, pubs=PUBS, rules=RULES, k=None):
    return R.verify_receipt(b, k or key(), pubs, rules)[0]


def test_session_key_and_chains_pinned():
    assert key().hex() == SESSION_KEY
    assert R.joint_chain_seed(key(), 3).hex() == CHAIN_SEED
    assert R.slot_chain_seed(key(), 3, 0).hex() == SLOT_SEED0
    c = chains(119)
    assert c.last == 119 and c.joint.hex() == CHAIN_119 and c.slot[1].hex() == SLOT1_119
    g = R.InputChains(key(), 3)
    assert not g.feed(1, 0, 0) and g.feed(0, 0, 0) and not g.feed(0, 0, 0) and not g.feed(2, 0, 0)
    t = chains(119, (50, 0))
    assert t.joint != c.joint and t.slot[0] != c.slot[0] and t.slot[1] == c.slot[1]


def test_receipts_pinned_and_round_trip():
    for r, pin in ((mk(R.STARTED, 0, 120), PIN_STARTED), (mk(R.PROGRESS, 1, 720, 1, 0), PIN_PROGRESS),
                   (mk(R.RESULT, 0, 1500, 2, 1, 0), PIN_RESULT)):
        b = sig(r)
        assert hashlib.sha256(b).hexdigest() == pin
        why, out = R.verify_receipt(b, key(), PUBS, RULES)
        assert why == R.OK and out == r and sig(out) == b
    assert len(sig(mk(R.RESULT, 0, 1500, 2, 1, 0))) == 4 + 32 + 2 + 14 + 2 + 16 + 145 + 1 + 64


def test_truncation_signatures_keys():
    b = sig(mk(R.RESULT, 0, 1500, 2, 1, 0))
    for n in range(len(b)):
        assert check(b[:n]) == "unknown_format"
    assert check(b + b"\0") == "unknown_format"
    flip = bytearray(b)
    flip[40] ^= 1
    assert check(bytes(flip)) == "bad_signature"
    assert check(b, (PUBS[1], PUBS[0])) == "bad_signature"
    fields = b[:-64]
    for tag in T.ALL_TAGS:
        assert (check(fields + T.sign(C0, tag, fields)) == R.OK) == (tag == T.TAG_RESULT)
    assert check(b, (PUBS[0], PUBS[0])) == "bad_client_key"
    weak = bytes([1]) + bytes(31)
    assert check(b, (weak, PUBS[1])) == "bad_client_key"
    p = mk(R.PROGRESS, 1, 720, 1, 0)
    assert check(R.sign_receipt(p, C0)) == "bad_signature"  # slot 0's key signing as slot 1
    other = replace(p, session_key=bytes([p.session_key[0] ^ 1]) + p.session_key[1:])
    assert check(sig(other)) == "unknown_key"


def test_game_row_and_shape():
    p = mk(R.PROGRESS, 0, 720, 1, 0)
    assert R.rounds_to_win_legal(1, RULES) and R.rounds_to_win_legal(3, RULES) and not R.rounds_to_win_legal(4, RULES)
    assert check(sig(p), rules=replace(RULES, rounds_to_win_legal=1 << 3)) == "noncanonical"
    assert check(sig(p), rules=replace(RULES, max_match_frames=719)) == "noncanonical"
    assert check(sig(p), rules=replace(RULES, max_match_frames=720)) == R.OK
    for bad in (dict(input_width=0), dict(input_width=5), dict(fps=0), dict(min_round_frames=0),
                dict(max_match_frames=0), dict(rounds_to_win_legal=0), dict(rounds_to_win_legal=0x0F),
                dict(rounds_to_win_legal=0x8000000E)):
        g = replace(RULES, **bad)
        assert not R.rules_valid(g) and check(sig(p), rules=g) == "noncanonical"
    assert R.legal_mask("1,2,3") == 0x0E
    # the shape (raw fields signed by hand, so the encoder's own refusal is bypassed)
    def raw(r):
        f = (r.session_key + T.le(r.match_index, 4) + T.lp(r.game_id) + T.lp(r.build_id) + bytes([r.kind, r.signer_slot])
             + T.le(r.started_unix, 8) + bytes([r.rounds_to_win]) + T.le(r.frame, 4) + r.state_hash + r.inputs_chain
             + r.slot_chain[0] + r.slot_chain[1] + bytes(r.rounds_won) + (bytes([r.winner_slot]) if r.kind == 1 else b""))
        return f + T.sign((C0, C1)[r.signer_slot & 1], T.TAG_RESULT, f)
    assert check(raw(replace(p, kind=3))) == "noncanonical"
    assert check(raw(replace(p, rounds_won=(2, 0)))) == "noncanonical"
    assert check(raw(replace(mk(R.STARTED, 0, 120), frame=121))) == "noncanonical"
    res = mk(R.RESULT, 1, 1500, 1, 2, 1)
    assert check(raw(res)) == R.OK
    assert check(raw(replace(res, winner_slot=0))) == "noncanonical"
    assert check(raw(replace(res, rounds_won=(2, 2), winner_slot=R.WINNER_DRAW))) == R.OK
    assert check(raw(replace(res, rounds_won=(1, 0), winner_slot=R.WINNER_LEFT_EARLY))) == R.OK
    assert check(raw(replace(res, winner_slot=R.WINNER_LEFT_EARLY))) == "noncanonical"
    z = replace(p, state_hash=bytes(32))
    assert check(raw(z)) == R.OK and not R.parse_receipt(raw(z))[1].has_evidence  # vector 22: parses, never counts


def test_rules_between_receipts():
    p0, p1 = mk(R.PROGRESS, 0, 600), mk(R.PROGRESS, 0, 1200, 1, 0)
    assert R.score_step_ok(p0, p1) and not R.score_step_ok(p1, p0)
    draw = mk(R.RESULT, 0, 1800, 2, 2, R.WINNER_DRAW)
    dko = replace(RULES, double_ko=True)
    assert not R.score_rules_ok(draw, RULES) and R.score_rules_ok(draw, dko)
    assert not R.reachable(p1, draw, RULES) and R.reachable(p1, draw, dko)
    frm = mk(R.PROGRESS, 1, 600)
    assert not R.reachable(frm, mk(R.RESULT, 0, 899, 2, 0, 0), RULES)
    assert R.reachable(frm, mk(R.RESULT, 0, 900, 2, 0, 0), RULES)
    other = replace(mk(R.RESULT, 0, 3000, 2, 0, 0), build_id=b"cdc1f90561600ea9")
    assert not R.reachable(frm, other, RULES)  # vector 40
    a, b = mk(R.PROGRESS, 0, 1200, 1, 0), mk(R.PROGRESS, 1, 1200, 1, 0)
    assert R.compare_at(a, b) == R.AGREE
    assert R.compare_at(a, replace(b, state_hash=bytes([b.state_hash[0] ^ 1]) + b.state_hash[1:])) == R.DESYNC
    forged = chains(1200, (900, 0))
    assert R.compare_at(a, replace(b, slot_chain=(forged.slot[0], b.slot_chain[1]))) == R.DESYNC  # vector 36
    assert R.compare_at(a, a) == R.NOT_COMPARABLE
    ra, rb = mk(R.RESULT, 0, 1500, 2, 1, 0), mk(R.RESULT, 1, 1500, 2, 1, 0)
    rs = replace(rb, state_hash=bytes(5) + bytes([rb.state_hash[5] ^ 1]) + rb.state_hash[6:])
    assert R.compare_at(ra, rb) == R.NOT_COMPARABLE
    assert R.same_result_desync_only(ra, rs) and not R.same_result_desync_only(ra, rb)  # vector 50
    assert not R.same_result_desync_only(ra, replace(rb, state_hash=bytes(32)))
    r = mk(R.RESULT, 0, 1500, 2, 0, 0)
    assert R.frame_fits_wall_time(r, STARTED - 105, RULES) and not R.frame_fits_wall_time(r, STARTED - 106, RULES)


def test_list_bounds_vector_65():
    g = RULES
    assert R.max_tape_receipts(g) == 1806 and R.report_body_cap(g) == 928286 and R.tape_cap(g) == 1652382
    p600 = sig(mk(R.PROGRESS, 0, 600))
    r600 = sig(mk(R.RESULT, 0, 600, 2, 0, 0))
    why, v = R.make_list([p600, r600], 10)
    assert why == R.OK and v[0].bytes == r600  # a result sorts before a progress at one frame
    assert R.make_list([p600, p600], 10)[0] == "noncanonical"
    a = sig(replace(mk(R.PROGRESS, 0, 600), state_hash=b"\x01" * 32))
    b = sig(replace(mk(R.PROGRESS, 0, 600), state_hash=b"\x02" * 32))
    assert R.make_list([p600, a], 10)[0] == R.OK
    assert R.make_list([p600, a, b], 10)[0] == "noncanonical"
    assert R.make_list([], 10)[0] == "noncanonical"

    def raw(lst):
        return T.le(len(lst), 2) + b"".join(T.le(len(x), 2) + x for x in lst)
    st = sig(mk(R.STARTED, 0, 120))
    ok = raw([st, p600])
    assert R.parse_list(ok, 10)[0] == R.OK and R.parse_list(ok, 1)[0] == "noncanonical"
    assert R.parse_list(b"\0\0", 10)[0] == "noncanonical"
    assert R.parse_list(raw([p600, st]), 10)[0] == "noncanonical"
    assert R.parse_list(ok + b"\0", 10)[0] == "unknown_format"
    assert R.parse_list(bytes([1, 0, 0x2C, 0x01]) + b"\xee" * 10, 10)[0] == "unknown_format"
    assert R.parse_list(bytes([1, 0, 0, 0]), 10)[0] == "unknown_format"
    assert R.parse_list(bytes([1, 0, 1, 2]) + bytes(513), 10)[0] == "unknown_format"
    assert R.parse_list(raw([b"A" * 100]), 10)[0] != R.OK
    assert R.parse_report_body(bytes(R.report_body_cap(g) + 1), g)[0] == "unknown_format"
    tiny = replace(g, max_match_frames=1200, min_round_frames=1200)
    assert R.max_tape_receipts(tiny) == 12
    _, enc = R.make_list([st, p600, a], 10)
    assert R.parse_list(R.encode_list(enc), 10)[1] == enc
