cmake_minimum_required(VERSION 3.16)
project(spsc_queue LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(SPSC_BUILD_TESTS      "Build the test suite"        ON)
option(SPSC_BUILD_BENCHMARKS "Build the benchmarks"        ON)
option(SPSC_BUILD_DEMOS      "Build the demos"             ON)
option(SPSC_ENABLE_TSAN      "Build with ThreadSanitizer"  OFF)
option(SPSC_ENABLE_ASAN      "Build with Address/UB sanitizer" OFF)

# ---------------------------------------------------------------------------
# The library itself: header-only -> an INTERFACE target.
# ---------------------------------------------------------------------------
add_library(spsc_queue INTERFACE)
add_library(spsc::queue ALIAS spsc_queue)
target_include_directories(spsc_queue INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(spsc_queue INTERFACE cxx_std_17)

# ---------------------------------------------------------------------------
# Sanitizers (applied to consumers via a helper interface target).
# ---------------------------------------------------------------------------
add_library(spsc_sanitizers INTERFACE)
if(SPSC_ENABLE_TSAN)
    target_compile_options(spsc_sanitizers INTERFACE -fsanitize=thread -g -O1)
    target_link_options(spsc_sanitizers INTERFACE -fsanitize=thread)
endif()
if(SPSC_ENABLE_ASAN)
    target_compile_options(spsc_sanitizers INTERFACE -fsanitize=address,undefined -g -O1)
    target_link_options(spsc_sanitizers INTERFACE -fsanitize=address,undefined)
endif()

if(SPSC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(SPSC_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(SPSC_BUILD_DEMOS)
    add_subdirectory(demo)
endif()
