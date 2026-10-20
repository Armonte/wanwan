#!/usr/bin/env bash
# Build the original-side parity DLL and the injector (llvm-mingw clang, i686).
# The DLL compiles the port's parity/parity_core.c plus the cp932 layer, the
# byte-exact ini it routes to and the file layer the ini opens through (pfile.c)
# (PORT_DIR, default the main fm2k-port checkout).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CC="${CC:-${LLVM_MINGW_DLL:-$HOME/tools/llvm-mingw}/bin/i686-w64-mingw32-clang}"
PORT_DIR="${PORT_DIR:-$(cd "$HERE/../../.." && pwd)/fm2k-port}"   # sibling checkout of the port
# The sibling guess follows the PHYSICAL path: D:\dev\wanwan is a junction onto C:\dev\wanwan, so it lands in C:\dev,
# where no port checkout exists. Say so instead of failing on the first missing source.
[ -f "$PORT_DIR/parity/parity_core.c" ] || { echo "parity_port: no fm2k-port at $PORT_DIR -- set PORT_DIR=<your fm2k-port checkout>" >&2; exit 1; }
mkdir -p "$HERE/build"
INC=()
while IFS= read -r d; do INC+=("-I$d"); done < <(find "$PORT_DIR/engine" "$PORT_DIR/platform" "$PORT_DIR/shell" "$PORT_DIR/parity" -name '*.h' -printf '%h\n' | sort -u)
"$CC" -O2 -shared -static-libgcc "${INC[@]}" -I"$PORT_DIR/deps/sdl3/include" -fwrapv -fno-strict-aliasing -Wno-switch \
    "$HERE/fm2k_parity_dll.c" "$PORT_DIR/parity/parity_core.c" "$PORT_DIR/platform/cp932.c" "$PORT_DIR/platform/ini.c" "$PORT_DIR/platform/pfile.c" "$PORT_DIR/platform/frame_hash.c" "$PORT_DIR/platform/kaddr.c" \
    -lwinmm -lgdi32 -o "$HERE/build/fm2k_parity.dll"
"$CC" -O2 "$HERE/fm2k_parity_inject.c" -o "$HERE/build/fm2k_parity_inject.exe"
ls -la "$HERE/build"
