cmake_minimum_required(VERSION 3.15)

# FM2K Hook DLL - must be 32-bit
project(FM2KHook)

# Force 32-bit build
set(CMAKE_GENERATOR_PLATFORM Win32)

# Enable debug symbols
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")

# Create the shared library for the hook
add_library(FM2KHook SHARED 
    src/dllmain.cpp
<<<<<<< Updated upstream
    src/sdl3_hooks.cpp
    src/ddraw_hooks.cpp
=======
    src/dummy_directdraw.cpp
    src/surface_manager.cpp
    # Temporarily disabled - SDL3 integration is handled directly in dllmain.cpp
    # src/sdl3_context.cpp
    # src/directdraw_compat.cpp
    # src/window_hooks.cpp
>>>>>>> Stashed changes
)

# Export symbols for DLL
target_compile_definitions(FM2KHook PRIVATE 
    FM2KHOOK_EXPORTS 
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
)

# Link against MinHook, SDL3, and DirectDraw
target_link_libraries(FM2KHook PRIVATE MinHook SDL3::SDL3 ddraw)

# Include directories
target_include_directories(FM2KHook PRIVATE 
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/vendored/minhook/include
    ${CMAKE_SOURCE_DIR}/vendored/SDL/include
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