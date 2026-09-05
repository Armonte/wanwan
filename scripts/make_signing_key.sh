#!/usr/bin/env bash
# make_signing_key.sh -- create the release-signing keypair, ONCE, and emit
# the public half as a C header the launcher compiles in.
#
# This is NOT Authenticode. It does not touch SmartScreen and it does not
# stop Windows calling us an unknown publisher -- that needs a CA-issued
# certificate on a hardware token or a cloud HSM. What it does is close the
# hole that actually exists today: FM2K_Updater downloads a zip over HTTPS
# and extracts it over the user's launcher and both hook DLLs WITHOUT
# CHECKING WHAT IT GOT. Anything that can answer for that URL -- a
# compromised token, a hijacked release asset, a proxy a user was talked
# into trusting -- currently ships code straight into every install.
#
# RSA-3072 + PKCS#1 v1.5 + SHA-256, because Windows verifies that through
# CNG on every version we support with no third-party crypto to vendor and
# no key material to get wrong. Ed25519 would be nicer and is Windows-11-only
# in CNG, which is not a trade worth making for this.
#
# THE PRIVATE KEY NEVER ENTERS THE REPO. Default location is
# ~/.config/fm2k/release_signing.pem, 0600. Back it up somewhere you would
# not mind explaining -- losing it means every future release has to be
# published under a new key and every old client stops accepting updates.
set -euo pipefail

KEY="${FM2K_SIGNING_KEY:-$HOME/.config/fm2k/release_signing.pem}"
HDR="$(cd "$(dirname "$0")/.." && pwd)/launcher/net/FM2K_ReleaseKey.h"

if [ -f "$KEY" ]; then
    echo "refusing to overwrite an existing key: $KEY" >&2
    echo "delete it deliberately if you really mean to rotate." >&2
    exit 1
fi

mkdir -p "$(dirname "$KEY")"
umask 077
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out "$KEY"
chmod 600 "$KEY"
echo "private key -> $KEY (0600, NOT in the repo)"

"$(dirname "$0")/emit_release_key_header.py" "$KEY" "$HDR"
echo "public key  -> $HDR (commit this)"
