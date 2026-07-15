#!/usr/bin/env bash
# build_native.sh -- native x86_64 build + run of the audio_convert tests.
#
# The real-file test wants an engine WAV extracted from a .player; set
# REAL_WAV to a pre-extracted path, or let this script pull one via
# tools/kgt/soundtool.py from PLAYER_FILE (default: pkmncc Bewear). If
# neither works the real-file test is skipped, the rest still run.
#
# Cross-compile sanity (the app ships as a mingw exe): if
# i686-w64-mingw32-g++ is on PATH, audio_convert.cpp is also
# compile-checked (-c, no link) for the 32-bit Windows target.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AUDIO="$HERE/.."
OUT="$HERE/audio_test"

CXXFLAGS="-O2 -std=c++17 -Wall -Wextra"

set -x
g++ $CXXFLAGS \
    "$AUDIO/audio_convert.cpp" \
    "$HERE/audio_test.cpp" \
    -o "$OUT"
set +x
echo "built: $OUT"

if command -v i686-w64-mingw32-g++ >/dev/null 2>&1; then
    set -x
    i686-w64-mingw32-g++ $CXXFLAGS -c "$AUDIO/audio_convert.cpp" \
        -o "$HERE/audio_convert_mingw_check.o"
    set +x
    rm -f "$HERE/audio_convert_mingw_check.o"
    echo "mingw compile-check: OK"
else
    echo "mingw compile-check: SKIPPED (i686-w64-mingw32-g++ not on PATH)"
fi

PLAYER_FILE="${PLAYER_FILE:-/mnt/d/games/fm2k/_NODEV/pkmncc/Bewear.player}"
if [ -z "${REAL_WAV:-}" ] && [ -f "$PLAYER_FILE" ]; then
    REAL_WAV="$(mktemp -t audio_test_real_XXXXXX.wav)"
    if ! python3 "$HERE/../../../../tools/kgt/soundtool.py" extract \
        "$PLAYER_FILE" --sound 7 -o "$REAL_WAV"; then
        rm -f "$REAL_WAV"
        REAL_WAV=""
    fi
fi

if [ -n "${REAL_WAV:-}" ]; then
    "$OUT" "$REAL_WAV"
else
    "$OUT"
fi
