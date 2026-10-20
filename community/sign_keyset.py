#!/usr/bin/env python3
"""Sign a PovertyCaster community KEYSET with an offline root key. Run this ONLY on the machine that holds the root
keys; it needs nothing but Python 3 and the `cryptography` package, and touches no network.

    python3 sign_keyset.py keyset_N.body root_a.private.pem root_b.private.pem keyset_N.pckeys

The body comes from `admin.py keyset-body` on the server. Its root_id byte says which root signs; only that root's
key file is opened (pass "-" for the other). The key file may be a PKCS#8 PEM, 32 raw seed bytes, or the seed as 64
hex digits. The key must match the root public key compiled into PovertyCaster (COMMUNITY_SERVICE.md section 7), so a
wrong file is refused before anything is written. Copy the .pckeys file back to the server and run
`admin.py install-keyset` there.
"""
from __future__ import annotations

import argparse
import os
import sys

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

TAG_KEYSET = b"PC-COMMUNITY-KEYSET-V1\n"
MAGIC = b"PCKEYS1\0"
ROOT_ID_OFFSET = 8 + 2 + 2 + 8 + 8 + 8 + 4  # magic, version, flags, serial, issued, valid_until, min_key_gen
ROOTS = (bytes.fromhex("c9e1bf09a428508b860cd21cb3b49dcadc2c8e3e6abe17cd51e8155bfe8797de"),   # root A
         bytes.fromhex("b597400beba4adb355936c998653c6b2bbc621047446b79fef412100b7d1995b"))   # root B


def load_root(path: str) -> Ed25519PrivateKey:
    with open(path, "rb") as f:
        data = f.read()
    if b"-----BEGIN" in data:
        key = serialization.load_pem_private_key(data, password=None)
        if not isinstance(key, Ed25519PrivateKey):
            sys.exit(f"{path}: not an Ed25519 private key")
        return key
    text = data.strip()
    if len(text) == 64 and all(c in b"0123456789abcdefABCDEF" for c in text):
        return Ed25519PrivateKey.from_private_bytes(bytes.fromhex(text.decode()))
    if len(data) in (32, 64):  # a raw seed, or seed || public
        return Ed25519PrivateKey.from_private_bytes(data[:32])
    sys.exit(f"{path}: unrecognized key file (PKCS#8 PEM, 32 raw bytes or 64 hex digits)")


def raw_pub(key: Ed25519PrivateKey) -> bytes:
    return key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("body")
    p.add_argument("root_a")
    p.add_argument("root_b")
    p.add_argument("out")
    p.add_argument("--roots", help=argparse.SUPPRESS)  # tests and staging: HEXA,HEXB instead of the real roots
    a = p.parse_args(argv)
    roots = ROOTS
    if a.roots:
        roots = tuple(bytes.fromhex(x) for x in a.roots.split(","))
    with open(a.body, "rb") as f:
        body = f.read()
    if len(body) < ROOT_ID_OFFSET + 1 + 1 + 1 + 1 or body[:8] != MAGIC:
        sys.exit(f"{a.body}: not a KEYSET body")
    if len(body) > 8192:
        sys.exit(f"{a.body}: too large for a KEYSET body (was a signed .pckeys passed?)")
    root_id = body[ROOT_ID_OFFSET]
    if root_id > 1:
        sys.exit(f"{a.body}: root_id {root_id} is neither a (0) nor b (1)")
    path = (a.root_a, a.root_b)[root_id]
    if path == "-":
        sys.exit(f"this body names root {'ab'[root_id]}: pass that root's key file")
    key = load_root(path)
    if raw_pub(key) != roots[root_id]:
        sys.exit(f"{path} is not root {'ab'[root_id]} (public key {raw_pub(key).hex()})")
    sig = key.sign(TAG_KEYSET + body)
    key.public_key().verify(sig, TAG_KEYSET + body)
    if os.path.exists(a.out):
        sys.exit(f"{a.out} exists; not overwriting")
    with open(a.out, "wb") as f:
        f.write(body + sig)
    print(f"signed with root {'ab'[root_id]}: wrote {a.out} ({len(body) + 64} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
