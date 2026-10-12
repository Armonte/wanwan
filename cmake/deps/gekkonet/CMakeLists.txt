# Prebuilt vendored/GekkoNet (our patched copy) for scripts/build_deps.sh. Same
# sources, include dirs and defines as the in-tree target in the top-level
# CMakeLists.txt; installs the static lib + the public header only, so the main
# build never stats GekkoNet's private/asio headers.
cmake_minimum_required(VERSION 3.24)
project(gekkonet_prebuilt CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(G ${CMAKE_CURRENT_LIST_DIR}/../../../vendored/GekkoNet/GekkoLib)
add_library(GekkoNet STATIC
    ${G}/src/gekkonet.cpp ${G}/src/backend.cpp ${G}/src/event.cpp ${G}/src/game_session.cpp
    ${G}/src/spectator_session.cpp ${G}/src/stress_session.cpp ${G}/src/input.cpp ${G}/src/net.cpp
    ${G}/src/player.cpp ${G}/src/storage.cpp ${G}/src/sync.cpp)
target_include_directories(GekkoNet PRIVATE
    ${G}/include ${G}/include/private ${G}/thirdparty ${G}/thirdparty/zpp ${G}/thirdparty/asio)
target_compile_definitions(GekkoNet PRIVATE _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN GEKKONET_STATIC)
install(TARGETS GekkoNet ARCHIVE DESTINATION lib)
install(FILES ${G}/include/gekkonet.h DESTINATION include)
