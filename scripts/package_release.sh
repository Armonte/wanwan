#!/usr/bin/env bash
# Bundles the latest build into fm2k_v<version>.zip for upload to a
# GitHub release. Invoked manually after `./go.sh` succeeds. Looks up
# the version code from scripts/make_version.sh so the zip name and
# the launcher's reported version always stay in lockstep.
#
# Usage:   ./scripts/package_release.sh
#          ./scripts/package_release.sh 0.1.1   # override version
#
# Output is dropped in dist/ next to the repo root.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build"
OUT_DIR="$REPO_ROOT/dist"

if [ "$#" -ge 1 ]; then
    VERSION="$1"
else
    VERSION="$(grep -E '^FM2K_VERSION=' "$SCRIPT_DIR/make_version.sh" | head -1 | cut -d'"' -f2)"
fi
if [ -z "$VERSION" ]; then
    echo "package_release: couldn't derive version" >&2
    exit 1
fi

LAUNCHER="$BUILD_DIR/FM2K_RollbackLauncher.exe"
# Ninja drops the hook DLLs alongside the launcher (single-config build).
# Paths are relative to the project root because we cd'd here above.
HOOK_FM2K="$BUILD_DIR/FM2KHook.dll"
HOOK_FM95="$BUILD_DIR/FM95Hook.dll"
UPDATER="$BUILD_DIR/FM2KUpdater.exe"
LOCALES_SRC="$REPO_ROOT/locales"

for f in "$LAUNCHER" "$HOOK_FM2K" "$HOOK_FM95" "$UPDATER"; do
    if [ ! -f "$f" ]; then
        echo "package_release: missing $f — run ./go.sh first" >&2
        exit 1
    fi
done
if [ ! -d "$LOCALES_SRC" ]; then
    echo "package_release: missing $LOCALES_SRC — translation files won't ship" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
STAGED="$(mktemp -d)"
# Top-level files at the zip root — match the layout the launcher
# expects on disk after extraction (locale subfolder beside the EXE).
cp "$LAUNCHER" "$HOOK_FM2K" "$HOOK_FM95" "$UPDATER" "$STAGED/"
i686-w64-mingw32-strip --strip-debug "$STAGED/FM2K_RollbackLauncher.exe"
i686-w64-mingw32-strip --strip-debug "$STAGED/FM2KHook.dll"
i686-w64-mingw32-strip --strip-debug "$STAGED/FM95Hook.dll"
i686-w64-mingw32-strip --strip-debug "$STAGED/FM2KUpdater.exe"

# Locale .ini files under locales/. T() reads <exedir>\locales at
# runtime; without these every UI string falls back to its literal
# translation key, which looks completely broken to a fresh user.
mkdir -p "$STAGED/locales"
cp "$LOCALES_SRC"/*.ini "$STAGED/locales/"

ZIP_NAME="fm2k_v${VERSION}.zip"
ZIP_PATH="$OUT_DIR/$ZIP_NAME"
rm -f "$ZIP_PATH"
# Use -r (recursive) instead of -j (junk paths) so locales/ stays a
# subdirectory inside the zip. FM2KUpdater.exe extracts via tar -xf
# which preserves dir structure on its way into the install dir.
( cd "$STAGED" && zip -9 -r "$ZIP_PATH" \
    FM2K_RollbackLauncher.exe FM2KHook.dll FM95Hook.dll FM2KUpdater.exe \
    locales )

rm -rf "$STAGED"

# ── sign it here, not as a separate thing you have to remember ──
#
# The updater REFUSES a zip whose .sig is missing or does not verify, so a
# release packaged without this is a release nobody can install. Making it
# part of packaging means the failure is "the cut stopped", not "everyone's
# update broke and we found out from Discord".
"$(dirname "$0")/sign_release.sh" "$ZIP_PATH"

echo "wrote $ZIP_PATH"
echo
echo "next steps:"
echo "  1. cut release:  gh release create v${VERSION} ${ZIP_PATH} ${ZIP_PATH}.sig --title v${VERSION} --notes 'release notes'"
echo "  2. update LatestVersion in armonte/fm2ktest:"
echo "       (in fm2ktest checkout) echo '${VERSION}' > LatestVersion && git commit -am 'release v${VERSION}' && git push"
