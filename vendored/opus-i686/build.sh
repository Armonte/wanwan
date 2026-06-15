#!/usr/bin/env bash
# Cross-build static libogg + libopus + libopusfile for i686-w64-mingw32,
# install headers+libs into this dir so FM2KHook can link the FPK_WITH_OPUS
# audio path (op_open_memory/op_read) in the 32-bit DLL.
#
# Release tarballs ship a pre-generated ./configure, so no autoconf/automake/
# libtool needed. libogg/libopus build via CMake; opusfile via its shipped
# configure (--host cross). Re-runnable; downloads cached in /tmp/opus_src.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PREFIX="$HERE"                       # vendored/opus-i686 (include/ + lib/)
SRC=/tmp/opus_src
HOST=i686-w64-mingw32
JOBS="$(nproc)"

OGG_V=1.3.5
OPUS_V=1.4
OPUSFILE_V=0.12

mkdir -p "$SRC" "$PREFIX"
cd "$SRC"

fetch() { # url
  local f; f="$(basename "$1")"
  [ -s "$f" ] || { echo ">> download $f"; curl -fsSL --retry 3 -o "$f" "$1"; }
  tar xf "$f"
}
fetch "https://downloads.xiph.org/releases/ogg/libogg-${OGG_V}.tar.gz"
fetch "https://downloads.xiph.org/releases/opus/opus-${OPUS_V}.tar.gz"
fetch "https://downloads.xiph.org/releases/opus/opusfile-${OPUSFILE_V}.tar.gz"

# CMake cross toolchain file
TC="$SRC/mingw32-toolchain.cmake"
cat > "$TC" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)
set(CMAKE_C_COMPILER ${HOST}-gcc)
set(CMAKE_CXX_COMPILER ${HOST}-g++)
set(CMAKE_RC_COMPILER ${HOST}-windres)
set(CMAKE_FIND_ROOT_PATH /usr/${HOST})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF

echo ">> build libogg ${OGG_V}"
cmake -S "libogg-${OGG_V}" -B build-ogg -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DINSTALL_DOCS=OFF \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
cmake --build build-ogg --target install -j"$JOBS" >/dev/null

# i686 MinGW defaults to x87 / no SSE2. libopus's float decode (the bulk of FPK
# stage-audio inflate cost) runs much faster with SSE2 + -O3; enable it for the
# decoder so a stage's minutes of BGM decode in a fraction of the time. All
# target CPUs (Pentium 4+) have SSE2; render_simd.cpp already requires it.
OPT_CFLAGS="-O3 -msse2 -mfpmath=sse"

echo ">> build libopus ${OPUS_V}"
cmake -S "opus-${OPUS_V}" -B build-opus -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF \
  -DCMAKE_C_FLAGS="$OPT_CFLAGS" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
cmake --build build-opus --target install -j"$JOBS" >/dev/null

echo ">> build opusfile ${OPUSFILE_V} (configure cross, no http)"
cd "$SRC/opusfile-${OPUSFILE_V}"
make distclean >/dev/null 2>&1 || true
CFLAGS="$OPT_CFLAGS" ./configure --host="$HOST" --prefix="$PREFIX" \
  --disable-shared --enable-static --disable-http --disable-examples --disable-doc \
  DEPS_CFLAGS="-I$PREFIX/include -I$PREFIX/include/opus" \
  DEPS_LIBS="-L$PREFIX/lib -lopus -logg" >/dev/null
make -j"$JOBS" >/dev/null
make install >/dev/null
cd "$SRC"

echo ">> installed:"
ls -la "$PREFIX/lib/"*.a
echo ">> headers:"; ls "$PREFIX/include" "$PREFIX/include/opus" 2>/dev/null | tr '\n' ' '; echo
echo "OPUS_I686_BUILD_DONE"
