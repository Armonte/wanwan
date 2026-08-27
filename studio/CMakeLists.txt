# 2dfm Studio -- standalone ImGui + SDL3 shell (studio/app), built as a
# single static i686 Windows exe with the repo's mingw toolchain.
# Independent of the root CMakeLists: reuses vendored/SDL + vendored/imgui
# directly, configures into studio/build (see studio/build.sh).
cmake_minimum_required(VERSION 3.20)

project(2dfm_studio VERSION 0.1.0 LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Cross checks must not try to link host executables (mirrors root).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

get_filename_component(WANWAN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# SDL3 static -- same cache flags the root CMakeLists forces.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(SDL_SHARED OFF CACHE BOOL "Build SDL3 as shared library" FORCE)
set(SDL_STATIC ON CACHE BOOL "Build SDL3 as static library" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "Build SDL3 test library" FORCE)

add_subdirectory("${WANWAN_ROOT}/vendored/SDL"
                 "${CMAKE_BINARY_DIR}/vendored_sdl" EXCLUDE_FROM_ALL)

# ImGui (docking branch) + SDL3/SDLRenderer3 backends, compiled directly
# from the vendored sources -- same set as the root's imgui_sdl target.
add_library(imgui_sdl STATIC
    "${WANWAN_ROOT}/vendored/imgui/imgui.cpp"
    "${WANWAN_ROOT}/vendored/imgui/imgui_demo.cpp"
    "${WANWAN_ROOT}/vendored/imgui/imgui_draw.cpp"
    "${WANWAN_ROOT}/vendored/imgui/imgui_tables.cpp"
    "${WANWAN_ROOT}/vendored/imgui/imgui_widgets.cpp"
    "${WANWAN_ROOT}/vendored/imgui/backends/imgui_impl_sdl3.cpp"
    "${WANWAN_ROOT}/vendored/imgui/backends/imgui_impl_sdlrenderer3.cpp"
)
target_include_directories(imgui_sdl PUBLIC
    "${WANWAN_ROOT}/vendored/imgui"
    "${WANWAN_ROOT}/vendored/imgui/backends"
)
target_link_libraries(imgui_sdl PUBLIC SDL3::SDL3-static)

# WIN32 = GUI subsystem; SDL3/SDL_main.h supplies the WinMain shim.
add_executable(2dfm-studio WIN32
    app/main.cpp
    app/sound_table.cpp
    app/sound_detail.cpp
    app/usage_panel.cpp
    app/real_model.cpp
    app/edit_session.cpp
    app/audio_player.cpp
    core/kgt_file.cpp
    core/xref.cpp
    core/sprite_decode.cpp
    core/audio/audio_convert.cpp
)
target_compile_definitions(2dfm-studio PRIVATE SDL_STATIC_LIB)
# core/audio/audio_convert.cpp compiles the vendored single-header
# decoders out of studio/third_party (dr_wav/dr_mp3/dr_flac/stb_vorbis).
target_include_directories(2dfm-studio PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
)
target_link_libraries(2dfm-studio PRIVATE
    imgui_sdl
    SDL3::SDL3-static
    # Win32 libs SDL3-static pulls in (mirrors the root launcher link set).
    winmm gdi32 user32 shell32 ole32 uuid setupapi version imm32 oleaut32 ws2_32
)
target_link_options(2dfm-studio PRIVATE
    -static
    -static-libgcc
    -static-libstdc++
    $<$<CONFIG:Release>:-s>   # strip in release
)
