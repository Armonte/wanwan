#!/bin/bash
# Contributor-facing build. Compiles the launcher + both hook DLLs and stages
# stripped copies into dist/. This is everything someone cloning the repo needs:
# NO deploy to /mnt/c/games and NO taskkill -- that author-specific step lives
# in go.sh. Run ./make_build.sh once first to configure (it also self-heals the
# submodule deps).
set -e
cd "$(dirname "$0")"

if [ ! -f build/build.ninja ]; then
    echo "build/ is not configured yet. Run ./make_build.sh first." >&2
    exit 1
fi

cd build
# All cores: cg2's /usr/bin/ninja wrapper clamps to PC_NINJA_MAX (default 8) and
# throttles above load PC_NINJA_LOAD; the pc-build cgroup still caps CPU/memory.
export PC_NINJA_MAX="${PC_NINJA_MAX:-$(nproc)}" PC_NINJA_LOAD="${PC_NINJA_LOAD:-$(( $(nproc) * 2 ))}"
ninja -j "$PC_NINJA_MAX" || { echo "Build failed!" >&2; exit 1; }

# Stage stripped binaries into dist/ (repo root). Debug info is ~25 MB/binary;
# stripping the staged copies keeps the unstripped originals in build/ for
# symbolication (addr2line / objdump) when diagnosing user logs.
DIST="$(cd .. && pwd)/dist"
mkdir -p "$DIST"
# llvm-strip from llvm-mingw when present (the default toolchain), else binutils.
STRIP="${LLVM_MINGW:-$HOME/tools/llvm-mingw-msvcrt}/bin/llvm-strip"
[ -x "$STRIP" ] || STRIP=i686-w64-mingw32-strip
for f in FM2K_RollbackLauncher.exe FM2KHook.dll FM95Hook.dll FM2KUpdater.exe; do
    if [ ! -f "$f" ]; then
        echo "Expected build output missing: build/$f" >&2
        exit 1
    fi
    # Re-stage only when the build output is newer than the staged copy:
    # stripping and copying ~30 MB over to the repo drive on every no-op build
    # was most of a no-op build's time.
    [ "$DIST/$f" -nt "$f" ] && continue
    cp "$f" "$DIST/"
    "$STRIP" --strip-debug "$DIST/$f"
done

# Locale .ini files. T() reads <exedir>\locales at runtime
# (launcher/game/FM2K_Locale.cpp Locale::Init) and LoadIni fails SILENTLY on a
# missing file, so a dist/ without these renders every UI string as its raw
# translation key with no error anywhere. Staging them here makes dist/ a
# complete deployable, which go.sh and package_release.sh both rely on.
mkdir -p "$DIST/locales"
cp -u "$(cd .. && pwd)/locales"/*.ini "$DIST/locales/"

echo "Build OK. Stripped binaries staged in dist/:"
ls -la "$DIST"/*.exe "$DIST"/*.dll | awk '{printf "  %-36s %s bytes\n", $NF, $5}'
