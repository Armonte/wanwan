"""The per-game table import (games.py): the generator's TSV, refused rows, and the body cap it implies."""
import pytest

import games as G
from report_fixture import TSV


def test_parse_and_import(h):
    rows = G.parse_tsv(TSV)
    assert [(r["family"], r["worker"]) for r in rows] == [("fm2k", 0), ("fm2k", 1)]
    assert G.import_rows(h.community.db, rows, 1) == 2
    assert G.lookup(h.community.db, "wonderfulworld", "943e826d7b675e52")["fps"] == 100
    assert G.lookup(h.community.db, "wonderfulworld", "nope") is None
    assert G.max_report_body(h.community.db) == 928286
    G.import_rows(h.community.db, rows[:1], 2, replace_all=True)
    assert h.community.db.one("SELECT COUNT(*) FROM games")[0] == 1


@pytest.mark.parametrize("edit", [
    lambda t: t.replace("943e826d7b675e52", "*"),           # the vendored placeholder build id
    lambda t: t.replace("\tFM2K\t943e", "\tXYZ\t943e"),      # no family
    lambda t: t.replace("1,2,3\t100", "0,2\t100", 1),       # rounds_to_win 0
    lambda t: t.replace("\t2\t180000\tno", "\t5\t180000\tno"),  # input width 5
    lambda t: t.replace("rounds_to_win_legal", "rtw"),     # a missing column
])
def test_refused_rows(edit):
    with pytest.raises(ValueError):
        G.parse_tsv(edit(TSV))
