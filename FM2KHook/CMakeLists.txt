cmake_minimum_required(VERSION 3.15)

# FM2K Hook DLL - must be 32-bit
project(FM2KHook)

# Force 32-bit build
set(CMAKE_GENERATOR_PLATFORM Win32)

# Enable debug symbols
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")

# Create simple DLL target
add_library(FM2KHook SHARED
    src/dllmain.cpp
    src/globals.cpp
    src/gekkonet_hooks.cpp
    src/logging.cpp
    src/shared_mem.cpp
    src/state_manager.cpp
    src/game_state_detector.cpp
    src/hooks.cpp
    src/input_handler.cpp
    src/savestate.cpp
    src/debug_features.cpp
    src/game_patches.cpp
    src/state_monitor.cpp
    src/css_handler.cpp
    src/object_tracker.cpp
    src/object_analysis.cpp
    src/object_pool_scanner.cpp
    ../LocalNetworkAdapter.cpp
)

# Export symbols for DLL
target_compile_definitions(FM2KHook PRIVATE 
    FM2KHOOK_EXPORTS 
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    GEKKONET_STATIC
    _WIN32_WINNT=0x0601
)

# Link against MinHook and GekkoNet
target_link_libraries(FM2KHook PRIVATE 
    MinHook
    GekkoNet
    SDL3::SDL3-static
    SDL3_net::SDL3_net-static
    ws2_32
    winmm
)

# Include directories
target_include_directories(FM2KHook PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/vendored/minhook/include
    ${CMAKE_SOURCE_DIR}/vendored/SDL/include
    ${CMAKE_SOURCE_DIR}/vendored/SDL_net/include
    ${CMAKE_SOURCE_DIR}/vendored/GekkoNet/include
    ${CMAKE_SOURCE_DIR}/vendored/GekkoNet/GekkoLib/include
)

# Set output directories
set_target_properties(FM2KHook PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# Install rules
install(TARGETS FM2KHook
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
) 