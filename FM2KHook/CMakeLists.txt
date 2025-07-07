cmake_minimum_required(VERSION 3.15)

# FM2K Hook DLL - must be 32-bit
project(FM2KHook)

# Force 32-bit build
set(CMAKE_GENERATOR_PLATFORM Win32)

# Create DLL target
add_library(FM2KHook SHARED
    src/dllmain.cpp
    src/fm2k_hook.cpp
    src/fm2k_hook.h
    src/ipc.cpp
    src/ipc.h
)

# Export symbols for DLL
target_compile_definitions(FM2KHook PRIVATE 
    FM2KHOOK_EXPORTS 
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
)

# Link against MinHook
target_link_libraries(FM2KHook PRIVATE minhook)

# Include directories
target_include_directories(FM2KHook PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/vendored/minhook/include
) 