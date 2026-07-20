cmake_minimum_required(VERSION 3.20)

# Set up cross-compilation for Windows target from WSL
# set(CMAKE_SYSTEM_NAME Windows)
# set(CMAKE_SYSTEM_PROCESSOR i686)
# set(CMAKE_CROSSCOMPILING TRUE)

# Configure compilers before project()
# set(CMAKE_C_COMPILER /usr/bin/i686-w64-mingw32-gcc)
# set(CMAKE_CXX_COMPILER /usr/bin/i686-w64-mingw32-g++)
# set(CMAKE_RC_COMPILER /usr/bin/i686-w64-mingw32-windres)
# set(CMAKE_ASM_COMPILER ${CMAKE_C_COMPILER})

# Configure paths
# set(CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32)
# set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
# set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Define project
project(FM2K_RollbackLauncher VERSION 1.0.0 LANGUAGES CXX C RC)

# Set C++ standard early
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MinGW-specific flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -m32")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -m32")

# Force static libraries for everything
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)

# Force skip GLIBC check
set(CMAKE_REQUIRED_DEFINITIONS "-D__GLIBC__=0")

# Pre-set GCC feature test results
set(HAVE_GDWARF_4 TRUE)
set(HAVE_GCC_WALL TRUE)
set(HAVE_GCC_WUNDEF TRUE)
set(HAVE_GCC_WFLOAT_CONVERSION TRUE)
set(HAVE_GCC_NO_STRICT_ALIASING TRUE)
set(HAVE_GCC_WDOCUMENTATION FALSE)
set(HAVE_GCC_WDOCUMENTATION_UNKNOWN_COMMAND FALSE)
set(HAVE_GCC_COMMENT_BLOCK_COMMANDS FALSE)
set(HAVE_CLANG_COMMENT_BLOCK_COMMANDS FALSE)
set(HAVE_GCC_WSHADOW TRUE)
set(HAVE_GCC_WUNUSED_LOCAL_TYPEDEFS TRUE)
set(HAVE_GCC_WIMPLICIT_FALLTHROUGH TRUE)
set(COMPILER_SUPPORTS_FDIAGNOSTICS_COLOR_ALWAYS TRUE)
set(LINKER_SUPPORTS_VERSION_SCRIPT TRUE)
set(COMPILER_SUPPORTS_GCC_ATOMICS TRUE)
set(HAVE_GCC_FVISIBILITY TRUE)
set(LINKER_SUPPORTS_WL_NO_UNDEFINED TRUE)
set(COMPILER_SUPPORTS_MMX TRUE)
set(COMPILER_SUPPORTS_SSE TRUE)
set(COMPILER_SUPPORTS_SSE2 TRUE)
set(COMPILER_SUPPORTS_SSE3 TRUE)
set(COMPILER_SUPPORTS_SSE4_1 TRUE)
set(COMPILER_SUPPORTS_SSE4_2 TRUE)
set(COMPILER_SUPPORTS_AVX TRUE)
set(COMPILER_SUPPORTS_AVX2 TRUE)
set(COMPILER_SUPPORTS_AVX512F TRUE)

# Pre-set system checks
set(__GLIBC__ FALSE)

# Pre-set header checks
set(HAVE_ALLOCA_H FALSE)
set(HAVE_FLOAT_H TRUE)
set(HAVE_ICONV_H TRUE)
set(HAVE_INTTYPES_H TRUE)
set(HAVE_LIMITS_H TRUE)
set(HAVE_MALLOC_H TRUE)
set(HAVE_MATH_H TRUE)
set(HAVE_MEMORY_H TRUE)
set(HAVE_SIGNAL_H TRUE)
set(HAVE_STDARG_H TRUE)
set(HAVE_STDBOOL_H TRUE)

# Configure symbol checks
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_REQUIRED_LIBRARIES "")
set(CMAKE_REQUIRED_QUIET TRUE)

# MinGW-GCC specific options
add_compile_options(-Wall -Wextra -Wpedantic)

include(FetchContent)

# SDL3 Configuration - Force static building
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Force static libraries for everything
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(SDL_SHARED OFF CACHE BOOL "Build SDL3 as shared library" FORCE)
set(SDL_STATIC ON CACHE BOOL "Build SDL3 as static library" FORCE)
set(SDL3IMAGE_INSTALL OFF CACHE BOOL "Install SDL3_image" FORCE)
set(SDL3IMAGE_BUILD_SHARED_LIBS OFF CACHE BOOL "Build SDL3_image shared" FORCE)
set(SDL3IMAGE_VENDORED ON CACHE BOOL "Use vendored libraries in SDL3_image" FORCE)
set(SDL3IMAGE_AVIF OFF CACHE BOOL "Disable AVIF support" FORCE)
set(SDL3IMAGE_JXL OFF CACHE BOOL "Disable JXL support" FORCE)
set(SDL3IMAGE_WEBP OFF CACHE BOOL "Disable WEBP support" FORCE)

# Use vendored SDL3 sources
FetchContent_Declare(
    SDL3
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendored/SDL
)

# Use vendored SDL_image sources
FetchContent_Declare(
    SDL3_image
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendored/SDL_image
)

# Use vendored SDL_net sources
FetchContent_Declare(
    SDL3_net
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendored/SDL_net
)

# Make SDL3 available
FetchContent_MakeAvailable(SDL3)

# Make SDL_image available after SDL3
FetchContent_MakeAvailable(SDL3_image)

# Make SDL_net available after SDL3
FetchContent_MakeAvailable(SDL3_net)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/launcher/core
    ${CMAKE_CURRENT_SOURCE_DIR}/launcher/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/launcher/hub
    ${CMAKE_CURRENT_SOURCE_DIR}/launcher/session
    ${CMAKE_CURRENT_SOURCE_DIR}/launcher/discovery
    ${CMAKE_CURRENT_SOURCE_DIR}/launcher/game
    ${CMAKE_CURRENT_SOURCE_DIR}/launcher/net
    ${CMAKE_CURRENT_SOURCE_DIR}/launcher/render
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/GekkoNet/include
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/imgui/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/minhook/include
    # Stock miniz lives in its own dir so its `#include "miniz.h"` resolves
    # without colliding with SDL_image's patched copy at vendored/SDL_image/src/.
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/miniz
)

# ImGui core sources (shared between launcher and hook)
set(IMGUI_CORE_SOURCES
    vendored/imgui/imgui.cpp
    vendored/imgui/imgui_demo.cpp
    vendored/imgui/imgui_draw.cpp
    vendored/imgui/imgui_tables.cpp
    vendored/imgui/imgui_widgets.cpp
)

# ImGui library for launcher (SDL backends)
set(IMGUI_SDL_SOURCES
    ${IMGUI_CORE_SOURCES}
    vendored/imgui/backends/imgui_impl_sdl3.cpp
    vendored/imgui/backends/imgui_impl_sdlrenderer3.cpp
)

add_library(imgui_sdl STATIC ${IMGUI_SDL_SOURCES})
target_include_directories(imgui_sdl PUBLIC 
    vendored/imgui
    vendored/imgui/backends
)
target_link_libraries(imgui_sdl PUBLIC SDL3::SDL3-static)

# ImGui library for hook DLL (DirectX backends)
set(IMGUI_DX_SOURCES
    ${IMGUI_CORE_SOURCES}
    vendored/imgui/backends/imgui_impl_win32.cpp
    vendored/imgui/backends/imgui_impl_dx9.cpp
)

add_library(imgui_dx STATIC ${IMGUI_DX_SOURCES})
target_include_directories(imgui_dx PUBLIC 
    vendored/imgui
    vendored/imgui/backends
)
target_link_libraries(imgui_dx PUBLIC d3d9 dwmapi)

# GekkoNet library - Build as static
add_library(GekkoNet STATIC
    vendored/GekkoNet/GekkoLib/src/gekkonet.cpp
    vendored/GekkoNet/GekkoLib/src/backend.cpp
    vendored/GekkoNet/GekkoLib/src/event.cpp
    vendored/GekkoNet/GekkoLib/src/game_session.cpp
    vendored/GekkoNet/GekkoLib/src/spectator_session.cpp
    vendored/GekkoNet/GekkoLib/src/stress_session.cpp
    vendored/GekkoNet/GekkoLib/src/input.cpp
    vendored/GekkoNet/GekkoLib/src/net.cpp
    vendored/GekkoNet/GekkoLib/src/player.cpp
    vendored/GekkoNet/GekkoLib/src/storage.cpp
    vendored/GekkoNet/GekkoLib/src/sync.cpp
)

target_include_directories(GekkoNet PUBLIC
    vendored/GekkoNet/GekkoLib/include
    vendored/GekkoNet/GekkoLib/include/private  # b5c6528: internal headers moved here; src does #include "backend.h"
    vendored/GekkoNet/GekkoLib/thirdparty
    vendored/GekkoNet/GekkoLib/thirdparty/zpp
    vendored/GekkoNet/GekkoLib/thirdparty/asio
)

# Windows-specific configuration for GekkoNet
if(WIN32)
    target_link_libraries(GekkoNet PUBLIC ws2_32)
    target_compile_definitions(GekkoNet PUBLIC
        _WIN32_WINNT=0x0601  # Target Windows 7 or later
        WIN32_LEAN_AND_MEAN  # Exclude rarely-used stuff from Windows headers
        GEKKONET_STATIC      # Static linking
    )
endif()

# miniupnpc — UPnP-IGD client (Phase 1 NAT reachability, the "hero" path).
# Hand-rolled static lib over the upstream submodule's C sources rather than
# add_subdirectory: upstream's CMakeLists also builds a SHARED lib, the
# upnpc/listdevices sample executables, the test suite, the python module,
# and install rules -- none of which we want. We mirror exactly the
# MINIUPNPC_SOURCES list from vendored/miniupnp/miniupnpc/CMakeLists.txt
# (the canonical library set; the test*.c / sample / module .c files in
# src/ are deliberately excluded). Pinned to tag miniupnpc_2_3_3.
#
# miniupnpcstrings.h is normally configure_file-generated by upstream's
# build (it bakes OS_STRING + version). We generate it the same way from
# the upstream template into the build dir and add that dir to the include
# path, so we don't carry a hand-edited copy that can drift from the pin.
set(MINIUPNPC_DIR vendored/miniupnp/miniupnpc)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/${MINIUPNPC_DIR}/miniupnpcstrings.h.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/miniupnpc_gen/miniupnpcstrings.h
    @ONLY)

add_library(miniupnpc STATIC
    ${MINIUPNPC_DIR}/src/igd_desc_parse.c
    ${MINIUPNPC_DIR}/src/miniupnpc.c
    ${MINIUPNPC_DIR}/src/minixml.c
    ${MINIUPNPC_DIR}/src/minisoap.c
    ${MINIUPNPC_DIR}/src/minissdpc.c
    ${MINIUPNPC_DIR}/src/miniwget.c
    ${MINIUPNPC_DIR}/src/upnpcommands.c
    ${MINIUPNPC_DIR}/src/upnpdev.c
    ${MINIUPNPC_DIR}/src/upnpreplyparse.c
    ${MINIUPNPC_DIR}/src/upnperrors.c
    ${MINIUPNPC_DIR}/src/connecthostport.c
    ${MINIUPNPC_DIR}/src/portlistingparse.c
    ${MINIUPNPC_DIR}/src/receivedata.c
    ${MINIUPNPC_DIR}/src/addr_is_reserved.c
)

# PUBLIC include dirs + MINIUPNP_STATICLIB propagate to FM2K_PortMapper so
# the wrapper sees the right headers and the dllimport-free declarations.
# MINIUPNP_STATICLIB is REQUIRED on Windows static builds: miniupnpc_declspec.h
# decorates the API with __declspec(dllimport) unless this is defined, which
# would produce __imp_ link errors against a static archive.
target_include_directories(miniupnpc PUBLIC
    ${MINIUPNPC_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/miniupnpc_gen
)
target_compile_definitions(miniupnpc PUBLIC
    MINIUPNP_STATICLIB    # static archive: drop the dllimport decoration
)
if(WIN32)
    # miniupnpc needs winsock + the IP helper API (iphlpapi) for the
    # interface enumeration in its discovery path. getnameinfo / getaddrinfo
    # need XP+ -- _WIN32_WINNT is already set globally below, but assert it
    # here for the lib in isolation too.
    target_link_libraries(miniupnpc PUBLIC ws2_32 iphlpapi)
    target_compile_definitions(miniupnpc PUBLIC
        _WIN32_WINNT=0x0601  # Windows 7+, matches the rest of the tree
        WIN32_LEAN_AND_MEAN
    )
endif()

# MinHook library
set(MINHOOK_SOURCES
    vendored/minhook/src/buffer.c
    vendored/minhook/src/hook.c
    vendored/minhook/src/trampoline.c
    vendored/minhook/src/hde/hde32.c
    vendored/minhook/src/hde/hde64.c
)

add_library(MinHook STATIC ${MINHOOK_SOURCES})

# Ensure MinHook is built with MinGW
set_target_properties(MinHook PROPERTIES 
    LINKER_LANGUAGE C
    POSITION_INDEPENDENT_CODE ON
)

target_compile_options(MinHook PRIVATE 
    -m32  # Force 32-bit compilation
)

target_include_directories(MinHook PUBLIC
    vendored/minhook/include
    vendored/minhook/src
)

# Windows-specific configuration for MinHook
target_compile_definitions(MinHook PRIVATE
    _WIN32_WINNT=0x0601  # Target Windows 7 or later
    WIN32_LEAN_AND_MEAN  # Exclude rarely-used stuff from Windows headers
)

# Add FM2KHook subdirectory
add_subdirectory(FM2KHook)

# Set FM2KHook output properties — both engines (FM2KHook + FM95Hook) ship
# alongside the launcher exe so it can pick the right DLL per game.engine.
set_target_properties(FM2KHook FM95Hook PROPERTIES
    PREFIX ""  # Remove "lib" prefix
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)

# Strip libwinpthread.a's `.rsrc` section so its VS_VERSION_INFO doesn't
# get merged into our final binaries alongside the one from launcher.rc /
# FM2KHook resource scripts. ld merges every input archive's RT_VERSION
# resource into the output PE; with two blocks present at resource ID 1
# Windows reads libwinpthread's first ("POSIX WinThreads for Windows")
# and that string leaks into Explorer details, taskbar tooltips, and
# Shell_NotifyIcon toast titles.
#
# Configure-time fix: `objcopy --remove-section=.rsrc` operates directly
# on a static archive, dropping the `.rsrc` section from every member
# object without touching any other section. We link --whole-archive
# against the cleaned copy so all libwinpthread symbols resolve from it,
# and the system -lwinpthread is then a no-op (no unresolved symbols
# left for it to satisfy → its `.rsrc` never gets pulled in).
execute_process(
    COMMAND ${CMAKE_C_COMPILER} -print-file-name=libwinpthread.a
    OUTPUT_VARIABLE SYS_LIBWINPTHREAD
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
get_filename_component(SYS_LIBWINPTHREAD "${SYS_LIBWINPTHREAD}" REALPATH)
set(CLEAN_LIBWINPTHREAD "${CMAKE_BINARY_DIR}/libwinpthread_no_rsrc.a")
add_custom_command(
    OUTPUT  "${CLEAN_LIBWINPTHREAD}"
    DEPENDS "${SYS_LIBWINPTHREAD}"
    COMMAND ${CMAKE_COMMAND} -E copy "${SYS_LIBWINPTHREAD}" "${CLEAN_LIBWINPTHREAD}"
    COMMAND ${CMAKE_OBJCOPY} --remove-section=.rsrc "${CLEAN_LIBWINPTHREAD}"
    # `version.o` inside libwinpthread.a is *only* a .rsrc container —
    # stripping its sole section leaves an empty/invalid object that ld
    # rejects ("member ... is not an object"). It contributes only the
    # VERSIONINFO resource we're trying to suppress, so dropping the
    # member entirely from the archive is the right move. `ar d` is
    # silent on missing members so this is safe across libwinpthread
    # versions where the file might be named differently or absent.
    COMMAND ${CMAKE_AR} d "${CLEAN_LIBWINPTHREAD}" version.o || true
    COMMENT "Stripping .rsrc from libwinpthread.a -> ${CLEAN_LIBWINPTHREAD}"
    VERBATIM
)
add_custom_target(libwinpthread_no_rsrc DEPENDS "${CLEAN_LIBWINPTHREAD}")

# Add pthread static linking for FM2KHook + FM95Hook
target_link_options(FM2KHook PRIVATE
    -static
    -static-libgcc
    -static-libstdc++
    -pthread
    -Wl,-Bstatic,--whole-archive "${CLEAN_LIBWINPTHREAD}" -Wl,--no-whole-archive
)
add_dependencies(FM2KHook libwinpthread_no_rsrc)
target_link_options(FM95Hook PRIVATE
    -static
    -static-libgcc
    -static-libstdc++
    -pthread
    -Wl,-Bstatic,--whole-archive "${CLEAN_LIBWINPTHREAD}" -Wl,--no-whole-archive
)
add_dependencies(FM95Hook libwinpthread_no_rsrc)

# Main executable
# Pre-build version stamper. Writes version_local.h with the current
# semver code + git SHA + build timestamp. Re-runs every build so the
# binary always knows its own version (used by the auto-updater to
# decide if a published release is newer).
add_custom_target(fm2k_version
    COMMAND ${CMAKE_COMMAND} -E env bash
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/make_version.sh"
    BYPRODUCTS "${CMAKE_CURRENT_SOURCE_DIR}/version_local.h"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Stamping version_local.h"
    VERBATIM
)

# Bake the hub log-upload shared secret into auto_upload_secret.h at
# configure time. CI / dev shells set FM2K_LOG_UPLOAD_SECRET in the
# environment before invoking cmake; if it's missing, the file still
# gets generated with an empty string and the launcher auto-disables
# the uploader. URL is overridable too (defaults to the prod hub).
set(FM2K_LOG_UPLOAD_SECRET "$ENV{FM2K_LOG_UPLOAD_SECRET}")
set(FM2K_LOG_UPLOAD_URL "$ENV{FM2K_LOG_UPLOAD_URL}")
if(FM2K_LOG_UPLOAD_URL STREQUAL "")
    set(FM2K_LOG_UPLOAD_URL "https://hub.2dfm.org/logs/upload")
endif()
if(FM2K_LOG_UPLOAD_SECRET STREQUAL "")
    message(WARNING
        "FM2K_LOG_UPLOAD_SECRET not set — log auto-upload will be DISABLED in this build. "
        "Set it before configure to enable.")
endif()
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/auto_upload_secret.h.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/auto_upload_secret.h"
    @ONLY
)

# windres compiles launcher.rc / updater.rc, but CMake can't see the
# `1 24 "app.manifest"` include inside the .rc, so editing app.manifest
# alone would NOT retrigger the resource recompile (stale .res relinked,
# the SxS/manifest fix silently doesn't ship). Declare the dependency
# explicitly. version_rc.h is regenerated every build by make_version.sh
# so it doesn't need listing here.
set_source_files_properties(launcher.rc updater.rc
    PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")

add_executable(FM2K_RollbackLauncher
    # launcher.rc must be FIRST so its compiled .res is the first
    # resource-bearing object on the link line. MinGW's libwinpthread.a
    # ships its own VS_VERSION_INFO ("POSIX WinThreads for Windows") and
    # ld merges every input's RT_VERSION resource into the final EXE.
    # Windows reads whichever block appears first in the resource
    # directory — putting ours first makes it win without surgery.
    launcher.rc
    # Main launcher files
    launcher/core/FM2K_RollbackClient.cpp
    launcher/core/launcher_log.cpp  # always-on launcher.log disk sink + crash breadcrumb (next to the EXE)
    launcher/discovery/game_discovery.cpp  # scan + engine-sniff + async discovery worker (split)
    launcher/discovery/game_discovery_cache.cpp  # games.cache binary serialization + path canon (split)
    launcher/discovery/game_discovery_sniff.cpp  # exe/engine identification: xxhash + registry + PE sniff (split)
    launcher/core/process_manager.cpp  # game-instance spawn/terminate + per-instance config (split from RollbackClient)
    launcher/core/session_control.cpp  # offline/stress/online session start + stop (split from RollbackClient)
    launcher/core/launcher_callbacks.cpp  # ui_->on_* callback wiring / WireUICallbacks (split from RollbackClient)
    launcher/core/launcher_init.cpp  # FM2KLauncher ctor/dtor + Initialize/InitializeSDL/Shutdown (split from RollbackClient)
    launcher/core/launcher_frame.cpp  # HandleEvent/Update/Render + small setters (split from RollbackClient)
    launcher/core/launcher_cli.cpp  # CLI parse + headless probes + replay/stress/offline/direct launch modes (split from RollbackClient)
    launcher/core/FM2K_GameInstance.cpp  # process mgmt: lifecycle + spawn + DLL injection (split)
    launcher/core/FM2K_GameInstance_ipc.cpp  # runtime IPC: rollback ops + DLL-event pump + shared mem + debug cmds (split)
    launcher/session/OnlineSession.cpp
    launcher/session/LocalSession.cpp
    launcher/ui/FM2K_LauncherUI.cpp
    launcher/ui/launcher_ui_settings_io.cpp  # shared dev-flags/game-patches/settings.ini persistence (namespace lui; split from LauncherUI)
    launcher/ui/launcher_ui_debug.cpp  # debug/dev tools, multi-client + network test tools, console log (split from LauncherUI)
    launcher/ui/launcher_ui_display.cpp  # cnc-ddraw display settings panel (split from LauncherUI)
    launcher/ui/launcher_ui_menubar.cpp  # top menu bar (split from LauncherUI)
    launcher/ui/launcher_ui_settings.cpp  # settings/host-config/discord/games-folders windows (split from LauncherUI)
    launcher/ui/launcher_ui_gameselect.cpp  # game picker + replay browser + direct-spectate (split from LauncherUI)
    launcher/ui/launcher_ui_netcfg.cpp  # network config + connection status + session controls (split from LauncherUI)
    launcher/ui/launcher_ui_input.cpp  # input-bindings tab + SOCD/random-stage state (split from LauncherUI)
    launcher/ui/launcher_ui_notify.cpp  # notifications + audio-mute state (split from LauncherUI)
    launcher/ui/launcher_ui_hub.cpp  # hub helpers + server/recent/in-progress lists (split from LauncherUI)
    launcher/ui/launcher_ui_hub_match.cpp  # match-result reporting + outcome/upload polling + stats push (split from hub)
    launcher/ui/launcher_ui_hub_events.cpp  # HandleHubEvent: hub WS event dispatch (split from RenderHubPanel)
    launcher/ui/launcher_ui_hub_events_match.cpp  # HandleMatchStartEvent: 358-line K::MatchStart handler (split)
    launcher/ui/launcher_ui_hub_panel.cpp  # RenderHubPanel: lobby panel rendering (split from hub)
    launcher/hub/FM2K_HubClient.cpp  # core: ctor/dtor + Connect/Disconnect/Poll/queue (split)
    launcher/hub/FM2K_HubClient_json.cpp  # minimal JSON encode/decode helpers (split)
    launcher/hub/FM2K_HubClient_outbound.cpp  # Send*/Join/Challenge/MatchResult senders (split)
    launcher/hub/FM2K_HubClient_transport.cpp  # WinHTTP WS IoThread + CleanupHandles (split)
    launcher/hub/FM2K_HubClient_dispatch.cpp  # OnMessage inbound dispatch (split)
    # UPnP-IGD automatic port mapping (Phase 1 NAT reachability). Launcher-
    # only: wraps the miniupnpc static lib, maps the game's UDP port on the
    # router when the hub WS connects so a peer can reach us directly.
    launcher/net/FM2K_PortMapper.cpp
    launcher/net/FM2K_DiscordAuth.cpp
    launcher/game/FM2K_Locale.cpp
    launcher/net/FM2K_Updater.cpp
    launcher/game/FM2K_GameIni.cpp
    launcher/render/FM2K_DDrawRedirect.cpp
    launcher/render/FM2K_CncDDraw.cpp
    launcher/game/FM2K_KgtParser.cpp
    launcher/net/FM2K_UploadQueue.cpp
    # Stock miniz (public domain) for in-process zip extraction. SDL_image
    # ships its own statically-scoped copy with archive APIs disabled, so
    # this independent copy gives us mz_zip_reader_* without disturbing
    # the SDL_image build.
    vendored/miniz/miniz.c
    launcher/session/LocalNetworkAdapter.cpp
    launcher/core/FM2K_Integration.h
    # ImGui-based input binder (shared with the hook overlay) — depends only
    # on SDL3 + ImGui, no project-specific includes. Split 5-way: core +
    # gamepads + profiles + sample + ui (ImGui render isolated for Slint).
    FM2KHook/src/ui/input_binder.cpp
    FM2KHook/src/ui/input_binder_gamepads.cpp
    FM2KHook/src/ui/input_binder_profiles.cpp
    FM2KHook/src/ui/input_binder_sample.cpp
    FM2KHook/src/ui/input_binder_ui.cpp
    # Shared PII scrubber — used by both the launcher's SDLCustomLogOutput
    # and the hook DLL's LogOutputFunction so anything users share (log
    # window copy, on-disk hook log) is pre-redacted.
    FM2KHook/src/util/pii_scrub.cpp
    # Spec hub-relay shared-memory queue (Phase 2c). Launcher consumes
    # the outbound side (hook -> launcher direction); same .cpp is
    # also compiled into FM2KHook for the producer side. Different
    # binaries, isolated function symbols, no link-time clash.
    FM2KHook/src/netplay/spec_relay_queue.cpp
    # DLL injection approach - hooks are in FM2KHook DLL
)

# Static linking flags for MinGW-GCC
target_link_options(FM2K_RollbackLauncher PRIVATE
    -static
    -static-libgcc
    -static-libstdc++
    -pthread
    # Use the .rsrc-stripped libwinpthread (see config above) so its
    # VS_VERSION_INFO doesn't conflict with our launcher.rc one.
    -Wl,-Bstatic,--whole-archive "${CLEAN_LIBWINPTHREAD}" -Wl,--no-whole-archive
)

# Add FM2KHook as dependency
add_dependencies(FM2K_RollbackLauncher FM2KHook fm2k_version libwinpthread_no_rsrc)

# FM2KUpdater.exe — companion process spawned by the launcher to
# perform the actual EXE/DLL replacement after launcher exit. Tiny
# binary, zero deps beyond Win32. See tools/FM2KUpdater.cpp.
add_executable(FM2KUpdater tools/FM2KUpdater.cpp updater.rc)
target_link_options(FM2KUpdater PRIVATE
    -static
    -static-libgcc
    -static-libstdc++
)
add_dependencies(FM2K_RollbackLauncher FM2KUpdater)

target_link_libraries(FM2K_RollbackLauncher PRIVATE
    GekkoNet
    # miniupnpc + iphlpapi for FM2K_PortMapper. miniupnpc is PUBLIC-marked
    # with MINIUPNP_STATICLIB + its include dirs, so the wrapper compiles
    # against the right (non-dllimport) declarations automatically.
    miniupnpc
    iphlpapi
    SDL3::SDL3-static
    SDL3_image::SDL3_image-static
    SDL3_net::SDL3_net-static
    MinHook
    imgui_sdl
    psapi
    ws2_32
    winmm
    gdi32
    user32
    shell32
    ole32
    uuid          # CLSID_FileOpenDialog / IID_IFileOpenDialog GUIDs (native folder picker)
    setupapi
    version
    imm32
    oleaut32
    wininet
    winhttp
    # XInput for input_binder's Sample_Win32 path. Compiled into the
    # launcher EXE too even though SDL3-Sample() is the default there;
    # unifying lets the launcher fall back if SDL3 events stop pumping.
    xinput9_1_0
)

# Preprocessor definitions
target_compile_definitions(FM2K_RollbackLauncher PRIVATE
    GEKKONET_STATIC
    SDL_STATIC_LIB
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

# Installation
install(TARGETS FM2K_RollbackLauncher
    RUNTIME DESTINATION bin
)

# Documentation
install(FILES
    README.md
    docs/FM2K_Rollback_Research.md
    docs/FM2K_Rollback_Plan.md
    docs/FM2K_Rollback_TestPlan.md
    docs/july7-2025.md
    DESTINATION docs
)

# Print configuration summary
message(STATUS "")
message(STATUS "FM2K Rollback Launcher Configuration Summary:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Install Prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Dependencies:")
message(STATUS "    - SDL3: ${SDL_VERSION}")
message(STATUS "    - SDL3_image: ${SDL3IMAGE_VERSION}")
message(STATUS "    - MinHook: Vendored")
message(STATUS "    - GekkoNet: Vendored")
message(STATUS "    - ImGui: Vendored")
message(STATUS "")