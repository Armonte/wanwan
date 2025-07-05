cmake_minimum_required(VERSION 3.20)
project(FM2KFramestep)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find SDL2
find_package(SDL2 REQUIRED)

# Create the executable
add_executable(framestep framestep.cpp)

# Link SDL2
target_link_libraries(framestep PRIVATE SDL2::SDL2 SDL2::SDL2main)

# Windows-specific settings
if(WIN32)
    # Enable console subsystem for printf output
    set_target_properties(framestep PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE"
    )
    
    # Copy SDL2 DLLs to output directory
    add_custom_command(TARGET framestep POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:SDL2::SDL2>
        $<TARGET_FILE_DIR:framestep>
    )
endif()

# Compiler-specific settings
if(MSVC)
    target_compile_options(framestep PRIVATE /W4)
else()
    target_compile_options(framestep PRIVATE -Wall -Wextra -Wpedantic)
endif() 