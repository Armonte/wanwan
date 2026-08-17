#!/bin/bash
# Author deploy convenience: build (via build.sh) then push the stripped
# binaries into the local Windows games tree for quick iteration. A contributor
# who runs this on a machine WITHOUT /mnt/c/games just gets the build -- the
# deploy half is skipped automatically, so `./go.sh` never fails on a clean
# clone. The canonical contributor flow is `./make_build.sh && ./build.sh`.
set -e
set -x
clear
cd "$(dirname "$0")"

# Build + stage stripped binaries into dist/ (shared with the contributor path).
./build.sh

# --- Author-only deploy below. Skipped when the local games tree is absent. ---
if [ ! -d /mnt/c/games ]; then
    set +x
    echo "deploy skipped: /mnt/c/games not present. Binaries are staged in dist/."
    exit 0
fi

# Kill any running launcher / game / updater before overwrite. WSL drvfs
# doesn't propagate Windows file locks, so cp would silently succeed against a
# running .exe -- disk gets new bytes but the in-memory process is still old,
# and tests run against the old code. /T cascades to spawned game children.
# `|| true` keeps a clean machine from failing.
taskkill.exe /F /T /IM FM2K_RollbackLauncher.exe 2>/dev/null || true
taskkill.exe /F /T /IM FM2KUpdater.exe           2>/dev/null || true
taskkill.exe /F /T /IM pkmncc.exe                2>/dev/null || true
taskkill.exe /F /T /IM CPW.exe                   2>/dev/null || true
sleep 0.3   # let Windows release file handles before cp

# Launcher distribution dir -- both DLLs sit alongside the launcher exe so
# FM2KGameInstance::GetDLLPath(engine) picks the right one per game.engine.
cp dist/FM2K_RollbackLauncher.exe \
   dist/FM2KHook.dll \
   dist/FM95Hook.dll \
   dist/FM2KUpdater.exe \
   /mnt/c/games/

# Per-game copies (Windows resolves the DLL from the game's own dir first when
# LoadLibrary is called via the remote thread). Drop both DLLs to
# /mnt/c/games/2dfm/wanwan so we can iterate quickly on either engine.
cp dist/FM2KHook.dll /mnt/c/games/2dfm/wanwan/
cp dist/FM95Hook.dll /mnt/c/games/2dfm/wanwan/
[ -d /mnt/c/games/2dfm/pkmncc ] && cp dist/FM2KHook.dll /mnt/c/games/2dfm/pkmncc/
# FM95 reference dir (clean CPW build) -- ships FM95Hook.dll for end-to-end test.
[ -d /mnt/c/games/2dfm/fm95/CPW ] && cp dist/FM95Hook.dll /mnt/c/games/2dfm/fm95/CPW/

# Sync the locale .ini files alongside the launcher. T() reads them from
# <exedir>\locales at runtime; stale deployed copies cause new translation
# keys to render as the literal key name in the UI.
mkdir -p /mnt/c/games/locales
cp dist/locales/*.ini /mnt/c/games/locales/

echo "Build completed successfully!"
echo "Deployed (stripped) sizes:"
ls -la /mnt/c/games/FM2K_RollbackLauncher.exe /mnt/c/games/FM2KHook.dll | awk '{printf "  %-40s %s\n", $NF, $5}'
