cmake_minimum_required(VERSION 3.16)

project(redis_lite
    VERSION 0.1.0
    DESCRIPTION "A Redis-inspired in-memory key-value database, built from scratch in C++"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Debug by default: without a build type there is no -g, which makes the
# debugger useless.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Lets editors and clangd understand the project.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Development warnings, applied to everything we build. GCC and Clang only;
# add an MSVC branch (/W4) if this ever needs to build on Windows.
add_compile_options(-Wall -Wextra -Wpedantic)

# The server is single-threaded again (Stage 7), but the tests still exercise
# the store from several threads, so they need the platform threading library.
find_package(Threads REQUIRED)

# The server.
add_executable(redis-lite-server
    src/main.cpp
    src/server.cpp
    src/resp.cpp
    src/commands.cpp
    src/persistence.cpp)
target_include_directories(redis-lite-server PRIVATE include)

# The tests. Plain executable; a non-zero exit code means failure, which is all
# CTest looks at.
enable_testing()
add_executable(redis-lite-tests
    tests/tests.cpp
    src/resp.cpp
    src/commands.cpp
    src/persistence.cpp)
target_include_directories(redis-lite-tests PRIVATE include src)
target_link_libraries(redis-lite-tests PRIVATE Threads::Threads)
add_test(NAME redis-lite-tests COMMAND redis-lite-tests)

# Stage 1 experiments: standalone TCP programs for learning the socket API.
# Deliberately not linked into the server above, and they use no project
# headers -- they are self-contained experiments.
add_executable(tcp-echo-server experiments/tcp_echo_server.cpp)
add_executable(tcp-echo-client experiments/tcp_echo_client.cpp)
