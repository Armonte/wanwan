#!/usr/bin/env bash
# build_native.sh -- native x86_64 build + run of the sound-xref test.
#
# Deliberately links ONLY studio/core/xref.cpp: kgt_file.h's structs are
# header-only aggregates and the test builds a KgtFile straight from the
# JSON fixture, so kgt_file.cpp is not (and must not become) a dependency.
#
# Fixture: bewear_fixture.json (regenerate with ./gen_fixture.py against
# /mnt/d/games/fm2k/_NODEV/pkmncc/Bewear.player).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE="$HERE/../../core"
OUT="$HERE/xref_test"

set -x
g++ -O2 -std=c++17 -Wall -Wextra -I"$CORE" \
    "$CORE/xref.cpp" \
    "$HERE/xref_test.cpp" \
    -o "$OUT"
set +x

"$OUT" "$HERE/bewear_fixture.json"
