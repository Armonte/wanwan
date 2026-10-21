"""The community results export (results.jsonl, FM2K_RESULTS_PATH) merged with the legacy hub log: latest line per
match_id, rated statuses, the public-id join with legacy players, public-id-only players, numeric tags, hidden rows,
and the cache keyed on both files."""
import json

import privacy
import ratings
from conftest import A, DISCORD_RE, FIXTURE

Z = "00112233aabbccdd"  # a community player with no legacy history


def row(mid, p1, p2, winner="p1", status="pending", ts=2_000_000_000, **kw):
    def who(pid, hidden):
        name, tag = ("zed", "7781") if pid == Z else ("alice2", "0412")
        return {"public_id": pid, "name": name, "tag": tag, "hidden": hidden, "char": None}
    r = {"v": 1, "match_id": mid, "game_id": "fm2k:testgame", "family": "fm2k", "build": "943e",
         "status": status, "started_at": ts - 30, "finished_at": ts, "rounds_to_win": 2,
         "p1": who(p1, kw.pop("h1", False)), "p2": who(p2, kw.pop("h2", False)),
         "winner": winner, "score": [2, 1], "left_early": None, "flags": [], "replay_url": None}
    r.update(kw)
    return r


def write(path, rows):
    path.write_text("".join(json.dumps(r) + "\n" for r in rows), encoding="utf-8")


def test_merge_join_and_statuses(tmp_path):
    res = tmp_path / "results.jsonl"
    pa = privacy.public_id(A)
    write(res, [
        row("m1", pa, Z),
        row("m2", pa, Z, status="pending", ts=2_000_000_100),
        row("m2", pa, Z, status="void", ts=2_000_000_100, winner=None),  # latest line wins: not rated
        row("m3", Z, pa, status="left_early", winner="p1", left_early="p2", ts=2_000_000_200),
        row("m4", pa, Z, flags=["pair_cap"], ts=2_000_000_300),
        row("m5", pa, pa),
        row("m6", "not-a-pid", Z),
    ])
    s = ratings.snapshot(FIXTURE, res)
    assert s.excluded["status_void"] == 1 and s.excluded["pair_cap"] == 1 and s.excluded["self"] >= 1
    rec = s.games["fm2k:testgame"]
    assert set(rec) == {A, privacy.PID_KEY + Z}  # A joined by public id; Z keyed by its public id
    assert (rec[A].wins, rec[A].losses) == (1, 1) and (rec["pid:" + Z].wins, rec["pid:" + Z].losses) == (1, 1)
    assert s.by_pid[Z] == "pid:" + Z and s.by_pid[pa] == A
    assert s.tags[A] == "0412" and s.nicks[A] == "alice2"  # the community name and #tag after the latest row
    left = next(m for m in s.matches if m.id == "m3")
    assert left.status == "left_early" and left.disconnect and left.winner == "p1" and left.p1 == "pid:" + Z


def test_api_serves_results_players(tmp_path, monkeypatch, client):
    res = tmp_path / "results.jsonl"
    write(res, [row(f"m{i}", privacy.public_id(A), Z, winner="p2") for i in range(12)])
    monkeypatch.setenv("FM2K_RESULTS_PATH", str(res))
    monkeypatch.delenv("FM2K_COMMUNITY_HIDDEN_PATH", raising=False)
    lb = client.get("/api/v1/g/fm2k:testgame/leaderboard").json()["players"]
    assert [p["public_id"] for p in lb] == [Z, privacy.public_id(A)]
    assert lb[0]["tag"] == "7781" and lb[0]["name"] == "zed" and lb[0]["wins"] == 12
    prof = client.get(f"/api/v1/u/{Z}").json()
    assert prof["private"] is False and prof["totals"]["wins"] == 12
    assert prof["recent"][0]["status"] == "pending" and prof["recent"][0]["p2"]["public_id"] == Z
    for u in ("/api/v1/recent", "/api/v1/games", f"/api/v1/u/{Z}", "/api/v1/g/fm2k:testgame/leaderboard"):
        assert not DISCORD_RE.search(client.get(u).text), u
    # the cache is keyed on the results file too: a hidden row (latest) hides Z from the board
    with res.open("a", encoding="utf-8") as f:
        f.write(json.dumps(row("m99", privacy.public_id(A), Z, winner="p2", h2=True)) + "\n")
    lb = client.get("/api/v1/g/fm2k:testgame/leaderboard").json()["players"]
    assert Z not in [p["public_id"] for p in lb]
    assert client.get(f"/api/v1/u/{Z}").json()["private"] is True
    # with the service's hidden.json configured, that file decides (Z is not in it)
    hid = tmp_path / "hidden.json"
    hid.write_text(json.dumps({"v": 1, "hidden": []}))
    monkeypatch.setenv("FM2K_COMMUNITY_HIDDEN_PATH", str(hid))
    assert client.get(f"/api/v1/u/{Z}").json()["private"] is False


def test_torn_line_is_skipped(tmp_path):
    res = tmp_path / "results.jsonl"
    write(res, [row("m1", privacy.public_id(A), Z)])
    with res.open("a") as f:
        f.write('{"v": 1, "match_id": "m2", "sta')
    assert [r["match_id"] for r in ratings.read_results(res)] == ["m1"]


def test_replay_url_passes_through(tmp_path, monkeypatch, client):
    """A later state line of the same match adds replay_url (the community service appends one after an upload)."""
    res = tmp_path / "results.jsonl"
    url = "https://replays.example.test/replays/v1/fm2k/testgame/2033-05/0123456789abcdef.pcrz"
    write(res, [row("m1", privacy.public_id(A), Z), row("m1", privacy.public_id(A), Z, replay_url=url)])
    monkeypatch.setenv("FM2K_RESULTS_PATH", str(res))
    monkeypatch.delenv("FM2K_COMMUNITY_HIDDEN_PATH", raising=False)
    m = next(x for x in ratings.snapshot(FIXTURE, res).matches if x.id == "m1")
    assert m.replay_url == url
    prof = client.get(f"/api/v1/u/{Z}").json()
    assert next(r for r in prof["recent"] if r["id"] == "m1")["replay_url"] == url
