import pytest

import glicko2
from glicko2 import Rating


def test_paper_worked_example():
    """Glickman 2013, section 'Example': 1500/200/0.06 plays 1400/30 (win),
    1550/100 (loss), 1700/300 (loss) in one period."""
    p = Rating(1500, 200, 0.06)
    out = glicko2.update(p, [(Rating(1400, 30), 1.0),
                             (Rating(1550, 100), 0.0),
                             (Rating(1700, 300), 0.0)])
    assert out.rating == pytest.approx(1464.06, abs=0.01)
    assert out.rd == pytest.approx(151.52, abs=0.01)
    assert out.vol == pytest.approx(0.05999, abs=0.00001)


def test_draw_between_equals_keeps_rating():
    a, b = glicko2.rate_match(Rating(), Rating(), 0.5)
    assert a.rating == pytest.approx(1500.0)
    assert b.rating == pytest.approx(1500.0)
    assert a.rd < 350 and b.rd < 350


def test_win_is_zero_sum_for_equals():
    a, b = glicko2.rate_match(Rating(), Rating(), 1.0)
    assert a.rating > 1500 > b.rating
    assert (a.rating - 1500) == pytest.approx(1500 - b.rating)


def test_idle_inflates_rd_only():
    r = glicko2.idle(Rating(1600, 50, 0.06))
    assert r.rating == 1600 and r.rd > 50
