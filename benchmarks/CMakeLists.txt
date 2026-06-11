find_package(Threads REQUIRED)

# Benchmarks are meaningless without optimization; force -O3 here regardless of
# the build type so `cmake --build` always produces representative numbers.
function(add_bench name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE spsc::queue Threads::Threads)
    target_compile_options(${name} PRIVATE -O3 -Wall -Wextra)
endfunction()

add_bench(bench_throughput)
add_bench(bench_batch)
add_bench(bench_vs_mutex)
add_bench(bench_false_sharing)
add_bench(bench_cached_index)
add_bench(bench_latency)

# Head-to-head against the reference SPSC implementations. Off by default so
# the normal build needs no network; turn on with -DSPSC_BENCH_THIRDPARTY=ON.
option(SPSC_BENCH_THIRDPARTY "Fetch rigtorp/moodycamel and build bench_vs_libs" OFF)
if(SPSC_BENCH_THIRDPARTY)
    include(FetchContent)
    FetchContent_Declare(rigtorp_spsc
        GIT_REPOSITORY https://github.com/rigtorp/SPSCQueue.git
        GIT_TAG        v1.1)
    FetchContent_Declare(moodycamel_rwq
        GIT_REPOSITORY https://github.com/cameron314/readerwriterqueue.git
        GIT_TAG        v1.0.7)
    FetchContent_MakeAvailable(rigtorp_spsc moodycamel_rwq)

    add_bench(bench_vs_libs)
    # Both are header-only; include their source dirs directly so we do not
    # depend on the exact target names each project exports.
    target_include_directories(bench_vs_libs PRIVATE
        ${rigtorp_spsc_SOURCE_DIR}/include
        ${moodycamel_rwq_SOURCE_DIR})
endif()
