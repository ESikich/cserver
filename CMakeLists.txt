cmake_minimum_required(VERSION 3.20)
project(cserve C)

add_compile_definitions(_GNU_SOURCE)

# GCC 14+ and Clang 18+ accept -std=c23; older Clang needs -std=c2x
if(CMAKE_C_COMPILER_ID STREQUAL "Clang"
   AND CMAKE_C_COMPILER_VERSION VERSION_LESS "18.0")
    add_compile_options(-std=c2x)
else()
    add_compile_options(-std=c23)
endif()

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wno-unused-parameter
)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-O0 -g3 -fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2)
endif()

# ------------------------------------------------------------------ #
# Main executable                                                     #
# ------------------------------------------------------------------ #

set(CSERVE_SOURCES
    src/main.c
    src/server.c
    src/conn.c
    src/parser.c
    src/router.c
    src/static.c
    src/response.c
    src/log.c
    src/config.c
    src/util.c
)

add_executable(cserve ${CSERVE_SOURCES})
target_include_directories(cserve PRIVATE include)

# ------------------------------------------------------------------ #
# Tests                                                               #
# ------------------------------------------------------------------ #

enable_testing()

add_executable(test_parser
    tests/test_parser.c
    src/parser.c
    src/log.c
)
target_include_directories(test_parser PRIVATE include)

add_executable(test_router
    tests/test_router.c
    src/router.c
    src/parser.c
    src/static.c
    src/response.c
    src/conn.c
    src/util.c
    src/config.c
    src/log.c
)
target_include_directories(test_router PRIVATE include)

add_executable(test_util
    tests/test_util.c
    src/util.c
    src/log.c
)
target_include_directories(test_util PRIVATE include)

add_test(NAME parser COMMAND test_parser)
add_test(NAME router COMMAND test_router)
add_test(NAME util   COMMAND test_util)

# ------------------------------------------------------------------ #
# Fuzz target (opt-in: cmake -DFUZZ=1)                               #
# ------------------------------------------------------------------ #

option(FUZZ "Build AFL++/libFuzzer fuzz targets" OFF)
if(FUZZ)
    add_executable(fuzz_parser
        fuzz/fuzz_parser.c
        src/parser.c
        src/log.c
    )
    target_include_directories(fuzz_parser PRIVATE include)
    target_compile_options(fuzz_parser PRIVATE
        -Wno-extra-semi
        -Wno-gnu-statement-expression-from-macro-expansion
    )
endif()
