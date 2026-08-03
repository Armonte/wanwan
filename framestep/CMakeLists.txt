cmake_minimum_required(VERSION 3.20)
project(FM2KFramestep)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Standalone side tool -- NOT part of the main build (the root CMakeLists
# only add_subdirectory(FM2KHook)). Configure this directory on its own
# when you actually need the tool.
#
# This used to build framestep.cpp against SDL2 with MSVC-only link flags
# (/SUBSYSTEM:CONSOLE) in a project that is SDL3 + mingw everywhere else,
# so it had been unbuildable for a long time while the SDL3 port sat
# uncompiled next to it. framestep_sdl3.cpp is now the one that builds;
# the SDL2 original is recoverable from git history if ever needed.
# framestep.rs is a separate Rust reference impl, not a build input.

find_package(SDL3 REQUIRED)

add_executable(framestep framestep_sdl3.cpp)

target_link_libraries(framestep PRIVATE SDL3::SDL3)

if(MSVC)
    target_compile_options(framestep PRIVATE /W4)
else()
    target_compile_options(framestep PRIVATE -Wall -Wextra -Wpedantic)
endif()
