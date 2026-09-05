#!/usr/bin/env bash
# build_host.sh -- link the Slint staticlib into an SDL3 exe (i686 mingw).
#
# Linked with -s: the unstripped exe was 46 MB, of which 24 MB was a
# 140k-entry COFF symbol table plus DWARF that nothing reads on a user's
# machine. Stripped it is 22 MB. What remains is code and tables, mostly
# Slint's font stack, every image codec the `image` crate ships, SVG
# support, and an SDL3 built with GPU renderers this exe never uses; those
# are Cargo-feature and CMake decisions, not link flags.
# Reuses the SDL3 the studio build already produced rather than building a
# third copy.
#
# The hub client is linked in from launcher/hub VERBATIM -- the same five
# TUs the shipping launcher compiles, not a copy. A second implementation
# of the protocol is exactly how a port ends up disagreeing with the
# product it is replacing. It needs only SDL_log, winhttp and the
# generated version_local.h at the repo root, so this costs one include
# path and one library.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$HERE/../.."

CARGO_TARGET_I686_PC_WINDOWS_GNU_LINKER=i686-w64-mingw32-gcc \
  cargo build --release --target i686-pc-windows-gnu --manifest-path "$HERE/Cargo.toml"

# In a git worktree the SDL submodule and the generated version_local.h live in
# the MAIN checkout: point FM2K_MAIN_ROOT at it (defaults to this tree).
MAIN="${FM2K_MAIN_ROOT:-$ROOT}"

# SDL: the lean build if someone made one (build_sdl_lean.sh -- video,
# OpenGL and input only, 3.6 MB against the studio's 5.0 MB), otherwise
# the studio's, which is what a fresh checkout has. Optional on purpose:
# a contributor who never runs the lean script still gets a launcher.
SDL_LIB="$HERE/../sdl_lean/libSDL3.a"
if [ ! -f "$SDL_LIB" ]; then
    SDL_LIB="$MAIN/studio/build/vendored_sdl/libSDL3.a"
fi
echo "SDL: $SDL_LIB"
i686-w64-mingw32-g++ -O2 -std=c++17 -static -static-libgcc -static-libstdc++ \
  -I"$MAIN/vendored/SDL/include" \
  -I"$ROOT/launcher/hub" \
  -I"$ROOT/launcher/render" \
  -I"$ROOT/launcher/game" \
  -I"$ROOT/vendored/miniz" \
  -I"$ROOT/FM2KHook/src/ui" \
  -I"$ROOT/FM2KHook/src" \
  -I"$ROOT/launcher/core" \
  -I"$ROOT/launcher/discovery" \
  -I"$ROOT/launcher/net" \
  -I"$ROOT/launcher/session" \
  -I"$ROOT/launcher/ui" \
  -I"$ROOT/FM2KHook/src/util" \
  -I"$ROOT/vendored/miniupnp/miniupnpc/include" \
  -I"$MAIN/build/miniupnpc_gen" \
  -DMINIUPNP_STATICLIB \
  -I"$ROOT/launcher" \
  -I"$MAIN/vendored/imgui" \
  -I"$MAIN/vendored/imgui/backends" \
  -I"$MAIN/vendored/minhook/include" \
  -I"$ROOT/vendored/GekkoNet/GekkoLib/include" \
  -I"$ROOT" \
  -I"$MAIN" \
  "$HERE/host.cpp" \
  "$HERE/host_hub.cpp" \
  "$HERE/host_scan.cpp" \
  "$HERE/host_settings.cpp" \
  "$HERE/host_replays.cpp" \
  "$HERE/host_http.cpp" \
  "$HERE/host_library.cpp" \
  "$ROOT/launcher/hub/FM2K_HubClient.cpp" \
  "$ROOT/launcher/hub/FM2K_HubClient_transport.cpp" \
  "$ROOT/launcher/hub/FM2K_HubClient_dispatch.cpp" \
  "$ROOT/launcher/hub/FM2K_HubClient_outbound.cpp" \
  "$ROOT/launcher/hub/FM2K_HubClient_json.cpp" \
  "$ROOT/launcher/render/FM2K_CncDDraw.cpp" \
  "$ROOT/launcher/render/FM2K_DDrawRedirect.cpp" \
  "$HERE/host_binder.cpp" \
  "$HERE/host_launch.cpp" \
  "$HERE/host_captures.cpp" \
  "$HERE/host_discord.cpp" \
  "$HERE/host_services.cpp" \
  "$ROOT/launcher/net/FM2K_DiscordAuth.cpp" \
  "$ROOT/launcher/net/FM2K_Updater.cpp" \
  "$ROOT/launcher/net/FM2K_HttpPost.cpp" \
  "$ROOT/launcher/net/FM2K_UploadQueue.cpp" \
  "$ROOT/launcher/net/FM2K_ReplayUpload.cpp" \
  "$ROOT/launcher/net/FM2K_PortMapper.cpp" \
  "$ROOT/launcher/core/launcher_log.cpp" \
  "$ROOT/launcher/game/FM2K_KgtParser.cpp" \
  "$ROOT/FM2KHook/src/util/pii_scrub.cpp" \
  "$ROOT/launcher/net/FM2K_ReleaseVerify.cpp" \
  "$ROOT/launcher/core/FM2K_GameInstance.cpp" \
  "$ROOT/launcher/core/FM2K_GameInstance_ipc.cpp" \
  "$ROOT/launcher/game/FM2K_GameIni.cpp" \
  "$ROOT/launcher/ui/launcher_ui_settings_io.cpp" \
  "$ROOT/launcher/session/FM2K_OnlineLaunch.cpp" \
  "$ROOT/launcher/session/FM2K_MatchOutcome.cpp" \
  "$ROOT/launcher/core/FM2K_FolderPicker.cpp" \
  "$ROOT/launcher/game/FM2K_InstallHealth.cpp" \
  "$ROOT/FM2KHook/src/ui/input_binder.cpp" \
  "$ROOT/FM2KHook/src/ui/input_binder_gamepads.cpp" \
  "$ROOT/FM2KHook/src/ui/input_binder_profiles.cpp" \
  "$ROOT/FM2KHook/src/ui/input_binder_sample.cpp" \
  -x c "$ROOT/vendored/miniz/miniz.c" -x none \
  "$HERE/target/i686-pc-windows-gnu/release/libslint_i686_spike.a" \
  "$SDL_LIB" \
  "$MAIN/build/libminiupnpc.a" \
  -lwinhttp -liphlpapi -lws2_32 -luserenv -lbcrypt -lntdll -lole32 -loleaut32 -limm32 -lversion \
  -lsetupapi -lwinmm -lgdi32 -lshell32 -luuid -ladvapi32 -lcfgmgr32 -lxinput1_4 \
  -mwindows \
  -s -Wl,--gc-sections \
  -o "$HERE/slint_sdl3_skeleton.exe"

echo "built: $HERE/slint_sdl3_skeleton.exe"
