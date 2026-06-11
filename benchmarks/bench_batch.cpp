// Throughput as a function of batch size. The bulk APIs pay one relaxed load,
// at most one acquire refresh, and one release store per BATCH instead of per
// element -- this benchmark measures what that amortization is worth. batch=1
// uses the single-element ops as the baseline.
#include "bench_common.hpp"
#include "spsc/spsc_queue.hpp"

#include <cstdint>
#include <thread>

static double bulk_mops(std::size_t batch, std::uint64_t count, int reps = 3) {
    auto one_run = [&](std::uint64_t n) {
        spsc::SPSCQueue<std::uint64_t> q(4096);
        auto t0 = bench::Clock::now();
        std::thread consumer([&] {
            std::uint64_t buf[256];
            std::uint64_t got = 0;
            while (got < n) {
                std::size_t m = q.try_pop_bulk(buf, batch);
                got += m;
            }
        });
        std::thread producer([&] {
            std::uint64_t buf[256];
            std::uint64_t next = 0;
            while (next < n) {
                std::size_t want = batch;
                if (next + want > n) want = static_cast<std::size_t>(n - next);
                for (std::size_t i = 0; i < want; ++i) buf[i] = next + i;
                next += q.try_push_bulk(buf, want);
            }
        });
        producer.join();
        consumer.join();
        return bench::seconds_since(t0);
    };

    one_run(count / 10 + 1);  // warmup
    double best = 1e300;
    for (int r = 0; r < reps; ++r) best = std::min(best, one_run(count));
    return (static_cast<double>(count) / best) / 1e6;
}

int main() {
    constexpr std::uint64_t kCount = 50'000'000;

    spsc::SPSCQueue<std::uint64_t> single(4096);
    double base = bench::throughput_mops(single, kCount);
    std::printf("batch=  1 (single ops) : %7.1f Mops/s   1.00x\n", base);

    for (std::size_t batch : {8u, 32u, 128u}) {
        double m = bulk_mops(batch, kCount);
        std::printf("batch=%3zu (bulk ops)   : %7.1f Mops/s   %.2fx\n", batch, m, m / base);
    }
    return 0;
}
