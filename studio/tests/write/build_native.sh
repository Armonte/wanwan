#!/usr/bin/env bash
# build_native.sh -- native x86_64 build + run of the write-path gate
# (edit_session + the RealModel write wrappers; Phase 4 contract).
#
# Links the FULL data layer -- kgt_file + xref + audio_convert +
# real_model + edit_session -- with no SDL and no ImGui, same separation
# proof as tests/model/. The test copies the corpus file into a fresh
# temp dir and never opens the source for writing.
#
# Usage: ./build_native.sh [PLAYER_FILE]
#   (default: /mnt/d/games/fm2k/_NODEV/pkmncc/Bewear.player)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STUDIO="$HERE/../.."
OUT="$HERE/write_test"

set -x
g++ -O2 -std=c++20 -Wall -Wextra \
    -I"$STUDIO/core" -I"$STUDIO/third_party" -I"$STUDIO/app" \
    "$STUDIO/core/kgt_file.cpp" \
    "$STUDIO/core/xref.cpp" \
    "$STUDIO/core/audio/audio_convert.cpp" \
    "$STUDIO/app/real_model.cpp" \
    "$STUDIO/app/edit_session.cpp" \
    "$HERE/write_test.cpp" \
    -o "$OUT"
set +x

"$OUT" "${1:-/mnt/d/games/fm2k/_NODEV/pkmncc/Bewear.player}"
