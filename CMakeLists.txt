cmake_minimum_required(VERSION 3.30)

project(fiber VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "fiber requires the LLVM Clang compiler")
endif()

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FIBER_BUILD_TESTS "Build the test suite" ON)
option(FIBER_BUILD_BENCHMARKS "Build benchmarks" ON)

add_library(fiber_warnings INTERFACE)
target_compile_options(
  fiber_warnings
  INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -Wcast-align
    -Wcast-qual
    -Wconversion
    -Wdocumentation
    -Wdouble-promotion
    -Wextra-semi
    -Wformat=2
    -Wimplicit-fallthrough
    -Wmissing-declarations
    -Wnon-virtual-dtor
    -Wnull-dereference
    -Wold-style-cast
    -Woverloaded-virtual
    -Wshadow-all
    -Wsign-conversion
    -Wswitch-enum
    -Wthread-safety
    -Wundef
    -Wunreachable-code-aggressive
    -Wzero-as-null-pointer-constant
)

# libghostty-vt remains pinned and built from the vendored Ghostty source.
add_subdirectory(vendor/ghostty EXCLUDE_FROM_ALL SYSTEM)

find_package(lua CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)

add_library(
  fiber_lib
  src/assert.cpp
  src/config.cpp
  src/fiber.cpp
  src/mux/single_pane.cpp
  src/vt/terminal.cpp
)
add_library(fiber::lib ALIAS fiber_lib)
target_include_directories(
  fiber_lib
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(
  fiber_lib
  PUBLIC ghostty-vt-static
  PRIVATE fiber_warnings lua::lua zstd::libzstd_static
)

add_executable(fiber src/main.cpp)
target_link_libraries(fiber PRIVATE fiber::lib fiber_warnings)

if(FIBER_BUILD_TESTS)
  enable_testing()
  find_package(GTest CONFIG REQUIRED)
  include(GoogleTest)

  add_executable(
    fiber_tests
    tests/config_test.cpp
    tests/core_test.cpp
    tests/fiber_test.cpp
    tests/terminal_test.cpp
  )
  target_link_libraries(
    fiber_tests
    PRIVATE fiber::lib fiber_warnings GTest::gtest_main GTest::gmock
  )
  gtest_discover_tests(fiber_tests)
endif()

if(FIBER_BUILD_BENCHMARKS)
  find_package(benchmark CONFIG REQUIRED)

  add_executable(fiber_benchmarks benchmarks/fiber_benchmark.cpp)
  target_link_libraries(
    fiber_benchmarks
    PRIVATE fiber::lib fiber_warnings benchmark::benchmark_main
  )
endif()
