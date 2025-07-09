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

# Make SDL3 available
FetchContent_MakeAvailable(SDL3)

# Make SDL_image available after SDL3
FetchContent_MakeAvailable(SDL3_image)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/GekkoNet/include
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/imgui/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/vendored/minhook/include
)

# ImGui sources
set(IMGUI_SOURCES
    vendored/imgui/imgui.cpp
    vendored/imgui/imgui_demo.cpp
    vendored/imgui/imgui_draw.cpp
    vendored/imgui/imgui_tables.cpp
    vendored/imgui/imgui_widgets.cpp
    vendored/imgui/backends/imgui_impl_sdl3.cpp
    vendored/imgui/backends/imgui_impl_sdlrenderer3.cpp
)

# GekkoNet library - Build as static
add_library(GekkoNet STATIC
    vendored/GekkoNet/GekkoLib/src/gekkonet.cpp
    vendored/GekkoNet/GekkoLib/src/backend.cpp
    vendored/GekkoNet/GekkoLib/src/event.cpp
    vendored/GekkoNet/GekkoLib/src/gekko.cpp
    vendored/GekkoNet/GekkoLib/src/input.cpp
    vendored/GekkoNet/GekkoLib/src/net.cpp
    vendored/GekkoNet/GekkoLib/src/player.cpp
    vendored/GekkoNet/GekkoLib/src/storage.cpp
    vendored/GekkoNet/GekkoLib/src/sync.cpp
)

target_include_directories(GekkoNet PUBLIC
    vendored/GekkoNet/GekkoLib/include
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

# Set FM2KHook output properties
set_target_properties(FM2KHook PROPERTIES 
    PREFIX ""  # Remove "lib" prefix
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
)

# Add pthread static linking for FM2KHook
target_link_options(FM2KHook PRIVATE
    -static
    -static-libgcc
    -static-libstdc++
    -pthread
    -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive
)

# Main executable
add_executable(FM2K_RollbackLauncher
    # Main launcher files
    FM2K_RollbackClient.cpp
    FM2K_GameInstance.cpp
    OnlineSession.cpp
    LocalSession.cpp
    FM2K_LauncherUI.cpp
    FM2K_GekkoNetBridge.cpp
    FM2K_Integration.h
    FM2K_Hooks.h
    FM2K_DLLInjector.h
    FM2K_DLLInjector.cpp
    
    

    # IPC shared with hook DLL
    FM2KHook/src/ipc.cpp
    FM2KHook/src/ipc.h
    
    # State management shared with hook DLL
    FM2KHook/src/state_manager.cpp
    FM2KHook/src/state_manager.h
    
    # ImGui sources
    ${IMGUI_SOURCES}
)

# Static linking flags for MinGW-GCC
target_link_options(FM2K_RollbackLauncher PRIVATE 
    -static 
    -static-libgcc 
    -static-libstdc++
    -pthread
    -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive
)

# Add FM2KHook as dependency
add_dependencies(FM2K_RollbackLauncher FM2KHook)

target_link_libraries(FM2K_RollbackLauncher PRIVATE
    GekkoNet
    SDL3::SDL3-static
    SDL3_image::SDL3_image-static
    MinHook
    psapi
    ws2_32
    winmm
    gdi32
    user32
    shell32
    ole32
    setupapi
    version
    imm32
    oleaut32
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