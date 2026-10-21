"""game_aliases.json: legacy hub exe-stem ids rated under the launcher's catalog ids."""
import json

import ratings


def use_aliases(tmp_path, monkeypatch, aliases, names=None):
    p = tmp_path / "aliases.json"
    p.write_text(json.dumps({"aliases": aliases, "names": names or {}}), encoding="utf-8")
    monkeypatch.setattr(ratings, "ALIASES_PATH", p)
    ratings.clear_cache()


def test_catalog_id_finds_legacy_history(client, tmp_path, monkeypatch):
    before = client.get("/api/v1/g/ww/leaderboard").json()["players"]
    use_aliases(tmp_path, monkeypatch, {"ww": "fm2k:wonderfulworld"}, {"fm2k:wonderfulworld": "WonderfulWorld"})
    body = client.get("/api/v1/g/fm2k:wonderfulworld/leaderboard").json()
    assert body["game_id"] == "fm2k:wonderfulworld" and body["name"] == "WonderfulWorld"
    assert body["family"] == "fm2k" and body["players"] == before  # same ratings, new key
    assert client.get("/api/v1/g/ww/leaderboard").json()["game_id"] == "fm2k:wonderfulworld"  # old id still works
    games = {g["game_id"] for g in client.get("/api/v1/games").json()["games"]}
    assert "fm2k:wonderfulworld" in games and "ww" not in games
    recent = client.get("/api/v1/recent", params={"game": "ww"}).json()
    assert recent and all(m["game_id"] == "fm2k:wonderfulworld" for m in recent["matches"])
    legacy = client.get("/api/v1/matches", params={"game_id": "fm2k:wonderfulworld"}).json()["matches"]
    assert legacy and all(m["game_id"] == "ww" for m in legacy)  # the pre-v1 endpoint keeps logged ids


def test_builds_share_one_rating(client, tmp_path, monkeypatch):
    use_aliases(tmp_path, monkeypatch, {"ww": "fm2k:one", "fm95:cpw1": "fm2k:one"})
    games = {g["game_id"]: g for g in client.get("/api/v1/games").json()["games"]}
    assert "fm2k:one" in games and "ww" not in games and "fm95:cpw1" not in games
    total = sum(g["matches"] for g in games.values())
    assert games["fm2k:one"]["matches"] == total - games["mbaacc"]["matches"]  # ww + cpw1 merged


def test_html_catalog_id_redirects_to_the_legacy_page(client, tmp_path, monkeypatch):
    use_aliases(tmp_path, monkeypatch, {"ww": "fm2k:wonderfulworld"})
    r = client.get("/g/fm2k:wonderfulworld", follow_redirects=False)
    assert r.status_code == 301 and r.headers["location"].endswith("/g/ww")
