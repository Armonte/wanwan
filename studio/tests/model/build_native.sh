#!/usr/bin/env bash
# build_native.sh -- native x86_64 build + run of the real-model gate.
#
# Links the FULL data layer: kgt_file + xref + audio_convert + real_model.
# real_model/app_state pull no SDL or ImGui headers -- this compiling and
# running natively is exactly what the gate proves (playback/dialogs live
# in the app layer: audio_player.cpp, main.cpp).
#
# Usage: ./build_native.sh [PLAYER_FILE]
#   (default: /mnt/d/games/fm2k/_NODEV/pkmncc/Bewear.player)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STUDIO="$HERE/../.."
OUT="$HERE/model_test"

set -x
g++ -O2 -std=c++20 -Wall -Wextra \
    -I"$STUDIO/core" -I"$STUDIO/third_party" -I"$STUDIO/app" \
    "$STUDIO/core/kgt_file.cpp" \
    "$STUDIO/core/xref.cpp" \
    "$STUDIO/core/audio/audio_convert.cpp" \
    "$STUDIO/app/real_model.cpp" \
    "$HERE/model_test.cpp" \
    -o "$OUT"
set +x

"$OUT" "${1:-/mnt/d/games/fm2k/_NODEV/pkmncc/Bewear.player}"
