import ratings
from conftest import A, B, C, H


def test_excluded_counts(snap):
    ex = snap.excluded
    assert ex["unlinked"] == 2       # guest id + pre-auth nick id
    assert ex["self"] == 1
    assert ex["no_contest"] == 1     # both-disconnect is not a draw
    assert ex["repaired"] == 1


def test_no_unlinked_player_anywhere(snap):
    for m in snap.matches:
        assert ratings.is_linked(m.p1) and ratings.is_linked(m.p2)


def test_timeout_draw_repaired_from_rounds(snap):
    m = next(m for m in snap.matches if m.repaired)
    assert m.winner == "p1" and m.p1 == A and m.score == (2, 0)


def test_equal_rounds_and_schema1_draws_stay_draws(snap):
    draws = [m for m in snap.matches if m.winner == "draw"]
    assert len(draws) == 2
    assert {m.score for m in draws} == {(1, 1), None}


def test_draw_counts_half(snap):
    ww = snap.games["ww"]
    assert ww[B].draws == 1 and ww[C].draws == 2 and ww[A].draws == 1


def test_disconnect_follows_winner_id(snap):
    m = next(m for m in snap.matches if m.disconnect)
    assert m.winner == "p1" and m.p1 == A


def test_records(snap):
    a = snap.games["ww"][A]
    # 10 vs B + 4 vs C + repaired draw + disconnect win; 12 losses to H
    assert (a.wins, a.losses, a.draws) == (16, 12, 1)


def test_game_family():
    f = ratings.game_family
    assert f("fm2k:ww") == "fm2k"
    assert f("fm95:cpw1") == "fm95"
    assert f("mbaacc") == "mbaacc"
    assert f("wonderful_world_ver_0946") == "fm2k"
    assert f("some_cpw_game", "FM95") == "fm95"
    assert f("") == "unknown"


def test_cache_reuses_snapshot(snap):
    from conftest import FIXTURE
    assert ratings.snapshot(FIXTURE) is snap


def test_cache_invalidates_on_change(tmp_path):
    import json
    p = tmp_path / "m.json"
    rec = {"id": "x1", "p1_id": A, "p2_id": B, "winner_id": A,
           "game_id": "g", "started_at": 1.0, "finished_at": 2.0}
    p.write_text(json.dumps({"matches": [rec]}))
    s1 = ratings.snapshot(p)
    rec2 = dict(rec, id="x2", finished_at=3.0)
    p.write_text(json.dumps({"matches": [rec, rec2]}))
    s2 = ratings.snapshot(p)
    assert len(s1.matches) == 1 and len(s2.matches) == 2
