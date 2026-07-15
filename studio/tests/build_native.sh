#!/usr/bin/env bash
# build_native.sh -- native x86_64 build of the kgtcore corpus gate
# (pattern: tools/fpk_core/build_native.sh).
#
# Usage: ./build_native.sh && ./kgt_gate FILE_LIST
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE="$HERE/../core"
OUT="$HERE/kgt_gate"

set -x
g++ -O2 -std=c++17 -Wall -Wextra -I"$CORE" \
    "$CORE/kgt_file.cpp" \
    "$HERE/kgt_gate.cpp" \
    -o "$OUT"
set +x
echo "built: $OUT"
