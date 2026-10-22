"""Public HTML pages: public ids in URLs, name#TAG identities, no Discord
id anywhere in a rendered page, legacy /u/<discord id> redirect, private
profile for opted-out players."""

import privacy
from conftest import A, B, C, H, DISCORD_RE


def pid(dc):
    return privacy.public_id(dc)


def tag(dc):
    return privacy.tag_for(pid(dc))


def public_pages():
    return ["/", "/games", "/recent", "/recent?limit=200", "/sessions",
            "/g/ww", "/g/mbaacc", "/g/ww/c/ryu", "/g/ww/c/ken",
            f"/u/{pid(A)}", f"/u/{pid(B)}", f"/u/{pid(C)}", f"/u/{pid(H)}",
            "/u/0000000000000000"]


def test_no_discord_ids_in_any_page(client):
    for u in public_pages():
        r = client.get(u)
        assert r.status_code in (200, 404), u
        assert not DISCORD_RE.search(r.text), u
        for dc in (A, B, C, H):
            assert dc not in r.text, u


def test_links_use_public_ids_and_tags(client):
    body = client.get("/recent?limit=200").text
    assert f'href="/u/{pid(A)}"' in body
    assert f"alice#{tag(A)}" in body
    assert f"hidden_hank#{tag(H)}" in body       # names are never private
    body = client.get("/").text
    assert f'href="/u/{pid(B)}"' in body and f"bob#{tag(B)}" in body


def test_profile_renders_rating_and_rank(client):
    r = client.get(f"/u/{pid(A)}")
    assert r.status_code == 200
    assert f"alice#{tag(A)}" in r.text
    assert "Ratings" in r.text and "#1</td>" in r.text
    assert "PRIVATE" not in r.text


def test_private_profile(client):
    r = client.get(f"/u/{pid(H)}")
    assert r.status_code == 200
    assert "PRIVATE" in r.text and f"hidden_hank#{tag(H)}" in r.text
    assert "Ratings" not in r.text and "Recent Matches" not in r.text


def test_legacy_discord_url_redirects(client):
    r = client.get(f"/u/{A}", follow_redirects=False)
    assert r.status_code == 301
    assert r.headers["location"].endswith(f"/u/{pid(A)}")
    # Opted-out players redirect too, then land on the private page.
    r = client.get(f"/u/{H}")
    assert r.status_code == 200 and "PRIVATE" in r.text
    assert not DISCORD_RE.search(r.text)


def test_unknown_user_404(client):
    assert client.get("/u/0000000000000000").status_code == 404
    assert client.get("/u/999999999999999999",
                      follow_redirects=False).status_code == 404


def test_recent_shows_community_results_with_replay_links(client, tmp_path, monkeypatch):
    import json as _json
    import privacy
    import ratings
    pa, pb = privacy.public_id("200000000000000001"), privacy.public_id("200000000000000002")
    rows = [{"v": 1, "match_id": "aaaaaaaaaaaa", "game_id": "fm2k:wonderfulworld", "family": "fm2k", "status": "pending",
             "started_at": 1792530000, "finished_at": 1792530400, "rounds_to_win": 2,
             "p1": {"public_id": pa, "name": "alice", "tag": "0412", "hidden": False, "char": None},
             "p2": {"public_id": pb, "name": "bob", "tag": "7781", "hidden": False, "char": None},
             "winner": "p1", "score": [2, 1], "flags": [],
             "replay_url": "https://replays.example.test/replays/v1/fm2k/x.pcrz"},
            {"v": 1, "match_id": "bbbbbbbbbbbb", "game_id": "fm2k:wonderfulworld", "family": "fm2k", "status": "final",
             "started_at": 1792530000, "finished_at": 1792530500, "rounds_to_win": 2,
             "p1": {"public_id": pa, "name": "alice", "tag": "0412", "hidden": False, "char": None},
             "p2": {"public_id": pb, "name": "bob", "tag": "7781", "hidden": False, "char": None},
             "winner": "p2", "score": [1, 2], "flags": [], "replay_url": "javascript:alert(1)"}]
    f = tmp_path / "results.jsonl"
    f.write_text("\n".join(_json.dumps(r) for r in rows) + "\n", encoding="utf-8")
    monkeypatch.setenv("FM2K_RESULTS_PATH", str(f))
    ratings.clear_cache()
    html = client.get("/recent").text
    assert "alice" in html and "bob" in html and f"/u/{pa}" in html
    assert 'href="https://replays.example.test/replays/v1/fm2k/x.pcrz"' in html
    assert "javascript:" not in html  # only https replay links render
    assert "pending" in html
