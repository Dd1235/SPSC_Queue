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
