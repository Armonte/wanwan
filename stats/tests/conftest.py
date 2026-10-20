import json
import sys
from pathlib import Path

import pytest

STATS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(STATS))
FIXTURE = Path(__file__).resolve().parent / "fixtures" / "matches.json"

A = "100000000000000001"
B = "100000000000000002"
C = "100000000000000003"
H = "100000000000000004"


@pytest.fixture(autouse=True)
def env(tmp_path, monkeypatch):
    """Fixed id secret, an opt-out file hiding H, fresh caches per test."""
    import privacy
    import ratings
    privacy.set_secret(b"test-secret")
    optout = tmp_path / "optout.json"
    optout.write_text(json.dumps({"hidden": [H]}), encoding="utf-8")
    monkeypatch.setenv("FM2K_STATS_OPTOUT_PATH", str(optout))
    ratings.clear_cache()
    yield optout
    ratings.clear_cache()


@pytest.fixture
def snap():
    import ratings
    return ratings.snapshot(FIXTURE)


@pytest.fixture
def client(monkeypatch):
    from fastapi.testclient import TestClient
    import app as app_mod
    monkeypatch.setattr(app_mod, "MATCHES_PATH", FIXTURE)
    return TestClient(app_mod.app)
