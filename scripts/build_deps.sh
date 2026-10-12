#!/usr/bin/env bash
# Build the third-party libraries once into a prefix on WSL's ext4 disk:
# SDL 3.4 (static), SDL3_image (vendored/SDL_image), and our patched GekkoNet.
# The main build imports them instead of compiling their thousands of files in
# its own graph -- on the repo's 9P-mounted drive, every one of those files was
# a ~2 ms stat on every build, including no-op builds.
# Each part is rebuilt only when its inputs change (stamp files). FRESH=1
# rebuilds everything. Prints the prefix on its last line.
set -euo pipefail
cd "$(dirname "$0")/.."
REPO="$PWD"
TC="$REPO/cmake/llvm-mingw-i686.cmake"
SDL_TAG="${SDL_TAG:-release-3.4.18}"
repo_id="$(basename "$REPO")-$(printf '%s' "$REPO" | md5sum | cut -c1-8)"
ROOT="${FM2K_DEPS_ROOT:-$HOME/.cache/fm2k-build/deps-$repo_id}"
PREFIX="$ROOT/prefix"
TCID="$(readlink -f "${LLVM_MINGW:-$HOME/tools/llvm-mingw-msvcrt}")"
[ "${FRESH:-0}" = 1 ] && rm -rf "$ROOT"
mkdir -p "$ROOT/src" "$PREFIX"
common=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX"
        -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_C_FLAGS=-w -DCMAKE_CXX_FLAGS=-w)
command -v ccache >/dev/null && common+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
export PC_NINJA_MAX="${PC_NINJA_MAX:-$(nproc)}" PC_NINJA_LOAD="${PC_NINJA_LOAD:-$(( $(nproc) * 2 ))}"

part() {  # part <name> <stamp-value> <cmake source dir> [extra cmake args...]
    local name="$1" want="$2" src="$3"; shift 3
    local stamp="$ROOT/$name.stamp"
    if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ]; then return; fi
    echo "deps: building $name"
    cmake -S "$src" -B "$ROOT/build-$name" "${common[@]}" "$@" >/dev/null
    cmake --build "$ROOT/build-$name" -- -j "$PC_NINJA_MAX"
    cmake --install "$ROOT/build-$name" >/dev/null
    echo "$want" > "$stamp"
}

[ -d "$ROOT/src/SDL" ] || git clone -q --depth 1 -b "$SDL_TAG" https://github.com/libsdl-org/SDL.git "$ROOT/src/SDL"
part sdl3 "$SDL_TAG|$TCID" "$ROOT/src/SDL" \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_DISABLE_WINDOWS_VERSION_RESOURCE=ON
part sdl3_image "$(git rev-parse HEAD:vendored/SDL_image)|$SDL_TAG|$TCID" "$REPO/vendored/SDL_image" \
    -DBUILD_SHARED_LIBS=OFF -DSDLIMAGE_VENDORED=ON -DSDLIMAGE_AVIF=OFF -DSDLIMAGE_JXL=OFF -DSDLIMAGE_WEBP=OFF \
    -DSDLIMAGE_SAMPLES=OFF -DSDLIMAGE_TESTS=OFF -DSDLIMAGE_INSTALL=ON -DSDLIMAGE_DEPS_SHARED=OFF
# GekkoNet changes with our patches: key on its git tree plus any uncommitted edits.
gk="$(git rev-parse HEAD:vendored/GekkoNet)$(git diff --quiet HEAD -- vendored/GekkoNet || git diff HEAD -- vendored/GekkoNet | md5sum | cut -c1-12)"
part gekkonet "$gk|$TCID" "$REPO/cmake/deps/gekkonet"
echo "$PREFIX"
