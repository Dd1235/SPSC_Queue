// The lock-free win: SPSCQueue vs a std::mutex + std::queue baseline, same
// capacity and workload. Reports the speedup factor.
#include "bench_common.hpp"
#include "spsc/spsc_queue.hpp"

#include <cstdint>

int main() {
    constexpr std::uint64_t kCount = 20'000'000;
    constexpr std::size_t kCapacity = 1024;

    spsc::SPSCQueue<std::uint64_t> lockfree(kCapacity);
    bench::MutexQueue<std::uint64_t> mutexed(kCapacity);

    double lf = bench::throughput_mops(lockfree, kCount);
    double mx = bench::throughput_mops(mutexed, kCount);

    std::printf("lock-free SPSC : %7.1f Mops/s\n", lf);
    std::printf("mutex + queue  : %7.1f Mops/s\n", mx);
    std::printf("speedup        : %7.1fx\n", lf / mx);
    return 0;
}
