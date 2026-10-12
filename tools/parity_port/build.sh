#!/usr/bin/env bash
# Build the original-side parity DLL and the injector (llvm-mingw clang, i686).
# The DLL compiles the port's src/parity_core.c (PORT_DIR, default the `parity` worktree).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CC="${CC:-/home/teo/tools/llvm-mingw/bin/i686-w64-mingw32-clang}"
PORT_DIR="${PORT_DIR:-/mnt/d/dev/fm2k-port-parity}"
mkdir -p "$HERE/build"
"$CC" -O2 -shared -static-libgcc -I"$PORT_DIR/src" -I"$PORT_DIR/include" -fwrapv -fno-strict-aliasing -Wno-switch \
    "$HERE/fm2k_parity_dll.c" "$PORT_DIR/src/parity_core.c" "$PORT_DIR/src/cp932.c" -lwinmm -lgdi32 \
    -o "$HERE/build/fm2k_parity.dll"
"$CC" -O2 "$HERE/fm2k_parity_inject.c" -o "$HERE/build/fm2k_parity_inject.exe"
ls -la "$HERE/build"
