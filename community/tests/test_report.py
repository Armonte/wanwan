"""POST /v1/report: verification, the counting state machine with the owner's rulings, the answer classes (7c) and the
results.jsonl export (STATS_DESIGN.md 4.4). Spec vectors 22, 29, 33, 40, 46, 49, 64, 65 (the service halves)."""
import re

import receipts as R
from conftest import DISCORD_ID, NOW
from report_fixture import (BUILD, CA, CB, CX, WBUILD, body, export_lines, latest, mk, report, setup, signed_as,
                            started_pair)

P, S, RES, LEFT = R.PROGRESS, R.STARTED, R.RESULT, R.WINNER_LEFT_EARLY


def win_a(**kw):
    """A (slot 0) wins 2-1 at frame 1500; both sides' started and progress 600 / 1200."""
    return started_pair(**kw) + [mk(P, 0, 600, **kw), mk(P, 1, 600, **kw), mk(P, 0, 1200, (1, 1), **kw),
                                 mk(P, 1, 1200, (1, 1), **kw), mk(RES, 0, 1500, (2, 1), 0, **kw)]


def test_lone_report_counts_pending_and_exports(h, tmp_path):
    ta, tb, a, b = setup(h, tmp_path)
    h.clock.t = NOW + 600
    r = report(h, CA, ta, body(*win_a()))
    assert r.status_code == 200, r.text
    j = r.json()
    assert j["status"] == "pending" and re.fullmatch(r"[0-9a-f]{12}", j["match_id"])
    row = latest(tmp_path)[j["match_id"]]
    assert row["game_id"] == "fm2k:wonderfulworld" and row["family"] == "fm2k" and row["build"] == BUILD
    assert row["winner"] == "p1" and row["score"] == [2, 1] and row["status"] == "pending" and row["flags"] == []
    assert row["p1"] == {"public_id": a["public_id"], "name": "Alice", "tag": a["tag"], "hidden": False, "char": None}
    assert row["p2"]["public_id"] == b["public_id"] and row["p2"]["name"] == "Bob"
    assert row["finished_at"] == NOW + 60 + 15 and row["started_at"] == NOW + 60
    # vector 64: the same body again (new nonce) is a no-op: same answer, no new export line, no new report row
    n = len(export_lines(tmp_path))
    again = report(h, CA, ta, body(*win_a()))
    assert again.json() == j and len(export_lines(tmp_path)) == n
    assert h.community.db.one("SELECT COUNT(*) FROM reports")[0] == 1
    # the other side's agreeing report changes nothing; one differing only in state_hash adds the desync flag
    rb = started_pair() + [mk(P, 0, 600), mk(P, 1, 600), mk(P, 0, 1200, (1, 1)), mk(P, 1, 1200, (1, 1)),
                           mk(RES, 1, 1500, (2, 1), 0, salt=7)]
    assert report(h, CB, tb, body(*rb)).json() == {"status": "pending", "match_id": j["match_id"]}
    assert latest(tmp_path)[j["match_id"]]["flags"] == ["desync"]
    text = (tmp_path / "export" / "results.jsonl").read_text()
    assert DISCORD_ID not in text and "account" not in text
    for resp in h.responses:
        assert DISCORD_ID.encode() not in resp.content


def test_settles_final_and_a_late_claim_never_flips(h, tmp_path):
    ta, tb, _, _ = setup(h, tmp_path)
    h.clock.t = NOW + 600
    mid = report(h, CA, ta, body(*win_a())).json()["match_id"]
    h.clock.t += 86400
    assert h.app.state.results.settle() == 1
    assert latest(tmp_path)[mid]["status"] == "final"
    # B (no worker game) later claims its own win at the same frame: reachable, but late -> flag + mark, W stays
    rb = started_pair() + [mk(P, 0, 600), mk(P, 1, 600), mk(P, 0, 1200, (1, 1)), mk(P, 1, 1200, (1, 1)),
                           mk(RES, 1, 1500, (1, 2), 1)]
    r = report(h, CB, tb, body(*rb))
    assert r.json()["status"] == "final"
    row = latest(tmp_path)[mid]
    assert row["winner"] == "p1" and row["flags"] == ["late_claim"]
    marks = {(m["kind"]) for m in h.community.db.q("SELECT kind FROM marks")}
    assert "late_claim" in marks
    h.clock.t = NOW + 60 + 8 * 86400
    assert report(h, CB, tb, body(*rb[:-1])).json()["error"] == "too_late"


def test_refusals(h, tmp_path):
    ta, tb, _, _ = setup(h, tmp_path)
    h.clock.t = NOW + 600
    self_play = [signed_as(CA, x) for x in win_a()]
    r = report(h, CA, ta, body(*self_play))
    assert (r.status_code, r.json()["error"]) == (403, "self_play")
    anon = [x if R.parse_receipt(x)[1].signer_slot == 0 else signed_as(CX, x) for x in win_a()]
    r = report(h, CA, ta, body(*anon))
    assert (r.status_code, r.json()["error"]) == (403, "anonymous_peer")
    zero = started_pair() + [mk(P, 0, 600, zero=True)]
    assert report(h, CA, ta, body(*zero)).json()["error"] == "no_evidence"
    r = report(h, CA, ta, body(*win_a(build="ffffffffffffffff")))
    assert (r.status_code, r.json()["error"]) == (422, "unknown_game")
    r = report(h, CA, ta, body(mk(S, 0, 120), mk(P, 0, 600)))
    assert (r.status_code, r.json()["error"]) == (422, "no_started_pair")
    mixed = started_pair() + [mk(P, 1, 600, n=1)]
    assert report(h, CA, ta, body(*mixed)).json()["error"] == "bad_receipt"
    assert report(h, CA, ta, b"\x00\x00").json()["error"] == "bad_receipt"
    # a third player: neither a signer nor (later) a participant
    c = h.link(CX, name="Carol", code="third-code").json()
    import base64
    tc = base64.b64decode(c["ticket"])
    assert report(h, CX, tc, body(*win_a())).json()["error"] == "not_participant"
    report(h, CA, ta, body(*win_a()))
    assert report(h, CX, tc, body(*win_a()[:-1])).json()["error"] == "not_participant"
    # the body cap is checked from Content-Length before a byte is read (vector 65)
    big = report(h, CA, ta, b"\0" * (928286 + 1))
    assert (big.status_code, big.json()["error"]) == (413, "body_too_large")
    # implausible: a result frame past the wall time since started_unix
    h.clock.t = NOW + 60 - 100
    late_frame = started_pair(n=5) + [mk(RES, 0, 9000, (2, 0), 0, n=5)]
    assert report(h, CA, ta, body(*late_frame)).json()["error"] == "bad_receipt"


def test_desync_voids_and_contradictions(h, tmp_path):
    ta, tb, _, _ = setup(h, tmp_path)
    h.clock.t = NOW + 600
    # progress state_hashes differ at one frame, no worker: void, no strike (vector 22)
    d = started_pair(n=1) + [mk(P, 0, 600, n=1), mk(P, 1, 600, salt=3, n=1)]
    assert report(h, CA, ta, body(*d)).json()["status"] == "void"
    # the same with a worker: disputed (the replay decides; with no verdict it closes void at 7 days, vector 49)
    dw = started_pair(n=2, build=WBUILD) + [mk(P, 0, 600, n=2, build=WBUILD), mk(P, 1, 600, salt=3, n=2, build=WBUILD)]
    j = report(h, CA, ta, body(*dw)).json()
    assert j["status"] == "disputed"
    h.clock.t = NOW + 60 + 7 * 86400
    h.app.state.results.settle()
    assert latest(tmp_path)[j["match_id"]]["status"] == "void"
    h.clock.t = NOW + 600
    # two reachable contradicting final claims at one frame: disputed, both marked
    base = started_pair(n=3) + [mk(P, 0, 600, n=3), mk(P, 1, 600, n=3), mk(P, 0, 1200, (1, 1), n=3),
                                mk(P, 1, 1200, (1, 1), n=3)]
    ra = base + [mk(RES, 0, 1500, (2, 1), 0, n=3)]
    rb = base + [mk(RES, 1, 1500, (1, 2), 1, n=3)]
    report(h, CA, ta, body(*ra))
    j = report(h, CB, tb, body(*rb)).json()
    assert j["status"] == "disputed"
    assert h.community.db.one("SELECT COUNT(*) FROM marks k JOIN matches m ON m.id = k.match "
                              "WHERE k.kind = 'disputed' AND m.match_id = ?", (j["match_id"],))[0] == 2
    # a stale claim: A claims at 900, B's later receipts continue to its own win at 1500 -> B counts, A struck
    sa = started_pair(n=4) + [mk(P, 0, 600, (1, 0), n=4), mk(P, 1, 600, (1, 0), n=4),
                              mk(RES, 0, 900, (2, 0), 0, n=4)]
    sb = started_pair(n=4) + [mk(P, 0, 600, (1, 0), n=4), mk(P, 1, 600, (1, 0), n=4), mk(P, 1, 1200, (1, 1), n=4),
                              mk(RES, 1, 1500, (1, 2), 1, n=4)]
    report(h, CA, ta, body(*sa))
    j = report(h, CB, tb, body(*sb)).json()
    assert j["status"] == "pending" and latest(tmp_path)[j["match_id"]]["winner"] == "p2"
    acct_a = h.community.db.one("SELECT id FROM accounts WHERE name = 'Alice'")[0]
    assert h.community.db.one("SELECT COUNT(*) FROM marks WHERE kind = 'strike' AND account = ?", (acct_a,))[0] == 1


def test_left_early_is_a_rated_loss_and_capped(h, tmp_path):
    ta, tb, a, b = setup(h, tmp_path)
    h.clock.t = NOW + 600

    def left(n):
        return started_pair(n=n) + [mk(P, 0, 600, (1, 0), n=n), mk(P, 1, 600, (1, 0), n=n),
                                    mk(RES, 0, 1500, (1, 0), LEFT, n=n)]
    j = report(h, CA, ta, body(*left(10))).json()
    assert j["status"] == "left_early"
    row = latest(tmp_path)[j["match_id"]]
    assert row["winner"] == "p1" and row["left_early"] == "p2" and row["status"] == "left_early"
    acct_b = h.community.db.one("SELECT id FROM accounts WHERE name = 'Bob'")[0]
    kinds = {r[0] for r in h.community.db.q("SELECT kind FROM marks WHERE account = ?", (acct_b,))}
    assert kinds == {"left_early", "left_early_behind"}
    # without the leaver's progress nothing is recorded (vector 22)
    no_prog = started_pair(n=11) + [mk(RES, 0, 1300, (0, 0), LEFT, n=11)]
    j = report(h, CA, ta, body(*no_prog)).json()
    assert j["status"] == "open" and j["match_id"] not in latest(tmp_path)
    # both claim the other left: dropped
    both = started_pair(n=12) + [mk(P, 0, 600, n=12), mk(P, 1, 600, n=12), mk(RES, 0, 1500, (0, 0), LEFT, n=12),
                                 mk(RES, 1, 1500, (0, 0), LEFT, n=12)]
    assert report(h, CA, ta, body(*both)).json()["status"] == "dropped"
    # at most 3 left-early results per reporter per day: the 4th is 429 cap_daily with Retry-After, not recorded
    for n in (13, 14):
        assert report(h, CA, ta, body(*left(n))).json()["status"] == "left_early"
    r = report(h, CA, ta, body(*left(15)))
    assert r.status_code == 429 and r.json()["error"] == "cap_daily" and int(r.headers["Retry-After"]) >= 60
    assert h.community.db.one("SELECT COUNT(*) FROM matches WHERE status = 'left_early'")[0] == 3


def test_pair_cap_flags_past_20_a_day(h, tmp_path):
    ta, _, _, _ = setup(h, tmp_path)
    h.clock.t = NOW + 600
    for n in range(21):
        assert report(h, CA, ta, body(*win_a(n=20 + n))).status_code == 200
    rows = latest(tmp_path).values()
    assert sum(1 for r in rows if "pair_cap" in r["flags"]) == 1 and len(rows) == 21
