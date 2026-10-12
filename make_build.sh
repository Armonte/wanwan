#!/bin/bash
set -e
set -x

# Toolchain. Default: llvm-mingw, msvcrt variant (one exe for Windows 7 SP1
# through 11, no redistributable). TOOLCHAIN=gcc keeps the old mingw-w64 GCC
# build for one release while the switch settles.
TOOLCHAIN="${TOOLCHAIN:-llvm}"
LLVM_MINGW="${LLVM_MINGW:-$HOME/tools/llvm-mingw-msvcrt}"
LLVM_MINGW_TAG="${LLVM_MINGW_TAG:-20261006}"
if [ "$TOOLCHAIN" = gcc ]; then
    if ! command -v i686-w64-mingw32-gcc >/dev/null 2>&1; then
        echo "MinGW-w64 cross compiler not found. Installing required packages..."
        sudo apt update
        sudo apt install -y gcc-mingw-w64-i686 g++-mingw-w64-i686 binutils-mingw-w64-i686 ninja-build
    fi
elif [ ! -x "$LLVM_MINGW/bin/i686-w64-mingw32-clang" ]; then
    echo "llvm-mingw (msvcrt) not found at $LLVM_MINGW -- downloading release $LLVM_MINGW_TAG"
    mkdir -p "$(dirname "$LLVM_MINGW")"
    pkg="llvm-mingw-${LLVM_MINGW_TAG}-msvcrt-ubuntu-22.04-x86_64"
    curl -fL "https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_TAG}/${pkg}.tar.xz" \
        | tar -xJ -C "$(dirname "$LLVM_MINGW")"
    ln -sfn "$(dirname "$LLVM_MINGW")/$pkg" "$LLVM_MINGW"
fi
command -v ninja >/dev/null 2>&1 || { sudo apt update; sudo apt install -y ninja-build; }
export LLVM_MINGW

# Self-heal the submodule deps so a fresh clone (even one that forgot
# --recursive) builds. GekkoNet and the kgt-parity header are flattened
# in-tree, so they need no init here. SDL_image's nested image-codec
# submodules are initialized SELECTIVELY -- only the formats the build uses
# (PNG = libpng + zlib, JPEG, TIFF) -- so we don't drag in the huge,
# force-disabled AVIF/JXL/WEBP codec repos (aom, dav1d, libavif, libjxl,
# libwebp), which would add hundreds of MB to every clone for nothing.
if git rev-parse --git-dir >/dev/null 2>&1; then
    git submodule sync --quiet
    git submodule update --init \
        vendored/SDL vendored/SDL_image vendored/imgui vendored/minhook vendored/miniupnp
    git -C vendored/SDL_image submodule update --init \
        external/jpeg external/libpng external/libtiff external/zlib
fi

# The build tree lives on WSL's own ext4 disk, not on the repo's drive: on a
# Windows drive mounted over 9P every stat, object write and link is several
# times slower. build/ is a symlink to it. An existing in-tree build/ is moved
# over intact (no recompile). FM2K_BUILD_IN_TREE=1 keeps the old layout.
if [ "${FM2K_BUILD_IN_TREE:-0}" != 1 ]; then
    repo_id="$(basename "$PWD")-$(printf '%s' "$PWD" | md5sum | cut -c1-8)"
    BUILD_DIR="${FM2K_BUILD_DIR:-$HOME/.cache/fm2k-build/$repo_id}"
    mkdir -p "$(dirname "$BUILD_DIR")"
    if [ -d build ] && [ ! -L build ]; then
        if [ -e "$BUILD_DIR" ]; then rm -rf "$BUILD_DIR"; fi
        echo "moving build/ to $BUILD_DIR"
        mv build "$BUILD_DIR"
    fi
    mkdir -p "$BUILD_DIR"
    [ -L build ] && [ "$(readlink build)" = "$BUILD_DIR" ] || { rm -f build; ln -s "$BUILD_DIR" build; }
elif [ ! -d build ]; then
    mkdir build
fi

cd build # change to build directory

# A compiler switch needs a fresh CMake cache. Reset only the cache (not the
# objects or the unstripped binaries) when the configured toolchain differs.
if [ -f CMakeCache.txt ]; then
    cached="$(grep -m1 '^CMAKE_CXX_COMPILER:' CMakeCache.txt | cut -d= -f2)"
    case "$TOOLCHAIN:$cached" in
        gcc:*clang*|llvm:*g++*) echo "toolchain changed ($cached) -- resetting the CMake cache"
                               rm -rf CMakeCache.txt CMakeFiles ;;
    esac
fi

# Load build secrets (FM2K_LOG_UPLOAD_SECRET etc.) into the environment so
# CMake bakes them at configure time. Without this the crash/desync auto-upload
# secret bakes empty and EVERY upload is 4xx-rejected (telemetry blackout).
# `set +x` around the source so the secret is never echoed to the build log.
if [ -f "$HOME/.config/fm2k-release.env" ]; then
    set +x
    set -a
    . "$HOME/.config/fm2k-release.env"
    set +a
    set -x
fi

# Force CMake to run
# ccache when available (its cache lives on ext4 under ~/.cache/ccache).
LAUNCHER=""
command -v ccache >/dev/null 2>&1 && LAUNCHER="-D CMAKE_C_COMPILER_LAUNCHER=ccache -D CMAKE_CXX_COMPILER_LAUNCHER=ccache"
DEBUG_FLAGS=""
[ "${DEBUG:-0}" = 1 ] && DEBUG_FLAGS="-D FM2K_DEBUG_FLAGS=-g"
REPO="$(cd .. && pwd)"
# Prebuilt third-party libraries (rebuilt only when their inputs change).
DEPS_ARG=""
if [ "$TOOLCHAIN" != gcc ]; then
    DEPS_PREFIX="$("$REPO/scripts/build_deps.sh" | tail -1)"
    DEPS_ARG="-D FM2K_DEPS_PREFIX=$DEPS_PREFIX"
fi
if [ "$TOOLCHAIN" != gcc ]; then
cmake "$REPO" -G "Ninja" -D CMAKE_BUILD_TYPE=RelWithDebInfo $LAUNCHER $DEBUG_FLAGS $DEPS_ARG \
    -D CMAKE_TOOLCHAIN_FILE="$REPO/cmake/llvm-mingw-i686.cmake" \
    -D SDL_DISABLE_WINDOWS_VERSION_RESOURCE=ON \
    -D CMAKE_C_FLAGS="-w" \
    -D CMAKE_CXX_FLAGS="-w" \
    -D CMAKE_EXE_LINKER_FLAGS="-static" \
    -D CMAKE_SHARED_LINKER_FLAGS="-static"
else
cmake "$REPO" -G "Ninja" -D CMAKE_BUILD_TYPE=RelWithDebInfo $LAUNCHER \
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
fi

echo "CMake configuration completed. Run 'bash go.sh' to build." 