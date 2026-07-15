#!/bin/bash
# 2dfm Studio build -- same mingw i686 cross toolchain as the root
# make_build.sh (flags mirrored verbatim), but fully standalone:
# configures into studio/build and stages the exe into studio/dist/.
# Does NOT touch the root build/ tree or vendored/.
set -e
cd "$(dirname "$0")"

if ! command -v i686-w64-mingw32-gcc >/dev/null 2>&1; then
    echo "MinGW-w64 cross compiler not found. Install with:" >&2
    echo "  sudo apt install gcc-mingw-w64-i686 g++-mingw-w64-i686 binutils-mingw-w64-i686 ninja-build" >&2
    exit 1
fi

cmake -S . -B build -G Ninja -D CMAKE_BUILD_TYPE=Release \
    -D CMAKE_C_COMPILER=i686-w64-mingw32-gcc \
    -D CMAKE_CXX_COMPILER=i686-w64-mingw32-g++ \
    -D CMAKE_ASM_COMPILER=i686-w64-mingw32-gcc \
    -D CMAKE_RC_COMPILER=i686-w64-mingw32-windres \
    -D CMAKE_SYSTEM_NAME=Windows \
    -D CMAKE_SYSTEM_PROCESSOR=i686 \
    -D SDL_DISABLE_WINDOWS_VERSION_RESOURCE=ON \
    -D CMAKE_C_FLAGS="-w -static-libgcc" \
    -D CMAKE_CXX_FLAGS="-w -static-libgcc -static-libstdc++" \
    -D CMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -D CMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -D CMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY

cmake --build build

mkdir -p dist
cp build/2dfm-studio.exe dist/
i686-w64-mingw32-strip --strip-debug dist/2dfm-studio.exe

echo "OK: studio/dist/2dfm-studio.exe"
file dist/2dfm-studio.exe
ls -la dist/2dfm-studio.exe
