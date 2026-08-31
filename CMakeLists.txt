cmake_minimum_required(VERSION 3.16)

project(redis_lite
    VERSION 0.1.0
    DESCRIPTION "A Redis-inspired in-memory key-value database, built from scratch in modern C++"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Language standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to a Debug build when the user did not pick one (single-config only).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Always emit compile_commands.json for clangd / editor tooling.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Development warnings
#
# Carried by an INTERFACE target so every target opts in explicitly with
# target_link_libraries(<target> PRIVATE redis_lite_warnings).
# ---------------------------------------------------------------------------
add_library(redis_lite_warnings INTERFACE)

if(MSVC)
    target_compile_options(redis_lite_warnings INTERFACE /W4 /permissive-)
else()
    target_compile_options(redis_lite_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wconversion
        -Wsign-conversion
        -Wdouble-promotion
        -Wformat=2)
endif()

# Opt-in: -DREDIS_LITE_WARNINGS_AS_ERRORS=ON (off by default so a new
# compiler version cannot break a plain build).
option(REDIS_LITE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
if(REDIS_LITE_WARNINGS_AS_ERRORS)
    if(MSVC)
        target_compile_options(redis_lite_warnings INTERFACE /WX)
    else()
        target_compile_options(redis_lite_warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# Public headers
#
# Header-only for now; becomes the include path for the real library once
# there is compiled code to put in it.
# ---------------------------------------------------------------------------
add_library(redis_lite_headers INTERFACE)
target_include_directories(redis_lite_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
add_subdirectory(src)
add_subdirectory(benchmarks)

option(REDIS_LITE_BUILD_TESTS "Build the test suite" ON)
if(REDIS_LITE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
