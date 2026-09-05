#!/usr/bin/env python3
"""Gate: does the public key COMPILED INTO THE LAUNCHER verify a signature
made by the private key on disk?

The signing side (openssl) and the verifying side (CNG) never meet on this
machine -- CNG is Windows-only -- so the one link that can silently be
wrong is the BCRYPT_RSAPUBLIC_BLOB layout in FM2K_ReleaseKey.h. Get the
field order or the endianness wrong and everything still builds, still
signs, still uploads, and every client refuses every update.

So: parse the header back into (exponent, modulus) exactly the way CNG
would, rebuild the public key from THAT, and check a real signature with
it. If this passes, the bytes in the header are the right key in the right
shape, and the only untested step left is BCryptImportKeyPair accepting a
layout that matches the documented struct.
"""
import re, struct, subprocess, sys, tempfile, os, hashlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HDR = os.path.join(ROOT, "launcher", "net", "FM2K_ReleaseKey.h")
KEY = os.environ.get("FM2K_SIGNING_KEY",
                     os.path.expanduser("~/.config/fm2k/release_signing.pem"))
BCRYPT_RSAPUBLIC_MAGIC = 0x31415352


def blob_from_header(path):
    src = open(path).read()
    body = src[src.index("kReleasePublicKeyBlob[] = {") :]
    body = body[body.index("{") + 1 : body.index("}")]
    return bytes(int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})", body))


def parse_blob(blob):
    magic, bits, cb_exp, cb_mod, p1, p2 = struct.unpack_from("<6I", blob, 0)
    assert magic == BCRYPT_RSAPUBLIC_MAGIC, f"bad magic 0x{magic:08X}"
    assert p1 == 0 and p2 == 0, "public blob must carry no primes"
    assert cb_mod * 8 == bits, f"BitLength {bits} != modulus {cb_mod * 8}"
    off = 24
    exp = int.from_bytes(blob[off : off + cb_exp], "big")
    off += cb_exp
    mod = int.from_bytes(blob[off : off + cb_mod], "big")
    assert off + cb_mod == len(blob), "trailing bytes after modulus"
    return bits, exp, mod


def main():
    blob = blob_from_header(HDR)
    bits, exp, mod = parse_blob(blob)
    print(f"header: RSA-{bits}, e={exp}, blob {len(blob)} bytes")

    if not os.path.exists(KEY):
        sys.exit(f"no private key at {KEY} -- run scripts/make_signing_key.sh")

    # Rebuild a PEM from ONLY what the header carries, so the verify below
    # cannot accidentally use the private key's own public half.
    try:
        from cryptography.hazmat.primitives.asymmetric import rsa, padding
        from cryptography.hazmat.primitives import hashes, serialization
    except ImportError:
        sys.exit("needs python3-cryptography (apt install python3-cryptography)")

    pub = rsa.RSAPublicNumbers(exp, mod).public_key()

    payload = os.urandom(4096)
    with tempfile.TemporaryDirectory() as td:
        f = os.path.join(td, "payload.bin")
        open(f, "wb").write(payload)
        sig = os.path.join(td, "payload.bin.sig")
        subprocess.run(["openssl", "dgst", "-sha256", "-sign", KEY,
                        "-out", sig, f], check=True)
        signature = open(sig, "rb").read()

    pub.verify(signature, payload, padding.PKCS1v15(), hashes.SHA256())
    print(f"OK: header key verifies a {len(signature)}-byte signature "
          f"made by {KEY}")

    # And prove a tampered payload is rejected, so a pass above cannot be
    # a verify that accepts anything.
    try:
        pub.verify(signature, payload[:-1] + bytes([payload[-1] ^ 1]),
                   padding.PKCS1v15(), hashes.SHA256())
    except Exception:
        print("OK: a one-bit change to the payload is rejected")
    else:
        sys.exit("FAIL: verification accepted a modified payload")


if __name__ == "__main__":
    main()
