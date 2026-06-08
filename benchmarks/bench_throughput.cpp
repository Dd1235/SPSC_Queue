// Headline throughput of the production configuration (padded + cached index).
#include "bench_common.hpp"
#include "spsc/spsc_queue.hpp"

#include <cstdint>

int main() {
    constexpr std::uint64_t kCount = 50'000'000;
    constexpr std::size_t kCapacity = 1024;

    spsc::SPSCQueue<std::uint64_t> q(kCapacity);
    double mops = bench::throughput_mops(q, kCount);

    std::printf("SPSCQueue<uint64_t>  capacity=%zu\n", kCapacity);
    std::printf("throughput: %.1f Mops/s  (%.2f ns/op)\n", mops, 1000.0 / mops);
    return 0;
}
