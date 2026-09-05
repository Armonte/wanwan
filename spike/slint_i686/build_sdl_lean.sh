#!/usr/bin/env bash
# A lean SDL3 for the launcher window: video, OpenGL and input, nothing else.
#
# The studio's shared build (studio/build/vendored_sdl) has every subsystem
# on, and build_host.sh reused it because it was already there.
#
# WHERE THE MEGABYTE ACTUALLY COMES FROM, because the obvious answer is
# wrong: NOT the render backends. Direct3D 11/12 and Vulkan are still
# compiled into this archive (SDL_RENDER=OFF does not drop them), but the
# linker never pulls them either way -- nothing references
# SDL_CreateRenderer, since this launcher renders with FemtoVG on the GL
# context it makes itself. Unreferenced objects in a static archive cost
# nothing. The saving is from the subsystems SDL_Init and SDL_Quit DO
# reach: audio, camera, sensor, haptic, power and the file dialogs, whose
# init and shutdown paths are linked in whether or not you use them.
# Measured: 22.7 MB -> 21.7 MB. Gamepads stay -- the input binder reads
# them, and SDL_Init(SDL_INIT_VIDEO) alone still enumerates them.
#
# Writes spike/sdl_lean/libSDL3.a. build_host.sh prefers it when present
# and falls back to the studio's, so this script is optional -- a
# contributor who never runs it still gets a working launcher.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
MAIN="${FM2K_MAIN_ROOT:-$ROOT}"
OUT="$HERE/../sdl_lean"

cmake -S "$MAIN/vendored/SDL" -B "$OUT" -G Ninja \
  -D CMAKE_BUILD_TYPE=Release \
  -D CMAKE_C_COMPILER=i686-w64-mingw32-gcc \
  -D CMAKE_CXX_COMPILER=i686-w64-mingw32-g++ \
  -D CMAKE_ASM_COMPILER=i686-w64-mingw32-gcc \
  -D CMAKE_RC_COMPILER=i686-w64-mingw32-windres \
  -D CMAKE_SYSTEM_NAME=Windows -D CMAKE_SYSTEM_PROCESSOR=i686 \
  -D SDL_DISABLE_WINDOWS_VERSION_RESOURCE=ON \
  -D CMAKE_C_FLAGS="-w -static-libgcc" \
  -D CMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
  -D CMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
  -D CMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
  -D SDL_SHARED=OFF -D SDL_STATIC=ON \
  -D SDL_TEST_LIBRARY=OFF -D SDL_EXAMPLES=OFF -D SDL_TESTS=OFF \
  -D SDL_RENDER=OFF -D SDL_GPU=OFF -D SDL_VULKAN=OFF \
  -D SDL_CAMERA=OFF -D SDL_SENSOR=OFF -D SDL_HAPTIC=OFF -D SDL_DIALOG=OFF \
  -D SDL_AUDIO=OFF -D SDL_POWER=OFF \
  -D SDL_OPENGL=ON -D SDL_OPENGLES=OFF \
  -D SDL_JOYSTICK=ON -D SDL_HIDAPI=ON \
  > /dev/null

cmake --build "$OUT" -j 4 > /dev/null
ls -la --block-size=K "$OUT/libSDL3.a" | awk '{print "lean  libSDL3.a:", $5}'
ls -la --block-size=K "$MAIN/studio/build/vendored_sdl/libSDL3.a" 2>/dev/null \
  | awk '{print "studio libSDL3.a:", $5}'
