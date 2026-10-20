"""KEYSET verification (good, one bad signature, revoked root, skew, staleness), the service's choice among the files
in its KEYSET folder, and the owner's offline path: admin keyset-body -> sign_keyset.py -> install."""
import os

import admin
import sign_keyset
import tickets as T
from conftest import NOW, ROOT_A, ROOT_B, ROOTS, make_keyset

PUB = T.seed_public(bytes([0x33]) * 32)


def test_good_keyset_verifies_under_either_root():
    for root in (0, 1):
        r, ks = T.parse_keyset(make_keyset(PUB, root=root), ROOTS, 0, NOW)
        assert r == T.OK and ks.root_id == root and ks.keys[0].pub == PUB


def test_one_bad_signature_byte_and_wrong_root():
    b = bytearray(make_keyset(PUB))
    b[-1] ^= 1
    assert T.parse_keyset(bytes(b), ROOTS, 0, NOW)[0] == "bad_signature"
    body = bytearray(make_keyset(PUB))
    body[30] ^= 1  # a body byte (issued) under the old signature
    assert T.parse_keyset(bytes(body), ROOTS, 0, NOW)[0] == "bad_signature"
    k = T.Keyset(1, NOW, NOW + 86400, 0, 1, [T.KeysetEntry(1, PUB, 0, NOW + 86400)])
    assert T.parse_keyset(T.sign_keyset(k, ROOT_A), ROOTS, 0, NOW)[0] == "bad_signature"  # names b, signed by a


def test_revoked_root():
    revoking = make_keyset(PUB, serial=2, root=1, revoked_root=0)
    r, ks = T.parse_keyset(revoking, ROOTS, 0, NOW)
    assert r == T.OK and ks.revoked_root == 0
    assert T.parse_keyset(make_keyset(PUB, serial=3, root=0), ROOTS, 0, NOW, revoked_mask=1)[0] == \
        "keyset_revoked_root"
    assert T.parse_keyset(make_keyset(PUB, root=0, revoked_root=0), ROOTS, 0, NOW)[0] == "noncanonical"


def test_skew_staleness_rollback():
    plus1h = make_keyset(PUB, issued=NOW + 3600)
    assert T.parse_keyset(plus1h, ROOTS, 0, NOW, T.CLIENT_KEYSET_SKEW)[0] == T.OK
    assert T.parse_keyset(plus1h, ROOTS, 0, NOW, T.SERVICE_KEYSET_SKEW)[0] == "not_yet_valid"
    plus25h = make_keyset(PUB, issued=NOW + 25 * 3600)
    assert T.parse_keyset(plus25h, ROOTS, 0, NOW, T.CLIENT_KEYSET_SKEW)[0] == "not_yet_valid"
    b = make_keyset(PUB, serial=5)
    assert T.parse_keyset(b, ROOTS, 6, NOW)[0] == "keyset_rollback"
    assert T.parse_keyset(b, ROOTS, 0, NOW + 46 * 86400)[0] == "keyset_stale"
    for n in range(len(b)):
        assert T.parse_keyset(b[:n], ROOTS, 0, NOW)[0] != T.OK
    assert T.parse_keyset(b + b"\0", ROOTS, 0, NOW)[0] != T.OK


def test_service_picks_newest_valid_and_skips_bad(h):
    pub = T.seed_public(h.key_seed)
    bad = bytearray(make_keyset(pub, serial=9))
    bad[-5] ^= 1
    h.install_keyset(bytes(bad), "keyset_000009.pckeys")  # bad signature: ignored
    h.install_keyset(make_keyset(pub, serial=4), "keyset_000004.pckeys")
    h.install_keyset(make_keyset(pub, serial=7, issued=NOW + 3600), "keyset_000007.pckeys")  # pre-signed, not due
    assert h.keyset().serial == 4
    h.clock.t = NOW + 3600
    h.community._ks_cache = (None, None, None)
    assert h.keyset().serial == 7
    assert int(h.community.db.meta_get("keyset_floor")) == 7


def test_service_root_revocation_is_sticky(h):
    pub = T.seed_public(h.key_seed)
    h.install_keyset(make_keyset(pub, serial=2, root=1, revoked_root=0), "keyset_000002.pckeys")
    assert h.keyset().serial == 2
    assert int(h.community.db.meta_get("revoked_root_mask")) == 1
    os.remove(os.path.join(h.cfg.keyset_dir, "keyset_000002.pckeys"))
    h.install_keyset(make_keyset(pub, serial=3, root=0), "keyset_000003.pckeys")  # from the revoked root
    h.community._ks_cache = (None, None, None)
    r = h.get("/v1/keyset")
    assert r.status_code == 503 and r.json()["error"] == "keyset_unusable"
    assert "keyset_revoked_root" in r.json()["detail"]


def test_ticket_key_not_in_keyset_refuses_issuing(h):
    for f in os.listdir(h.cfg.keyset_dir):
        os.remove(os.path.join(h.cfg.keyset_dir, f))
    h.install_keyset(make_keyset(T.seed_public(bytes([0x44]) * 32), serial=2), "keyset_000002.pckeys")
    r = h.get("/v1/deny")
    assert r.status_code == 503 and r.json()["error"] == "ticket_key_not_in_keyset"
    assert h.get("/health").json()["issuing"] == {"error": "ticket_key_not_in_keyset"}


def test_owner_offline_signing_round_trip(tmp_path, capsys):
    body_path, out_path = str(tmp_path / "k.body"), str(tmp_path / "k.pckeys")
    admin.main(["keyset-body", "--serial", "3", "--issued", str(NOW), "--root", "b", "--min-key-gen", "1",
                "--ticket-key", f"1:{PUB.hex()}", "--worker-key",
                f"2:{T.seed_public(bytes([5]) * 32).hex()}:{NOW}:{NOW + 86400}:wonderfulworld,fm2k:other",
                "--out", body_path])
    key_b = tmp_path / "root_b.hex"
    key_b.write_text(ROOT_B.hex())
    roots_arg = ",".join(r.hex() for r in ROOTS)
    assert sign_keyset.main([body_path, "-", str(key_b), out_path, "--roots", roots_arg]) == 0
    b = open(out_path, "rb").read()
    r, ks = T.parse_keyset(b, ROOTS, 0, NOW)
    assert r == T.OK and ks.root_id == 1 and ks.serial == 3 and ks.min_key_gen == 1
    assert [e.key_gen for e in ks.keys] == [1, 2] and ks.keys[1].scope == ["fm2k:other", "wonderfulworld"]
    # PEM key files work too; a wrong root file is refused before anything is written
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    pem = Ed25519PrivateKey.from_private_bytes(ROOT_A).private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
    (tmp_path / "root_a.pem").write_bytes(pem)
    try:
        sign_keyset.main([body_path, "-", str(tmp_path / "root_a.pem"), str(tmp_path / "x.pckeys"), "--roots",
                          roots_arg])
        assert False, "root a's key must not sign as root b"
    except SystemExit as e:
        assert "is not root b" in str(e)
    assert not (tmp_path / "x.pckeys").exists()
