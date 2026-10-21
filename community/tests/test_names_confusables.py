"""The full UTS #39 table (shipped in unicode/confusables.txt) on top of case folding, and reskeleton after a
table change."""
import names


def test_full_table_is_shipped_and_used():
    assert len(names._confusables_file()) > 6000
    sk = names.skeleton
    assert sk("Skeleton") == sk("SKELETON") == sk("Ske leton")  # case and spaces still fold
    assert sk("Paypal") == sk("Рaypal")  # Cyrillic Р
    assert sk("teo") == sk("ｔｅｏ")  # fullwidth
    assert sk("Armonte") == sk("Armonté")  # accents
    assert sk("Mario") == sk("Mar1o")
    assert sk("Mario") != sk("Maria")


def test_reskeleton_keeps_oldest_tag_and_reflags(h):
    a = h.link(bytes([0x81]) * 32, name="Skeleton").json()
    h.app.state.discord.users["third-code"] = "333"
    b = h.link(bytes([0x82]) * 32, name="Other", code="third-code").json()
    db = h.community.db
    # pretend the later account was named under an older table: same tag, a skeleton that no longer matches
    db.x("UPDATE accounts SET name = 'SKELETОN', skeleton = 'stale', tag = ? WHERE id = 2", (int(a["tag"]),))
    changed = h.community.reskeleton()
    rows = db.q("SELECT id, tag, skeleton, flagged FROM accounts ORDER BY id")
    assert rows[0]["tag"] == int(a["tag"]) and rows[0]["skeleton"] == rows[1]["skeleton"]
    assert rows[1]["tag"] != rows[0]["tag"] and changed == [(2, int(a["tag"]), rows[1]["tag"])]
    assert rows[0]["flagged"] == 1 and rows[1]["flagged"] == 1
    assert b["display"]  # untouched otherwise
