# Cross toolchain: llvm-mingw (clang) targeting 32-bit Windows. The MSVCRT variant
# by default: msvcrt.dll ships with every Windows, so one exe runs on Windows 7 SP1
# through 11 with no redistributable (UCRT is only built into Windows 10+).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)
if(DEFINED ENV{LLVM_MINGW})
  set(LLVM_MINGW "$ENV{LLVM_MINGW}" CACHE PATH "llvm-mingw root")
else()
  set(LLVM_MINGW "$ENV{HOME}/tools/llvm-mingw-msvcrt" CACHE PATH "llvm-mingw root (msvcrt variant)")
endif()
set(CMAKE_C_COMPILER   ${LLVM_MINGW}/bin/i686-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER ${LLVM_MINGW}/bin/i686-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER  ${LLVM_MINGW}/bin/i686-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH ${LLVM_MINGW}/i686-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
