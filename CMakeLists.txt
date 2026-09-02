cmake_minimum_required(VERSION 3.16)

project(redis_lite
    VERSION 0.1.0
    DESCRIPTION "A Redis-inspired in-memory key-value database, built from scratch in C++"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Development warnings, applied to everything we build. GCC and Clang only;
add_compile_options(-Wall -Wextra -Wpedantic)

# The server is single-threaded again (Stage 7), but the tests still exercise
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

add_executable(redis-lite-integration
    tests/integration_test.cpp
    src/resp.cpp)
target_include_directories(redis-lite-integration PRIVATE src)
add_test(NAME redis-lite-integration
         COMMAND redis-lite-integration $<TARGET_FILE:redis-lite-server>)

# Benchmark client.
add_executable(redis-lite-bench
    benchmarks/bench.cpp
    src/resp.cpp)
target_include_directories(redis-lite-bench PRIVATE src)

# Stage 1 experiments: standalone TCP programs for learning the socket API.
add_executable(tcp-echo-server experiments/tcp_echo_server.cpp)
add_executable(tcp-echo-client experiments/tcp_echo_client.cpp)
