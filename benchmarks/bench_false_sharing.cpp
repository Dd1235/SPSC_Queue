// A/B test of the false-sharing fix: identical code, padded (indices on
// separate cache lines) vs packed (indices sharing one line). The packed
// version forces the producer's and consumer's stores onto the same line, so
// every store triggers a cross-core invalidation (MESI ping-pong). Cached
// index is held ON for both so this isolates padding alone.
#include "bench_common.hpp"
#include "spsc/spsc_queue.hpp"

#include <cstdint>

int main() {
    constexpr std::uint64_t kCount = 20'000'000;
    constexpr std::size_t kCapacity = 1024;

    spsc::SPSCQueue<std::uint64_t, /*Padded=*/true, /*Cached=*/true> padded(kCapacity);
    spsc::SPSCQueue<std::uint64_t, /*Padded=*/false, /*Cached=*/true> packed(kCapacity);

    double p = bench::throughput_mops(padded, kCount);
    double k = bench::throughput_mops(packed, kCount);

    std::printf("padded (alignas %zu) : %7.1f Mops/s\n", spsc::kCacheLineSize, p);
    std::printf("packed (shared line) : %7.1f Mops/s\n", k);
    std::printf("false-sharing penalty: %7.1fx\n", p / k);
    return 0;
}
