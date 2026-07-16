find_package(Threads REQUIRED)

# Benchmarks are meaningless without optimization; force -O3 here regardless of
# the build type so `cmake --build` always produces representative numbers.
function(add_bench name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE spsc::queue Threads::Threads)
    if(MSVC)
        target_compile_options(${name} PRIVATE /O2 /W4)
    else()
        target_compile_options(${name} PRIVATE -O3 -Wall -Wextra)
    endif()
endfunction()

add_bench(bench_throughput)
add_bench(bench_batch)
add_bench(bench_vs_mutex)
add_bench(bench_false_sharing)
add_bench(bench_cached_index)
add_bench(bench_latency)

# The MPMC study's measurement binary (scripts/run_matrix.py drives it).
add_bench(bench_mpmc)

if(SPSC_BUILD_TESTS)
    # 0.01 s * 10,000 scheduled messages/s = exactly 100 retained observations.
    add_test(NAME bench_latency_exact_samples COMMAND bench_mpmc
        --queue spsc --producers 1 --consumers 1 --mode latency
        --seconds 0.01 --rate 10000 --capacity 64)
    set_tests_properties(bench_latency_exact_samples PROPERTIES
        PASS_REGULAR_EXPRESSION "100 ops / 0.01s"
        TIMEOUT 10)

    add_test(NAME bench_reject_unknown_queue COMMAND bench_mpmc --queue unknown)
    add_test(NAME bench_reject_unknown_mode COMMAND bench_mpmc --mode unknown)
    add_test(NAME bench_reject_unknown_qos COMMAND bench_mpmc --qos unknown)
    set_tests_properties(
        bench_reject_unknown_queue bench_reject_unknown_mode bench_reject_unknown_qos
        PROPERTIES WILL_FAIL TRUE TIMEOUT 2)
endif()

# Instrumented twin: same source with mechanism counters compiled in. Used for
# the mechanism dataset only, never for headline numbers (perturbation).
add_executable(bench_mpmc_stats bench_mpmc.cpp)
target_link_libraries(bench_mpmc_stats PRIVATE spsc::queue Threads::Threads)
if(MSVC)
    target_compile_options(bench_mpmc_stats PRIVATE /O2 /W4)
else()
    target_compile_options(bench_mpmc_stats PRIVATE -O3 -Wall -Wextra)
endif()
target_compile_definitions(bench_mpmc_stats PRIVATE SPSC_QUEUE_STATS)

# Head-to-head against the reference implementations. Off by default so the
# normal build needs no network. Commit hashes (rather than movable tags) make
# a configured study build auditable and repeatable.
if(SPSC_BENCH_THIRDPARTY)
    include(FetchContent)
    FetchContent_Declare(rigtorp_spsc
        GIT_REPOSITORY https://github.com/rigtorp/SPSCQueue.git
        GIT_TAG        565a5149d54930463d58cb0f69b978d439555e66)
    FetchContent_Declare(moodycamel_rwq
        GIT_REPOSITORY https://github.com/cameron314/readerwriterqueue.git
        GIT_TAG        8b2176698e9bdaba653cdc20c32b54737a934b47)
    FetchContent_Declare(moodycamel_cq
        GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
        GIT_TAG        6dd38b8a1dbaa7863aa907045f32308a56a6ff5d)
    FetchContent_Declare(rigtorp_mpmc
        GIT_REPOSITORY https://github.com/rigtorp/MPMCQueue.git
        GIT_TAG        11ffb3cd77998cba83c20d2853720be1625cd719) # v1.0
    FetchContent_Declare(xenium
        GIT_REPOSITORY https://github.com/mpoeter/xenium.git
        GIT_TAG        1c449ae953ce2a440b0d16c5ed1181d2754860ab # HEAD 2026-07
        # Header-only usage: do NOT configure xenium's own project (it
        # registers a gtest ctest entry against a binary we never build).
        SOURCE_SUBDIR  use-include-dir-only)
    FetchContent_MakeAvailable(rigtorp_spsc moodycamel_rwq moodycamel_cq
                               rigtorp_mpmc xenium)

    add_bench(bench_vs_libs)
    # Both are header-only; include their source dirs directly so we do not
    # depend on the exact target names each project exports.
    target_include_directories(bench_vs_libs PRIVATE
        ${rigtorp_spsc_SOURCE_DIR}/include
        ${moodycamel_rwq_SOURCE_DIR})

    # bench_mpmc (and its stats twin) gain the industrial reference arms:
    # moodycamel CQ, rigtorp's ticket/turn MPMCQueue, and xenium's
    # Michael-Scott queue with hazard-pointer reclamation (the
    # reclamation-scheme cross-check for F8).
    foreach(tgt bench_mpmc bench_mpmc_stats)
        target_compile_definitions(${tgt} PRIVATE
            SPSC_HAVE_MOODYCAMEL SPSC_HAVE_RIGTORP_MPMC SPSC_HAVE_XENIUM)
        target_include_directories(${tgt} PRIVATE
            ${moodycamel_cq_SOURCE_DIR}
            ${rigtorp_mpmc_SOURCE_DIR}/include
            ${xenium_SOURCE_DIR})
    endforeach()
endif()
