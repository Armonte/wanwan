#!/usr/bin/env bash
# build_native.sh -- native x86_64 test build of the .fpk reconstructor.
#
# Lossless-only by default (zstd only). Pass FPK_WITH_OPUS=1 to additionally
# decode codec==1 audio via opusfile (needs the opus dev headers/libs on the
# include/link path -- see DEPS recon for the no-root /tmp/opusdev recipe).
#
# Gates (run against /tmp/fpk_out/*.fpk vs /mnt/d/Games/fm2k/RobotHeroes/Game/):
#   compare_regions.py ORIG OUT.bin          -- lossless gate (non-audio byte-exact)
#   dump_sounds.py     ORIG OUT.bin OUTDIR   -- audio gate (per-sound WAV dump,
#                                               channels+sample-rate preserved)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VFS="$HERE/../../FM2KHook/src/vfs"
OUT="$HERE/test_fpk_decode"

CXXFLAGS="-O2 -std=c++17 -Wall -Wextra -I$VFS"
LDFLAGS="-lzstd"

if [ "${FPK_WITH_OPUS:-0}" = "1" ]; then
    : "${OPUSDEV:=/tmp/opusdev}"
    CXXFLAGS="$CXXFLAGS -DFPK_WITH_OPUS -I$OPUSDEV/usr/include -I$OPUSDEV/usr/include/opus"
    LDFLAGS="$LDFLAGS -L$OPUSDEV/usr/lib/x86_64-linux-gnu -Wl,-rpath,$OPUSDEV/usr/lib/x86_64-linux-gnu -lopusfile -lopus -logg"
fi

set -x
g++ $CXXFLAGS \
    "$VFS/fpk_reader.cpp" \
    "$HERE/test_fpk_decode.cpp" \
    $LDFLAGS \
    -o "$OUT"
set +x
echo "built: $OUT"
