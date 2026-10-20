"""The Python encoders against PovertyCaster's pinned C++ vectors (pc-core tests/community_ticket/ticket_test.cpp):
same fixed seeds, same fields, same SHA-256 of the bytes. Plus the C++ test's ticket and name negatives."""
import pytest

import names
import tickets as T

# from ticket_test.cpp
PCCT_VECTOR1 = "68fdb1520939a142470e1d44d33081017cc00c70a43e3db61ffac27c176c7dea"
PCCT_VECTOR_KEYSET = "79460eff01575fa85d73a95b6c5fe615938d55a7d8860fa3c33c74a0464e92c5"
PCCT_VECTOR_PROOF = "f47c425c22e5e10837ca6cdaf1efb2b9fe69dd757a6cb7426d2714a19b362ec3"
PCCT_VECTOR_CALL = "3e6473c51a64ddca858393f256f48b52844eb1f7238e457d6551b93c95f00c8d"

NOW = 1800000000


def seed(b):
    return bytes([b]) * 32


ROOT_A, ROOT_B, TICKET_KEY, CLIENT, CLIENT2, WORKER = seed(1), seed(7), seed(3), seed(2), seed(4), seed(5)
ROOTS = (T.seed_public(ROOT_A), T.seed_public(ROOT_B))


def keyset(serial=5):
    return T.Keyset(serial, NOW - 86400, NOW + 45 * 86400, 3, 0, [
        T.KeysetEntry(7, T.seed_public(TICKET_KEY), NOW - 30 * 86400, NOW + 30 * 86400),
        T.KeysetEntry(8, T.seed_public(seed(6)), NOW - 86400, NOW + 60 * 86400),
        T.KeysetEntry(9, T.seed_public(WORKER), NOW - 86400, NOW + 60 * 86400, T.ROLE_WORKER,
                      ["fm2k:other", "wonderfulworld"])])


def vks(k=None):
    k = k or keyset()
    r, out = T.parse_keyset(T.sign_keyset(k, ROOT_B if k.root_id else ROOT_A), ROOTS, 0, 0)
    assert r == T.OK
    return out


def ticket(**kw):
    t = T.Ticket(7, bytes(0xA0 + i for i in range(16)), T.seed_public(CLIENT), NOW - 3600, NOW - 3600 + 48 * 3600,
                 1234, 4821, "Skeleton")
    for k, v in kw.items():
        setattr(t, k, v)
    return t


def resign(b, key=TICKET_KEY):
    b = bytes(b)
    return b[:188] + T.sign(key, T.TAG_TICKET, b[:188])


def check(b, now=NOW):
    return T.verify_ticket(bytes(b), vks(), now)[0]


def test_tags_and_layout():
    for a in T.ALL_TAGS:
        for b in T.ALL_TAGS:
            assert a is b or not b.startswith(a)
    assert T.OFF["name"] == 91 and T.OFF["sig"] == 188 and T.TICKET_SIZE == 252


def test_vector1_ticket_bytes_pinned():
    b = T.sign_ticket(ticket(), TICKET_KEY)
    assert len(b) == 252
    assert T.sha256(b).hex() == PCCT_VECTOR1
    r, t = T.verify_ticket(b, vks(), NOW)
    assert r == T.OK and t.name == "Skeleton" and t.tag == 4821 and t.serial == 1234 and t.key_gen == 7
    assert T.sign_ticket(t, TICKET_KEY) == b  # encode(decode(x)) == x
    assert t.display == "Skeleton#4821"


def test_vector_keyset_bytes_pinned():
    b = T.sign_keyset(keyset(), ROOT_A)
    assert T.sha256(b).hex() == PCCT_VECTOR_KEYSET
    r, k = T.parse_keyset(b, ROOTS, 5, NOW)
    assert r == T.OK and len(k.keys) == 3
    assert k.keys[2].role == T.ROLE_WORKER and k.keys[2].scope == ["fm2k:other", "wonderfulworld"]
    assert T.sign_keyset(k, ROOT_A) == b


def test_vector_proof_message_pinned():
    ta = T.sign_ticket(ticket(), TICKET_KEY)
    tb = T.sign_ticket(ticket(client_pub=T.seed_public(CLIENT2), name="Other"), TICKET_KEY)
    pm = T.proof_message(b"fm2k:ww", b"b1", b"S", b"\x11" * 16, b"\x22" * 16, T.ticket_id(ta), T.ticket_id(tb), 0)
    assert T.sha256(pm).hex() == PCCT_VECTOR_PROOF


def test_vector_call_message_pinned():
    cm = T.call_message(b"api.2dfm.org", b"POST", b"/v1/ticket/refresh", b"\x5a" * 32, NOW, b"\x07" * 16,
                        b"\x77" * 32)
    assert T.sha256(cm).hex() == PCCT_VECTOR_CALL
    sig = T.sign(CLIENT, b"", cm)
    assert T.verify_call(T.seed_public(CLIENT), cm, sig)
    other = T.call_message(b"staging.2dfm.org", b"POST", b"/v1/ticket/refresh", b"\x5a" * 32, NOW, b"\x07" * 16,
                           b"\x77" * 32)
    assert not T.verify_call(T.seed_public(CLIENT), other, sig)


def test_signature_domain_and_key_separation():
    b = T.sign_ticket(ticket(), TICKET_KEY)
    flipped = bytearray(b)
    flipped[91] ^= 1
    assert check(flipped) == "bad_signature"
    proof_domain = b[:188] + T.sign(TICKET_KEY, T.TAG_PROOF, b[:188])
    assert check(proof_domain) == "bad_signature"
    assert check(resign(b, seed(6))) == "bad_signature"
    gen8 = bytearray(b)
    gen8[12] = 8
    assert check(resign(gen8)) == "bad_signature"
    sl = bytearray(b)
    sl[188 + 32:] = (T._L).to_bytes(32, "little")  # S = L
    assert check(sl) == "bad_signature"


def test_length_magic_version_flags_padding():
    b = T.sign_ticket(ticket(), TICKET_KEY)
    for n in range(252):
        assert check(b[:n]) == "unknown_format"
    assert check(b + b"\0") == "unknown_format"
    assert check(b"X" + b[1:]) == "unknown_format"
    for v in (0, 2, 255):
        x = bytearray(b)
        x[8] = v
        assert check(resign(x)) == "newer_format"
    for at, val, want in ((10, 1, "unknown_flags"), (91 + 20, ord("x"), "bad_padding"), (187, 1, "bad_padding"),
                          (90, 0, "bad_name"), (90, 97, "bad_name"), (90, 4, "bad_padding")):
        x = bytearray(b)
        x[at] = val
        assert check(resign(x)) == want, (at, val)


def test_keys_unknown_revoked_window():
    b = T.sign_ticket(ticket(), TICKET_KEY)
    g9 = bytearray(b)
    g9[12] = 9
    assert check(resign(g9)) == "unknown_key"  # a worker key never signs a ticket
    k = keyset()
    k.revoked = [7]
    assert T.verify_ticket(b, vks(k), NOW)[0] == "revoked_key"
    k = keyset()
    k.min_key_gen = 8
    assert T.verify_ticket(b, vks(k), NOW)[0] == "revoked_key"
    k = keyset()
    k.keys[0].not_after = NOW - 7200
    assert T.verify_ticket(b, vks(k), NOW)[0] == "unknown_key"


def test_times():
    b = T.sign_ticket(ticket(), TICKET_KEY)
    t = ticket()
    assert check(b, t.issued - 300) == T.OK and check(b, t.issued - 301) == "not_yet_valid"
    assert check(b, t.expires + 300) == T.OK and check(b, t.expires + 301) == "expired"
    with pytest.raises(ValueError):
        T.sign_ticket(ticket(expires=t.issued + 73 * 3600), TICKET_KEY)
    forged = bytearray(b)
    forged[72:80] = (t.issued + 73 * 3600).to_bytes(8, "little")
    assert check(resign(forged)) == "lifetime"
    back = bytearray(b)
    back[72:80] = (t.issued - 1).to_bytes(8, "little")
    assert check(resign(back)) == "lifetime"


def test_client_keys_small_order_and_noncanonical():
    b = T.sign_ticket(ticket(), TICKET_KEY)
    identity = bytes([1]) + bytes(31)
    order2 = bytes([0xec]) + b"\xff" * 30 + bytes([0x7f])
    order4 = bytes(32)
    y_is_p = bytes([0xed]) + b"\xff" * 30 + bytes([0x7f])
    neg_zero = identity[:31] + bytes([0x80])
    for p in (identity, order2, order4, y_is_p, neg_zero):
        assert not T.pubkey_strong(p)
        x = bytearray(b)
        x[32:64] = p
        assert check(resign(x)) == "bad_client_key"
    assert T.pubkey_strong(T.seed_public(CLIENT))


def test_names_section10():
    ok = ["Skeleton", "Teo", "a b", "あいうえおかきくけこさしすせそたちつてとなにぬね", "é̂"]
    bad = ["abcdefghijklmnopqrstuvwxy", "", " a", "a ", "a  b", "a\0b", "a\x07", "a‮b", "a​b", "a️",
           "a\U000e0001", "ㅤ", "a­b", "⠀", "a　b", "é̂̃", "́a"]
    for n in ok:
        assert names.name_ok(n), n
    for n in bad:
        assert not names.name_ok(n), repr(n)
    for raw in (b"\xC0\xAF", b"\xED\xA0\x80", b"\xE3\x81", b"\xFF"):
        assert not names.name_ok(raw)


def test_service_normalization():
    assert names.normalize_name("  Ske​leton  ") == "Skeleton"
    assert names.normalize_name("a　　b") == "a b"
    assert names.normalize_name("é") == "é"  # NFC
    for bad in ("a\x07", "", "   ", "x" * 25, "\U000f0000"):
        with pytest.raises(names.BadName):
            names.normalize_name(bad)
    assert names.skeleton("Skeleton") == names.skeleton("SKELET0N") == names.skeleton("Ѕkeleton")
