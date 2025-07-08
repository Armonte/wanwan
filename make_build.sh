#!/bin/bash
set -e
set -x

# Check for required packages
if ! command -v i686-w64-mingw32-gcc >/dev/null 2>&1; then
    echo "MinGW-w64 cross compiler not found. Installing required packages..."
    sudo apt update
    sudo apt install -y gcc-mingw-w64-i686 g++-mingw-w64-i686 binutils-mingw-w64-i686 ninja-build
fi

# Create build directory if it doesn't exist
if [ ! -d "build" ]; then
    mkdir build
fi

cd build

# Clean if build.ninja exists
if [ -f "build.ninja" ]; then
    ninja clean
fi

# Force CMake to run
cmake .. -G "Ninja" -D CMAKE_BUILD_TYPE=RelWithDebInfo \
    -D CMAKE_C_COMPILER=i686-w64-mingw32-gcc \
    -D CMAKE_CXX_COMPILER=i686-w64-mingw32-g++ \
    -D CMAKE_ASM_COMPILER=i686-w64-mingw32-gcc \
    -D CMAKE_RC_COMPILER=i686-w64-mingw32-windres \
    -D CMAKE_SYSTEM_NAME=Windows \
    -D CMAKE_SYSTEM_PROCESSOR=i686 \
    -D SDL_DISABLE_WINDOWS_VERSION_RESOURCE=ON \
    -D CMAKE_C_FLAGS="-w -static-libgcc" \
    -D CMAKE_CXX_FLAGS="-w -static-libgcc -static-libstdc++" \
    -D CMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -D CMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -D CMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY

echo "CMake configuration completed. Run 'bash go.sh' to build." 