#!/usr/bin/env bash
# build_native.sh -- native x86_64 build + run of the sprite-decode gate.
# Same shape as tests/model/build_native.sh; adds core/sprite_decode.cpp.
#
# Usage: ./build_native.sh [FILE.kgt]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STUDIO="$HERE/../.."
OUT="$HERE/sprite_test"

set -x
g++ -O2 -std=c++20 -Wall -Wextra \
    -I"$STUDIO/core" -I"$STUDIO/third_party" -I"$STUDIO/app" \
    "$STUDIO/core/kgt_file.cpp" \
    "$STUDIO/core/xref.cpp" \
    "$STUDIO/core/sprite_decode.cpp" \
    "$STUDIO/core/audio/audio_convert.cpp" \
    "$STUDIO/app/real_model.cpp" \
    "$STUDIO/app/edit_session.cpp" \
    "$HERE/sprite_test.cpp" \
    -o "$OUT"
set +x

"$OUT" "${1:-/mnt/c/games/mikyaku/Mikyaku Impact.kgt}"
