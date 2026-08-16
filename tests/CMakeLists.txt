# Tests build: native Linux gcc, no SDL / no Windows. Tests pin the wire
# format and queue mechanics of replay + spectator code without requiring
# the full mingw cross-compile + Wine round-trip every iteration.

cmake_minimum_required(VERSION 3.20)
# C is enabled too: reliable_channel_test links vendored/reliable/reliable.c.
# With a CXX-only project() that .c source is silently dropped and the link
# fails on every reliable_* symbol.
project(fm2k_tests C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(fm2k_tests
    test_main.cpp
    test_replay_format.cpp
    test_spectator_protocol.cpp
    test_spectator_protocol_sessionfile.cpp
    test_spectator_queue.cpp
    test_snapshot_coverage.cpp
)

target_include_directories(fm2k_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendored/doctest
)

target_compile_options(fm2k_tests PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-invalid-offsetof  # offsetof on non-POD is fine for our pinned layouts
)

# Two standalone tests that predate this CMakeLists and each carry their own
# main(), so they cannot join the doctest binary. They were previously built
# only by hand-copying a g++ line out of their header comments, which meant
# nothing ran them -- wire them in as their own targets instead.

# Input-delay negotiation math (#24). Header-only dependency on the hook.
add_executable(delay_math_test delay_math_test.cpp)
target_compile_options(delay_math_test PRIVATE -Wall -Wextra -Wpedantic)

# ReliableChannel ordering + FEC reconstruction, no game / no Windows.
# Its body is guarded by RC_STANDALONE_TEST.
add_executable(reliable_channel_test
    ../FM2KHook/src/netplay/reliable_channel_test.cpp
    ../FM2KHook/src/netplay/reliable_channel_liveness_test.cpp
    ../FM2KHook/src/netplay/reliable_channel.cpp
    ../FM2KHook/src/netplay/reliable_channel_fec.cpp
    ../FM2KHook/src/netplay/reliable_channel_liveness.cpp
    ../vendored/reliable/reliable.c
)
target_compile_definitions(reliable_channel_test PRIVATE RC_STANDALONE_TEST)
target_include_directories(reliable_channel_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/shims        # SDL3/SDL_log.h stub, test-only
    ${CMAKE_CURRENT_SOURCE_DIR}/../vendored/reliable
    ${CMAKE_CURRENT_SOURCE_DIR}/../FM2KHook/src/netplay
)

enable_testing()
add_test(NAME fm2k_tests COMMAND fm2k_tests)
add_test(NAME delay_math_test COMMAND delay_math_test)
add_test(NAME reliable_channel_test COMMAND reliable_channel_test)
