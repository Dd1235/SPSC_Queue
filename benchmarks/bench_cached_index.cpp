// A/B test of the cached-index optimization: with caching ON the hot path reads
// a thread-local copy of the other side's index and only issues the contended
// acquire load when that copy says full/empty. With it OFF every push and pop
// issues an acquire load of the other thread's atomic -- a guaranteed cross-core
// coherence read. Padding is held ON for both so this isolates caching alone.
//
// We measure two capacities because the optimization is workload-dependent:
//   - large capacity: the queue spends most time away from full/empty, so the
//     cache hits and we skip the coherence read -> big win.
//   - tiny capacity: the queue hovers at a boundary, so the cached copy keeps
//     missing and refreshing anyway -> the extra branch can even cost a little.
// Being able to explain that crossover is the point.
#include "bench_common.hpp"
#include "spsc/spsc_queue.hpp"

#include <cstdint>

int main() {
    constexpr std::uint64_t kCount = 20'000'000;

    for (std::size_t cap : {16u, 1024u}) {
        spsc::SPSCQueue<std::uint64_t, /*Padded=*/true, /*Cached=*/true> cached(cap);
        spsc::SPSCQueue<std::uint64_t, /*Padded=*/true, /*Cached=*/false> uncached(cap);

        double c = bench::throughput_mops(cached, kCount);
        double u = bench::throughput_mops(uncached, kCount);

        std::printf("capacity=%-4zu  cached: %7.1f Mops/s   uncached: %7.1f Mops/s   speedup: %.2fx\n",
                    cap, c, u, c / u);
    }
    return 0;
}
