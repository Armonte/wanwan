#!/usr/bin/env bash
# sign_release.sh -- sign a packaged release zip so the updater can refuse
# anything that is not ours. Run after package_release.sh, before upload.
#
#   scripts/sign_release.sh [dist/fm2k_v0.2.85.zip]
#
# Produces <zip>.sig next to it. BOTH files go up as release assets: the
# updater fetches the .sig by convention (<zip URL> + ".sig") and will not
# extract a download whose signature does not verify.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KEY="${FM2K_SIGNING_KEY:-$HOME/.config/fm2k/release_signing.pem}"

ZIP="${1:-}"
if [ -z "$ZIP" ]; then
    ZIP="$(ls -1t "$ROOT"/dist/fm2k_v*.zip 2>/dev/null | head -1 || true)"
fi
[ -n "$ZIP" ] && [ -f "$ZIP" ] || { echo "no release zip found (pass one)" >&2; exit 1; }

if [ ! -f "$KEY" ]; then
    echo "no signing key at $KEY" >&2
    echo "run scripts/make_signing_key.sh once, then keep that file safe." >&2
    exit 1
fi

openssl dgst -sha256 -sign "$KEY" -out "$ZIP.sig" "$ZIP"

# Verify what we just wrote, against the PUBLIC half, the same way the
# launcher will. Signing and never checking is how you find out at the
# worst possible moment that the key was wrong.
PUB="$(mktemp)"; trap 'rm -f "$PUB"' EXIT
openssl rsa -in "$KEY" -pubout -out "$PUB" 2>/dev/null
openssl dgst -sha256 -verify "$PUB" -signature "$ZIP.sig" "$ZIP"

echo "signed: $ZIP.sig  ($(stat -c%s "$ZIP.sig") bytes)"
echo "upload BOTH $(basename "$ZIP") and $(basename "$ZIP").sig as release assets."
