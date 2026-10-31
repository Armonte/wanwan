"""/v1/site/matches (two accounts + a <=24 h window) and /v1/site/replaymeta (the REPLAYMETA-V1 assertion)."""
import base64
import hashlib

import tickets as T
from conftest import DISCORD_ID, NOW
from report_fixture import CA, body, mk, report, setup, started_pair
from test_site import SEED, SITE, call
import receipts as R

P, RES = R.PROGRESS, R.RESULT
RSHA = hashlib.sha256(b"replay bytes before the PCRM block").hexdigest()


def win_a(**kw):
    return started_pair(**kw) + [mk(P, 0, 600, **kw), mk(P, 1, 600, **kw), mk(P, 0, 1200, (1, 1), **kw),
                                 mk(P, 1, 1200, (1, 1), **kw), mk(RES, 0, 1500, (2, 1), 0, **kw)]


def played(h, tmp_path):
    ta, tb, a, b = setup(h, tmp_path)
    h.community.add_site(SITE, T.seed_public(SEED))
    h.clock.t = NOW + 600
    j = report(h, CA, ta, body(*win_a())).json()
    assert j["status"] == "pending"
    ids = [base64.b64encode(bytes.fromhex(x["account_id"])).decode() for x in (a, b)]
    return j["match_id"], ids, a, b


def q(h, ids, frm=NOW, to=NOW + 3600, **extra):
    return call(h, "/v1/site/matches", dict({"accounts": ids, "from": frm, "to": to}, **extra), ts=h.clock.t)


def test_find_both_orders_and_window(h, tmp_path):
    mid, ids, a, b = played(h, tmp_path)
    r = q(h, ids)
    assert r.status_code == 200, r.text
    m = r.json()
    assert m["hidden"] == 0 and len(m["matches"]) == 1
    x = m["matches"][0]
    assert x["match_id"] == mid and x["slots"] == ids and x["score"] == [2, 1] and x["winner"] == "p1"
    assert x["status"] == "pending" and x["game_no"] == 3 and x["started"] == NOW + 60
    assert q(h, ids[::-1]).json()["matches"][0]["match_id"] == mid           # slot order does not matter
    assert q(h, ids, frm=NOW + 61).json()["matches"] == []                    # outside the window
    assert q(h, ids, game_id="fm2k:wonderfulworld").json()["matches"][0]["match_id"] == mid
    assert q(h, ids, game_id="wonderfulworld").json()["matches"][0]["match_id"] == mid
    assert q(h, ids, game_id="mbaacc").json()["matches"] == []
    for resp in h.responses:
        assert DISCORD_ID.encode() not in resp.content


def test_refusals_and_unknown_is_empty(h, tmp_path):
    mid, ids, a, b = played(h, tmp_path)
    assert q(h, ids, to=NOW + 86401).status_code == 400                        # window > 24 h
    assert q(h, ids, frm=NOW + 10, to=NOW).status_code == 400
    assert q(h, [ids[0], ids[0]]).status_code == 400
    assert q(h, ids[:1]).status_code == 400
    assert q(h, [ids[0], "AAAA"]).status_code == 400
    ghost = base64.b64encode(bytes(16)).decode()
    assert q(h, [ids[0], ghost]).json() == {"matches": [], "hidden": 0}       # same answer as "never met"
    assert call(h, "/v1/site/matches", {"accounts": ids, "from": NOW, "to": NOW + 10},
                seed=bytes([7]) * 32, ts=h.clock.t).status_code == 401       # not the site's key


def test_hidden_is_count_only_and_no_meta(h, tmp_path):
    mid, ids, a, b = played(h, tmp_path)
    row = h.community.find_account(a["account_id"])
    h.community.set_stats_hidden(row["id"], True)
    assert q(h, ids).json() == {"matches": [], "hidden": 1}
    r = call(h, "/v1/site/replaymeta", {"match_id": mid, "replay_sha256": RSHA}, ts=h.clock.t)
    assert r.status_code == 403 and r.json()["error"] == "stats_hidden"


def test_replaymeta_assertion_verifies(h, tmp_path):
    mid, ids, a, b = played(h, tmp_path)
    r = call(h, "/v1/site/replaymeta", {"match_id": mid, "replay_sha256": RSHA}, ts=h.clock.t)
    assert r.status_code == 200, r.text
    j = r.json()
    assert j["slots"] == ids and j["score"] == [2, 1] and j["winner"] == 0 and j["nslots"] == 2 and j["game_no"] == 3
    key = h.community.signing_key()
    msg = T.replay_meta_message(mid, bytes.fromhex(RSHA), 3, [base64.b64decode(x) for x in ids], [2, 1], 0, j["issued"])
    assert T.verify(T.seed_public(key.seed), b"", msg, base64.b64decode(j["assertion"]))
    assert j["key_gen"] == key.key_gen
    for bad in ({"match_id": mid, "replay_sha256": RSHA.upper()}, {"match_id": mid[:11], "replay_sha256": RSHA},
                {"match_id": mid}):
        assert call(h, "/v1/site/replaymeta", bad, ts=h.clock.t).status_code == 400
    r = call(h, "/v1/site/replaymeta", {"match_id": "0" * 12, "replay_sha256": RSHA}, ts=h.clock.t)
    assert r.status_code == 404


def test_tag_prefix_free():
    assert T.TAG_REPLAYMETA in T.ALL_TAGS
    for x in T.ALL_TAGS:
        for y in T.ALL_TAGS:
            assert x is y or not y.startswith(x)


def test_replaymeta_pinned_vector():
    seed = bytes([3]) * 32  # TEST seed standing in for an online ticket key
    msg = T.replay_meta_message("0123456789ab", bytes(range(32)), 3, [bytes([0xA0]) * 16, bytes([0xB0]) * 16],
                                [2, 1], 0, 1800000000)
    assert msg.hex() == VEC_MSG
    assert base64.b64encode(T.sign(seed, b"", msg)).decode() == VEC_SIG
    assert T.seed_public(seed).hex() == VEC_PUB


VEC_MSG = ("50432d434f4d4d554e4954592d5245504c41594d4554412d56310a303132333435363738396162000102030405060708090a0b0c0d0e0f"
           "101112131415161718191a1b1c1d1e1f0300000002a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0020001000000d2496b00000000")
VEC_SIG = "shNM00vI2z0zPDoNT+cqOWEUx5cai2+AI5fKHBVw5ejKPOxqF7RjYNJ8sSr2ogwNVt2sGssocATP08/pA+d2Dg=="
VEC_PUB = "ed4928c628d1c2c6eae90338905995612959273a5c63f93636c14614ac8737d1"
