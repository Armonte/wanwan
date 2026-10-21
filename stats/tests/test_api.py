import json

import privacy
from conftest import A, B, C, H, DISCORD_RE


def pid(dc):
    return privacy.public_id(dc)


def test_no_discord_ids_in_any_response(client):
    urls = ["/api/v1/games", "/api/v1/g/ww/leaderboard", "/api/v1/recent",
            "/api/v1/matches", f"/api/v1/u/{pid(A)}", f"/api/v1/u/{pid(H)}"]
    for u in urls:
        r = client.get(u)
        assert r.status_code == 200, u
        assert not DISCORD_RE.search(r.text), u


def test_games(client):
    g = {x["game_id"]: x for x in client.get("/api/v1/games").json()["games"]}
    assert g["ww"]["family"] == "fm2k"
    assert g["fm95:cpw1"]["family"] == "fm95"
    assert g["mbaacc"]["family"] == "mbaacc"
    assert g["ww"]["matches"] == 40


def test_leaderboard_ordering_and_hidden(client):
    body = client.get("/api/v1/g/ww/leaderboard").json()
    rows = body["players"]
    names = [r["name"] for r in rows]
    assert names == ["alice", "bob", "carol"]     # H opted out
    assert [r["rank"] for r in rows] == [1, 2, 3]
    ratings_ = [r["rating"] for r in rows]
    assert ratings_ == sorted(ratings_, reverse=True)
    a = rows[0]
    assert a["public_id"] == pid(A) and a["tag"] == pid(A)[:4].upper()
    assert (a["wins"], a["losses"], a["draws"]) == (16, 12, 1)


def test_leaderboard_eligibility(client):
    # fm95:cpw1 has a single match each: RD ~ 290, not listed.
    body = client.get("/api/v1/g/fm95:cpw1/leaderboard").json()
    assert body["players"] == []


def test_leaderboard_unknown_game(client):
    assert client.get("/api/v1/g/nope/leaderboard").status_code == 404


def test_profile(client):
    p = client.get(f"/api/v1/u/{pid(A)}").json()
    assert p["private"] is False and p["name"] == "alice"
    ww = next(g for g in p["games"] if g["game_id"] == "ww")
    assert ww["rank"] == 1
    assert ww["top_characters"][0] == {"char": "ryu", "matches": 16}
    assert len(p["recent"]) == 20
    assert p["recent"][0]["game_id"] == "fm95:cpw1"   # newest first


def test_profile_private_stub(client):
    p = client.get(f"/api/v1/u/{pid(H)}").json()
    assert p == {"public_id": pid(H), "name": "hidden_hank",
                 "tag": pid(H)[:4].upper(), "private": True}


def test_profile_unknown(client):
    assert client.get("/api/v1/u/0000000000000000").status_code == 404
    # A raw Discord id is not a valid key.
    assert client.get(f"/api/v1/u/{A}").status_code == 404


def test_recent_hidden_player_keeps_name(client):
    rows = client.get("/api/v1/recent?game=ww&limit=200").json()["matches"]
    hm = [m for m in rows if m["p1"]["public_id"] == pid(H)]
    assert len(hm) == 12
    for m in hm:
        assert m["p1"]["private"] is True
        assert m["p1"]["name"] == "hidden_hank"
        assert m["p1"]["tag"] == pid(H)[:4].upper()
        assert "rating" not in m["p1"]


def test_opponent_profile_lists_hidden_player_by_name(client):
    p = client.get(f"/api/v1/u/{pid(A)}").json()
    # A's 20 most recent include the 12 losses to H (only the fm95
    # match is newer).
    opp = [m["p1"] for m in p["recent"] if m["p1"]["public_id"] == pid(H)]
    assert opp and all(o["name"] == "hidden_hank" and o["private"]
                       for o in opp)


def test_recent_shape_and_replay(client):
    rows = client.get("/api/v1/recent?limit=2").json()["matches"]
    assert rows[1]["game_id"] == "mbaacc" or rows[0]["game_id"] == "mbaacc"
    mb = next(m for m in rows if m["game_id"] == "mbaacc")
    assert mb["replay_url"].endswith("1.fm2krep")
    assert mb["winner"] == "p2"
    cpw = next(m for m in rows if m["game_id"] == "fm95:cpw1")
    assert cpw["score"] == [2, 1]


def test_optout_toggle(client, env):
    privacy.set_hidden(H, False)
    names = [r["name"] for r in
             client.get("/api/v1/g/ww/leaderboard").json()["players"]]
    assert names[0] == "hidden_hank"
    privacy.set_hidden(A, True)
    names = [r["name"] for r in
             client.get("/api/v1/g/ww/leaderboard").json()["players"]]
    assert "alice" not in names
    assert json.loads(env.read_text())["hidden"] == [A]


def test_legacy_matches_feed(client):
    rows = client.get("/api/v1/matches?limit=500").json()["matches"]
    assert all(H not in (r["p1_id"], r["p2_id"]) for r in rows)
    assert any(r["p1_id"] == pid(H) and r["p1_nick"] == "hidden_hank"
               for r in rows)
    mine = client.get(f"/api/v1/matches?user_id={pid(B)}").json()["matches"]
    assert mine and all(pid(B) in (r["p1_id"], r["p2_id"]) for r in mine)


def test_launcher_opt_out_by_public_id(client, tmp_path, monkeypatch):
    # the community service exports public ids of players who hid their profile from the launcher
    f = tmp_path / "hidden.json"
    f.write_text(json.dumps({"v": 1, "hidden": [pid(A)]}), encoding="utf-8")
    monkeypatch.setenv("FM2K_COMMUNITY_HIDDEN_PATH", str(f))
    body = client.get(f"/api/v1/u/{pid(A)}").json()
    assert body["private"] is True and set(body) == {"public_id", "name", "tag", "private"}
